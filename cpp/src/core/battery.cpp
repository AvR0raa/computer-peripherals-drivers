// battery.cpp — 电量编排实现（core/battery.py 平移）。
#include "battery.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "vendor_battery.hpp"

namespace pt {

namespace {

// EZ2.4G 实测常用电量 usage 对（标准 HID 之外的厂商习惯用法）+ Battery System 页
constexpr std::pair<int, int> BATTERY_USAGE_PAIRS[] = {
    {0x0006, 0x0020}, {0x000D, 0x003B}, {0x0085, 0x0065}};

}  // namespace

std::optional<int> scale_percent(const ReportField& field, int raw) {
    // 按字段逻辑范围缩放到 0-100；无范围或已近似百分比则原样钳制
    if (field.logical_min && field.logical_max) {
        const int lo = *field.logical_min, hi = *field.logical_max;
        if (hi > lo && !(lo == 0 && hi == 100)) {
            const double scaled = (raw - lo) * 100.0 / (hi - lo);
            return std::clamp(static_cast<int>(std::lround(scaled)), 0, 100);
        }
    }
    if (raw >= 0 && raw <= 100) return std::clamp(raw, 0, 100);
    return raw;
}

namespace {  // 以下为内部编排函数

std::optional<BatteryResult> from_hidpp(const HidppBattery& b) {
    BatteryResult r;
    r.percent = b.percent;
    r.voltage_mv = b.voltage_mv;
    r.source = b.source;
    r.detail = b.detail;
    return r;
}

std::string fmt_version(const std::optional<double>& version) {
    if (!version) return "HID++ None";
    char buf[32];
    std::snprintf(buf, sizeof buf, "HID++ %.1f", *version);
    return buf;
}

bool has_data(const std::optional<BatteryResult>& r) {
    // 扫描结果是否带真实电量数据（没数据的不能短路后续扫描）
    return r && (r->percent || r->voltage_mv);
}

// 对已打开的 HID++ 接口读电量；设备不在线返回空。
std::optional<BatteryResult> hidpp_battery_for(HidppChannel& io, int dev) {
    const auto [state, version] = ping(io, dev);
    if (state == "offline" || state == "empty-slot") return std::nullopt;
    const auto battery = read_battery(io, dev);
    if (!battery) {
        BatteryResult r;
        r.source = "none";
        r.detail = fmt_version(version) + " 设备在线但无电量 feature";
        return r;
    }
    return from_hidpp(*battery);
}

// 罗技：短/长双通道（FF00:0001=短，FF00:0002=长），接收器槽位 ping + 直连 0xFF。
// 注意顺序：0xFF（接收器自身/直连设备）无电量时必须继续扫槽位 1-7 ——
// 接收器自己应答 HID++ 1.0 但没有电量 feature，不能短路。
std::optional<BatteryResult> scan_hidpp(const std::vector<DeviceInfo>& ifaces,
                                        DevFactory& factory) {
    const DeviceInfo* short_if = nullptr;
    const DeviceInfo* long_if = nullptr;
    for (const auto& d : ifaces) {
        if (hex(d.usage_page, 4) == "FF00" && hex(d.usage, 4) == "0001") short_if = &d;
        if (hex(d.usage_page, 4) == "FF00" && hex(d.usage, 4) == "0002") long_if = &d;
    }
    if (!short_if && !long_if) return std::nullopt;
    HidppLink link(short_if ? short_if->path : std::string(),
                   long_if ? long_if->path : std::string(), factory);
    link.open();
    struct LinkCloser {
        HidppLink& l;
        ~LinkCloser() { l.close(); }
    } closer{link};

    // 1) 直连设备（0xFF）
    auto result = hidpp_battery_for(link, 0xFF);
    if (has_data(result)) return result;
    // 2) 接收器槽位 1-7（长报文优先，新款设备如 PRO X2 只应答长报文）
    std::optional<BatteryResult> fallback;
    for (int slot = 1; slot <= 7; ++slot) {
        result = hidpp_battery_for(link, slot);
        if (has_data(result)) {
            result->detail = "槽位 " + std::to_string(slot) + ": " + result->detail;
            return result;
        }
        if (result && !fallback) fallback = result;  // 在线但无电量：记下来兜底
    }
    if (fallback) {
        fallback->detail = "槽位设备在线但无电量 feature — " + fallback->detail;
        return fallback;
    }
    return std::nullopt;
}

// 对 vendor 自定义接口跑厂商电量协议（写查询命令 + 听应答）。
std::optional<BatteryResult> scan_vendor(const std::string& vid_hex,
                                         const std::vector<DeviceInfo>& ifaces,
                                         DevFactory& factory) {
    for (const auto& iface : ifaces) {
        if (iface.usage_page < 0xFF00) continue;
        try {
            ReportIO io(iface.path, factory);
            io.open();
            const auto vb = try_vendor_battery(vid_hex, io);
            if (vb) {
                BatteryResult r;
                r.percent = vb->percent;
                r.source = "vendor-" + vb->source;
                r.detail = "厂商协议 " + vb->source + ": " + vb->detail;
                return r;
            }
        } catch (...) {
            continue;
        }
    }
    return std::nullopt;
}

std::optional<BatteryResult> scan_standard(const std::vector<DeviceInfo>& ifaces,
                                           const ReadDescriptorFn& read_descriptor,
                                           DevFactory& factory) {
    for (const auto& iface : ifaces) {
        const Bytes raw = read_descriptor(iface.path);
        if (raw.empty()) continue;
        const auto fields = find_battery_fields(raw);
        if (fields.empty()) continue;
        ReportIO io(iface.path, std::move(factory));
        io.open();
        const auto [value, detail] = read_standard_battery(io, fields);
        BatteryResult r;
        r.source = "hid-standard";
        if (value) {
            r.percent = (*value <= 100) ? std::optional<int>(*value) : std::nullopt;
            r.detail = detail;
            return r;
        }
        r.source = "none";
        r.detail = detail;
        return r;
    }
    return std::nullopt;
}

}  // namespace

std::string BatteryResult::display() const {
    if (percent) return std::to_string(*percent) + "%";
    if (voltage_mv) return std::to_string(*voltage_mv) + "mV";
    return "?";
}

std::vector<ReportField> find_battery_fields(const Bytes& descriptor) {
    const auto info = parse_report_descriptor(descriptor);
    std::vector<ReportField> out;
    for (const auto& f : info.fields) {
        if (f.is_constant || !f.usage) continue;
        const std::pair<int, int> pair{static_cast<int>(*f.usage >> 16),
                                       static_cast<int>(*f.usage & 0xFFFF)};
        if (pair.first == BATTERY_USAGE_PAGE
            || std::find(std::begin(BATTERY_USAGE_PAIRS), std::end(BATTERY_USAGE_PAIRS), pair)
                   != std::end(BATTERY_USAGE_PAIRS))
            out.push_back(f);
    }
    return out;
}

std::pair<std::optional<int>, std::string> read_standard_battery(
        ReportIO& io, const std::vector<ReportField>& fields) {
    const auto feature_it = std::find_if(fields.begin(), fields.end(),
                                         [](const ReportField& f) { return f.report_type == "feature"; });
    if (feature_it != fields.end()) {
        const ReportField& feature_field = *feature_it;
        const auto data = io.get_feature(feature_field.report_id, 16);
        if (data && data->size() > 1) {
            const int offset_byte = feature_field.offset / 8;
            const Bytes body = feature_field.report_id ? Bytes(data->begin() + 1, data->end())
                                                       : *data;
            if (offset_byte < static_cast<int>(body.size())) {
                const int raw = body[static_cast<std::size_t>(offset_byte)];
                const auto pct = scale_percent(feature_field, raw);
                std::string detail = "标准HID电量 usage=0x"
                                     + hex(static_cast<unsigned>(*feature_field.usage & 0xFFFF), 4)
                                     + " raw=" + std::to_string(raw);
                if (pct && *pct != raw) detail += " → " + std::to_string(*pct) + "%";
                return {pct, detail};
            }
        }
        return {std::nullopt, "标准HID电量字段读取失败（可能为写触发型）"};
    }

    const auto input_it = std::find_if(fields.begin(), fields.end(),
                                       [](const ReportField& f) { return f.report_type == "input"; });
    if (input_it != fields.end()) {
        const ReportField& input_field = *input_it;
        const auto data = io.read_input(200);
        if (data) {
            const int byte_index = input_field.offset / 8;
            if (byte_index < static_cast<int>(data->size())) {
                const int raw = (*data)[static_cast<std::size_t>(byte_index)];
                return {scale_percent(input_field, raw),
                        "标准HID电量页 input 推送 raw=" + std::to_string(raw)};
            }
        }
        return {std::nullopt, "标准HID电量字段存在但 200ms 内无推送"};
    }
    return {std::nullopt, "无可读电量字段"};
}

std::vector<BatteryResult> scan_all(const std::map<std::string, std::vector<DeviceInfo>>& grouped,
                                    const ReadDescriptorFn& read_descriptor,
                                    const std::set<std::string>& hidpp_vids,
                                    DevFactory factory) {
    std::vector<BatteryResult> results;
    for (const auto& [vidpid, ifaces] : grouped) {  // map 有序 = Python sorted(grouped)
        if (ifaces.empty()) continue;
        const DeviceInfo& first = ifaces.front();
        BatteryResult result;
        result.vidpid = vidpid;
        result.product = first.product.empty() ? "(未命名)" : first.product;

        // 1) 罗技 → HID++ 路线
        if (hidpp_vids.count(first.vid_hex())) {
            auto found = scan_hidpp(ifaces, factory);
            if (found) {
                found->vidpid = vidpid;
                found->product = result.product;
                results.push_back(std::move(*found));
                continue;
            }
            result.detail = "罗技设备：HID++ 未取得电量（无配对设备/无 vendor 接口）";
        }

        // 2) 厂商协议（EZ2.4G 移植: ATK前行者/MCHOSE/杰理）——只读查询命令
        auto vendor = scan_vendor(first.vid_hex(), ifaces, factory);
        if (vendor) {
            vendor->vidpid = vidpid;
            vendor->product = result.product;
            results.push_back(std::move(*vendor));
            continue;
        }

        // 3) 标准电量页
        auto standard = scan_standard(ifaces, read_descriptor, factory);
        if (standard) {
            standard->vidpid = vidpid;
            standard->product = result.product;
            results.push_back(std::move(*standard));
            continue;
        }

        // 4) 无源
        if (result.detail.empty())
            result.detail = "未发现电量来源（vendor 协议未逆向，可用学习模式抓官方软件）";
        results.push_back(std::move(result));
    }
    return results;
}

}  // namespace pt

// hidpp.cpp — HID++ 客户端实现（core/hidpp.py 平移）。
#include "hidpp.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace pt {

namespace {

// 每请求轮换 sw_id(1-7)：应答回显 funct|sw_id，轮换让匹配无歧义。
// （实测 C54D+PRO X2：固定 sw_id 时前一个慢应答会被误配给下一个请求）
int g_sw_n = 0;
int next_sw_id() {
    g_sw_n = (g_sw_n % 7) + 1;
    return g_sw_n;
}

}  // namespace

const int POLL_RATE_BY_INDEX[7] = {125, 250, 500, 1000, 2000, 4000, 8000};

std::optional<int> poll_index_by_hz(int hz) {
    for (int i = 0; i < 7; ++i)
        if (POLL_RATE_BY_INDEX[i] == hz) return i;
    return std::nullopt;
}

std::string battery_status_name(int status) {
    switch (status) {
        case 0: return "放电中";
        case 1: return "充电中";
        case 2: return "接近充满";
        case 4: return "已充满";
        case 5: return "电量不足(?)";
        default: return "0x" + hex(static_cast<unsigned>(status), 2);
    }
}

void hidpp_reset_sw_id() { g_sw_n = 0; }

// ---------------------------------------------------------------------------
// HidppLink
// ---------------------------------------------------------------------------

HidppLink::HidppLink(const std::string& short_path, const std::string& long_path,
                     DevFactory factory) {
    if (!short_path.empty())
        short_ = std::make_unique<ReportIO>(short_path, factory);
    if (!long_path.empty())
        long_ = std::make_unique<ReportIO>(long_path, std::move(factory));
}

void HidppLink::open() {
    if (short_) short_->open();
    if (long_) long_->open();
}

void HidppLink::close() {
    if (short_) short_->close();
    if (long_) long_->close();
}

HidppChannel& HidppLink::tx_for(const Bytes& wire) {
    HidppChannel* tx = (wire[0] == 0x11 && long_) ? long_.get() : short_.get();
    if (!tx)
        throw ReportError("HID++ 缺少所需通道（短/长）");
    return *tx;
}

int HidppLink::write_output(const Bytes& data) {
    return tx_for(data).write_output(data);
}

std::optional<Bytes> HidppLink::read_input(int timeout_ms, int size) {
    // 单通道：等价 ReportIO；双通道由 call_multi 轮询，此处取短通道
    return short_ ? short_->read_input(timeout_ms, size) : std::nullopt;
}

void HidppLink::set_channels(std::unique_ptr<HidppChannel> short_ch,
                             std::unique_ptr<HidppChannel> long_ch) {
    short_ = std::move(short_ch);
    long_ = std::move(long_ch);
}

std::optional<std::pair<Bytes, std::string>> HidppLink::call_multi(
        const Bytes& wire, const ReportIO::Checks& checks, const Params* params,
        int timeout_ms) {
    HidppChannel& tx = tx_for(wire);
    if (!long_ || !short_)
        return tx.call_multi(wire, checks, params, timeout_ms);

    tx.write_output(wire);
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (true) {
        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      deadline - std::chrono::steady_clock::now())
                                      .count();
        if (remaining_ms <= 0) return std::nullopt;
        for (HidppChannel* io : {short_.get(), long_.get()}) {
            auto response = io->read_input(static_cast<int>(std::min<long long>(remaining_ms, 30)), 64);
            if (response) {
                for (const auto& [label, check] : checks) {
                    if (check.is_null() || match_response(*response, check, params))
                        return std::make_pair(std::move(*response), label);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// feature 调用
// ---------------------------------------------------------------------------

Bytes call_feature(HidppChannel& link, int dev, int feature_index, int funct,
                   const Bytes& params, int timeout_ms, std::optional<bool> long_msg) {
    const int req_lo = ((funct & 0x0F) << 4) | next_sw_id();
    const bool use_long = long_msg.value_or(params.size() > 3);
    Bytes wire;
    if (use_long) {
        wire = {0x11, static_cast<std::uint8_t>(dev),
                static_cast<std::uint8_t>(feature_index), static_cast<std::uint8_t>(req_lo)};
        for (std::size_t i = 0; i < 16 && i < params.size(); ++i) wire.push_back(params[i]);
        wire.resize(20, 0x00);
    } else {
        wire = {0x10, static_cast<std::uint8_t>(dev),
                static_cast<std::uint8_t>(feature_index), static_cast<std::uint8_t>(req_lo)};
        for (std::size_t i = 0; i < 3 && i < params.size(); ++i) wire.push_back(params[i]);
        wire.resize(7, 0x00);
    }
    // 应答报文长度与请求无关（短请求可能得长应答），check 兼容两种 rid
    ReportIO::Checks checks;
    checks.emplace_back("primary", Json::array({Json::array({0x10, 0x11}), dev, feature_index, req_lo}));
    checks.emplace_back("err", Json::array({Json::array({0x10, 0x11}), dev, 0x8F}));
    const auto result = link.call_multi(wire, checks, nullptr, timeout_ms);
    if (!result || result->second == "err") return {};
    const Bytes& resp = result->first;
    if (resp.size() < 4) return {};
    const std::size_t end = std::min<std::size_t>(resp.size(), 20);
    return Bytes(resp.begin() + 4, resp.begin() + end);
}

std::optional<int> get_feature_index(HidppChannel& io, int dev, int feature_id, int timeout_ms) {
    const Bytes params = {static_cast<std::uint8_t>((feature_id >> 8) & 0xFF),
                          static_cast<std::uint8_t>(feature_id & 0xFF)};
    const Bytes payload = call_feature(io, dev, FEATURE_ROOT, 0, params, timeout_ms);
    if (payload.empty()) return std::nullopt;
    const int index = payload[0];
    return index ? std::optional<int>(index) : std::nullopt;
}

// ---------------------------------------------------------------------------
// ping / 在线探测
// ---------------------------------------------------------------------------

std::pair<std::string, std::optional<double>> ping(HidppChannel& link, int dev, int mark,
                                                   int timeout_ms) {
    std::vector<bool> attempts;
    if (auto* hl = dynamic_cast<HidppLink*>(&link); hl && hl->has_long())
        attempts.push_back(true);   // 双通道先试长报文（新款设备只应答长报文）
    attempts.push_back(false);

    std::optional<int> seen_err;
    for (bool use_long : attempts) {
        const int rid = use_long ? 0x11 : 0x10;
        const int sw = next_sw_id();
        Bytes wire = {static_cast<std::uint8_t>(rid), static_cast<std::uint8_t>(dev), 0x00,
                      static_cast<std::uint8_t>(0x10 | sw), 0, 0,
                      static_cast<std::uint8_t>(mark)};
        wire.resize(use_long ? 20 : 7, 0x00);
        ReportIO::Checks checks;
        checks.emplace_back("2.0", Json::array({rid, dev, 0x00, 0x10 | sw, Json(), Json()}));
        checks.emplace_back("err", Json::array({rid, dev, 0x8F}));
        const auto result = link.call_multi(wire, checks, nullptr, timeout_ms);
        if (!result) continue;
        const auto& [response, label] = *result;
        if (label == "2.0")
            return {"2.0", response[4] + response[5] / 10.0};
        const int error_code = response.size() > 5 ? response[5] : 0;
        if (error_code == 0x01)  // INVALID_SUB_ID_COMMAND → HID++ 1.0 设备
            return {"1.0", 1.0};
        seen_err = error_code;
    }
    return {(seen_err == 0x08) ? "empty-slot" : "offline", std::nullopt};
}

std::optional<double> ensure_online(HidppChannel& link, int dev, int attempts, double interval,
                                    int timeout_ms) {
    for (int i = 0; i < attempts; ++i) {
        const auto [state, version] = ping(link, dev, 66, timeout_ms);
        if (state == "2.0") return version;
        if (attempts > 1)
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<long long>(interval * 1000)));
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 报告率（0x8060 基础版 / 0x8061 扩展版，Solaar ReportRate + ExtendedReportRate）
// ---------------------------------------------------------------------------

std::vector<int> get_poll_rates(HidppChannel& link, int dev, int timeout_ms) {
    if (const auto idx = get_feature_index(link, dev, FEATURE_EXTENDED_REPORT_RATE, timeout_ms)) {
        const auto payload = call_feature(link, dev, *idx, 0x10, {}, timeout_ms);
        if (!payload.empty() && payload.size() >= 2) {
            const int flags = payload[0] | (payload[1] << 8);
            std::vector<int> rates;
            for (int i = 0; i < 7; ++i)
                if ((flags >> i) & 0x01) rates.push_back(POLL_RATE_BY_INDEX[i]);
            std::sort(rates.begin(), rates.end(), std::greater<>());
            return rates;
        }
    }
    if (const auto idx = get_feature_index(link, dev, FEATURE_REPORT_RATE, timeout_ms)) {
        const auto payload = call_feature(link, dev, *idx, 0x00, {}, timeout_ms);
        if (!payload.empty()) {
            const int flags = payload[0];
            std::vector<int> rates;
            for (int i = 0; i < 8; ++i)
                if ((flags >> i) & 0x01) rates.push_back(1000 / (i + 1));
            std::sort(rates.begin(), rates.end(), std::greater<>());
            return rates;
        }
    }
    return {};
}

std::optional<int> read_poll_rate(HidppChannel& link, int dev, int timeout_ms) {
    if (const auto idx = get_feature_index(link, dev, FEATURE_EXTENDED_REPORT_RATE, timeout_ms)) {
        const auto payload = call_feature(link, dev, *idx, 0x20, {}, timeout_ms);
        // payload[0] = 档位索引（POLL_RATE_BY_INDEX 按键查值）
        if (!payload.empty() && payload[0] < 7)
            return POLL_RATE_BY_INDEX[payload[0]];
        if (!payload.empty()) return std::nullopt;
    }
    if (const auto idx = get_feature_index(link, dev, FEATURE_REPORT_RATE, timeout_ms)) {
        const auto payload = call_feature(link, dev, *idx, 0x10, {}, timeout_ms);
        if (!payload.empty() && payload[0])
            return 1000 / payload[0];  // 基础版: 字节 = ms
    }
    return std::nullopt;
}

std::pair<bool, std::string> set_poll_rate(HidppChannel& link, int dev, int hz, int timeout_ms) {
    const auto rates = get_poll_rates(link, dev, timeout_ms);
    if (rates.empty())
        return {false, "设备不支持报告率调节（0x8060/0x8061 均无）"};
    const bool supported = std::find(rates.begin(), rates.end(), hz) != rates.end();
    if (!supported)
        return {false, "报告率 " + std::to_string(hz) + "Hz 不在支持列表"};

    if (const auto idx = get_feature_index(link, dev, FEATURE_EXTENDED_REPORT_RATE, timeout_ms)) {
        const int index = *poll_index_by_hz(hz);
        call_feature(link, dev, *idx, 0x30, Bytes{static_cast<std::uint8_t>(index)}, timeout_ms);
        const auto confirm = read_poll_rate(link, dev, timeout_ms);
        if (confirm == hz)
            return {true, "报告率已设为 " + std::to_string(hz) + "Hz（读回验证通过）"};
        return {false, "验证失败：读回 " + (confirm ? std::to_string(*confirm) + "Hz" : "无")
                           + "（板载配置档开启时写入可能不生效）"};
    }
    if (const auto idx = get_feature_index(link, dev, FEATURE_REPORT_RATE, timeout_ms)) {
        const int ms = 1000 / hz;
        call_feature(link, dev, *idx, 0x20, Bytes{static_cast<std::uint8_t>(ms)}, timeout_ms);
        const auto confirm = read_poll_rate(link, dev, timeout_ms);
        if (confirm == hz)
            return {true, "报告率已设为 " + std::to_string(hz) + "Hz（读回验证通过）"};
        return {false, "验证失败：读回 " + (confirm ? std::to_string(*confirm) + "Hz" : "无")};
    }
    return {false, "无可用报告率 feature"};
}

// ---------------------------------------------------------------------------
// 电量
// ---------------------------------------------------------------------------

std::optional<HidppBattery> read_battery(HidppChannel& io, int dev, int timeout_ms) {
    // 1) Unified Battery 0x1004 funct 1: [discharge, level, status]
    if (const auto idx = get_feature_index(io, dev, FEATURE_UNIFIED_BATTERY, timeout_ms)) {
        const auto payload = call_feature(io, dev, *idx, 1, {}, timeout_ms);
        if (payload.size() >= 3) {
            const int discharge = payload[0], level = payload[1], status = payload[2];
            std::string approx;
            switch (level) {
                case 8: approx = "满"; break;
                case 4: approx = "良好"; break;
                case 2: approx = "低"; break;
                case 1: approx = "危急"; break;
                default: approx = "0x" + hex(static_cast<unsigned>(level), 2);
            }
            std::optional<int> percent = discharge ? std::optional<int>(discharge) : std::nullopt;
            HidppBattery b;
            b.percent = percent;
            b.charging = (status == 1 || status == 2);
            b.status_raw = status;
            b.source = "hidpp-unified";
            b.detail = "统一电量: "
                       + (percent ? std::to_string(*percent) : approx + "(档位)")
                       + std::string("%") + (b.charging ? " 充电中" : "");
            return b;
        }
    }

    // 2) Battery Status 0x1000 funct 0: [level, next, status]
    if (const auto idx = get_feature_index(io, dev, FEATURE_BATTERY_STATUS, timeout_ms)) {
        const auto payload = call_feature(io, dev, *idx, 0, {}, timeout_ms);
        if (payload.size() >= 3) {
            const int level = payload[0], status = payload[2];
            HidppBattery b;
            b.percent = level ? std::optional<int>(level) : std::nullopt;
            b.charging = (status == 1 || status == 2);
            b.status_raw = status;
            b.source = "hidpp-status";
            b.detail = "电量 " + std::to_string(level) + "% (" + battery_status_name(status) + ")";
            return b;
        }
    }

    // 3) Battery Voltage 0x1001 funct 0: [vHi, vLo, flags]
    if (const auto idx = get_feature_index(io, dev, FEATURE_BATTERY_VOLTAGE, timeout_ms)) {
        const auto payload = call_feature(io, dev, *idx, 0, {}, timeout_ms);
        if (payload.size() >= 3) {
            const int voltage = (payload[0] << 8) | payload[1];
            const int flags = payload[2];
            HidppBattery b;
            b.voltage_mv = voltage;
            b.charging = (flags & 0x80) != 0;
            b.status_raw = flags;
            b.source = "hidpp-voltage";
            b.detail = "电压 " + std::to_string(voltage) + " mV"
                       + ((flags & 0x80) ? " 充电中" : "");
            return b;
        }
    }
    return std::nullopt;
}

}  // namespace pt

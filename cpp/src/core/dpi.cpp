// dpi.cpp — DPI 读写实现（core/dpi.py 平移）。
#include "dpi.hpp"

namespace pt {

namespace {

// BE16 序列 + 0x0000 结尾 + 0b111 步进编码展开（Solaar produce_dpi_list）。
std::vector<int> parse_dpi_values(const Bytes& raw) {
    std::vector<int> values;
    int i = 0;
    while (i + 1 < static_cast<int>(raw.size())) {
        const int val = (raw[static_cast<std::size_t>(i)] << 8)
                        | raw[static_cast<std::size_t>(i + 1)];
        if (val == 0) break;
        if (val >> 13 == 0b111) {
            const int step = val & 0x1FFF;
            if (i + 3 >= static_cast<int>(raw.size())) break;
            const int last = (raw[static_cast<std::size_t>(i + 2)] << 8)
                             | raw[static_cast<std::size_t>(i + 3)];
            if (!values.empty() && last > values.back())
                for (int v = values.back() + step; v <= last; v += step) values.push_back(v);
            i += 4;
        } else {
            values.push_back(val);
            i += 2;
        }
    }
    return values;
}

// 0x2201 funct 1 分页列表（ignore=1）。
std::vector<int> dpi_list_standard(HidppChannel& link, int dev, int idx, int timeout_ms) {
    Bytes raw;
    for (int page = 0; page < 0x100; ++page) {
        const Bytes payload =
            call_feature(link, dev, idx, 1, Bytes{0x00, 0x00, static_cast<std::uint8_t>(page)},
                         timeout_ms, true);
        if (payload.empty()) break;
        raw.insert(raw.end(), payload.begin() + 1, payload.end());
        if (raw.size() >= 2 && raw[raw.size() - 2] == 0 && raw[raw.size() - 1] == 0) break;
    }
    return parse_dpi_values(raw);
}

// 0x2202 funct 2 分页列表（ignore=3，params [0, axis, page]）。
std::vector<int> dpi_list_extended(HidppChannel& link, int dev, int idx, int axis,
                                   int timeout_ms) {
    Bytes raw;
    for (int page = 0; page < 0x100; ++page) {
        const Bytes payload =
            call_feature(link, dev, idx, 2,
                         Bytes{0x00, static_cast<std::uint8_t>(axis),
                               static_cast<std::uint8_t>(page)},
                         timeout_ms, true);
        if (payload.empty()) break;
        raw.insert(raw.end(), payload.begin() + std::min<std::size_t>(3, payload.size()),
                   payload.end());
        if (raw.size() >= 2 && raw[raw.size() - 2] == 0 && raw[raw.size() - 1] == 0) break;
    }
    return parse_dpi_values(raw);
}

struct DpiDetect {
    std::string name;  // "0x2201" | "0x2202"
    int idx = 0;
    bool y = false;
    bool lod = false;
};

// 探测设备用哪个 DPI feature。返回空 = 都不支持。
std::optional<DpiDetect> detect(HidppChannel& link, int dev, int timeout_ms) {
    if (const auto idx = get_feature_index(link, dev, FEATURE_ADJUSTABLE_DPI, timeout_ms))
        return DpiDetect{"0x2201", *idx, false, false};
    if (const auto idx = get_feature_index(link, dev, FEATURE_EXTENDED_ADJUSTABLE_DPI, timeout_ms)) {
        DpiDetect det{"0x2202", *idx, false, false};
        const auto payload = call_feature(link, dev, *idx, 1, Bytes{0x00}, timeout_ms);
        if (payload.size() >= 3) {
            det.y = (payload[2] & 0x01) != 0;
            det.lod = (payload[2] & 0x02) != 0;
        }
        return det;
    }
    return std::nullopt;
}

int be16(std::uint8_t hi, std::uint8_t lo) { return (hi << 8) | lo; }

}  // namespace

std::vector<int> get_dpi_list(HidppChannel& link, int dev, int timeout_ms) {
    const auto det = detect(link, dev, timeout_ms);
    if (!det) return {};
    if (det->name == "0x2201")
        return dpi_list_standard(link, dev, det->idx, timeout_ms);
    return dpi_list_extended(link, dev, det->idx, 0, timeout_ms);
}

DpiRead read_dpi(HidppChannel& link, int dev, int timeout_ms) {
    const auto det = detect(link, dev, timeout_ms);
    if (!det) return {};
    if (det->name == "0x2201") {
        const auto payload = call_feature(link, dev, det->idx, 2, {}, timeout_ms);
        if (payload.size() < 3) return {};
        int dpi = be16(payload[1], payload[2]);
        std::optional<int> default_dpi;
        if (payload.size() >= 5) {
            const int d = be16(payload[3], payload[4]);
            default_dpi = d ? std::optional<int>(d) : std::nullopt;
        }
        if (dpi == 0) dpi = default_dpi.value_or(0);  // Solaar: 0 → 用默认值
        DpiRead out;
        out.x = dpi ? std::optional<int>(dpi) : std::nullopt;
        out.default_dpi = default_dpi;
        return out;
    }
    // 0x2202 funct 5: X@[1:3](0→默认@[3:5])，Y@[5:7](0→@[7:9])，LOD@[9]
    const auto payload = call_feature(link, dev, det->idx, 5, {}, timeout_ms);
    if (payload.size() < 3) return {};
    int dx = be16(payload[1], payload[2]);
    if (dx == 0 && payload.size() >= 5) dx = be16(payload[3], payload[4]);
    DpiRead out;
    out.x = dx ? std::optional<int>(dx) : std::nullopt;
    if (det->y && payload.size() >= 7) {
        int v = be16(payload[5], payload[6]);
        if (v == 0 && payload.size() >= 9) v = be16(payload[7], payload[8]);
        out.y = v ? std::optional<int>(v) : std::nullopt;
    }
    if (det->lod && payload.size() >= 10)
        out.lod = payload[9];
    return out;
}

DpiOutcome set_dpi(HidppChannel& link, int dev, int new_dpi, int timeout_ms,
                   std::optional<int> lod_override) {
    DpiOutcome out;
    if (new_dpi < 50 || new_dpi > 32000) {
        out.log.push_back("拒绝: DPI " + std::to_string(new_dpi) + " 超出安全范围 (50-32000)");
        return out;
    }

    const auto det = detect(link, dev, timeout_ms);
    if (!det) {
        out.log.push_back("设备不支持 Adjustable DPI (0x2201/0x2202)");
        return out;
    }
    const std::string& name = det->name;
    out.feature = name;
    const int write_funct = (name == "0x2201") ? 3 : 6;

    const auto old_read = read_dpi(link, dev, timeout_ms);
    out.old = old_read.x;
    out.log.push_back("读旧值: " + (old_read.x ? std::to_string(*old_read.x) : "无")
                      + " DPI (" + name + ")"
                      + (old_read.default_dpi
                             ? "（默认 " + std::to_string(*old_read.default_dpi) + "）"
                             : ""));

    // 参数布局（真机实证 PRO X2, 2026-11-14）：0x2202 且 Y/LOD 支持时必须写
    // 完整三段式 [sensorIdx, X, Y, LOD]——缺 LOD/Y 会被设备静默忽略
    const bool has_y = det->y;
    const bool has_lod = det->lod;
    Bytes params = {0x00, static_cast<std::uint8_t>((new_dpi >> 8) & 0xFF),
                    static_cast<std::uint8_t>(new_dpi & 0xFF)};
    if (name == "0x2202" && has_y) {
        // X/Y 双轴设备：Y 轴跟随 X（Solaar 整体写语义的简化）
        params.push_back(static_cast<std::uint8_t>((new_dpi >> 8) & 0xFF));
        params.push_back(static_cast<std::uint8_t>(new_dpi & 0xFF));
        const int lod_byte = lod_override ? *lod_override
                                          : old_read.lod.value_or(2);
        params.push_back(static_cast<std::uint8_t>(lod_byte));
    }
    const auto payload = call_feature(link, dev, det->idx, write_funct, params, timeout_ms);
    out.wrote = new_dpi;
    out.log.push_back(std::string("写: SetDpi(") + std::to_string(new_dpi) + ") → "
                      + (payload.empty() ? "无应答" : "ACK"));

    const auto rb = read_dpi(link, dev, timeout_ms);
    out.readback = rb.x;
    out.log.push_back("读回: " + (rb.x ? std::to_string(*rb.x) : "无") + " DPI");
    if (rb.x == new_dpi) {
        out.ok = true;
        out.log.push_back("验证: 通过");
        return out;
    }

    out.log.push_back("验证: 失败（期望 " + std::to_string(new_dpi)
                      + "）——板载配置档开启时主机写入可能不生效");
    if (out.old) {
        const int old_dpi = *out.old;
        Bytes rollback = {0x00, static_cast<std::uint8_t>((old_dpi >> 8) & 0xFF),
                          static_cast<std::uint8_t>(old_dpi & 0xFF)};
        if (name == "0x2202" && det->y) {
            rollback.push_back(static_cast<std::uint8_t>((old_dpi >> 8) & 0xFF));
            rollback.push_back(static_cast<std::uint8_t>(old_dpi & 0xFF));
            if (det->lod) {
                rollback.push_back(static_cast<std::uint8_t>(
                    old_read.lod.value_or(2)));      // 回滚同样带 LOD
            }
        }
        call_feature(link, dev, det->idx, write_funct, rollback, timeout_ms);
        const auto confirm = read_dpi(link, dev, timeout_ms);
        out.rolled_back = (confirm.x == old_dpi);
        out.log.push_back(std::string("回滚") + (out.rolled_back ? "" : "失败")
                          + ": 读回 " + (confirm.x ? std::to_string(*confirm.x) : "无"));
    }
    return out;
}

}  // namespace pt

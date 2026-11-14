// onboard.cpp — 板载配置档实现（布局真机实证 PRO X2, 2026-11-14）。
// funct 0x00（长）→ [memory, current, macro, count, oob, buttons, sectors, size, ff...]
// funct 0x50（长）→ 分页读 headers，每项 4B [sectorHi, sectorLo, enabled, x]；
//                    前 4B 全 0/全 F 表示需切 ROM 区（param[0]=1）
// funct 0x20      → 板载启用标志（0x01=onboard，0x00=host）
// funct 0x10[01]  → 启用板载；[02] → 切 Host
// funct 0x30 [memory, profile] → 写活动配置档
// funct 0x40（长）→ 活动标识（前 2 字节 = [memory, profile]，真机 01 07）
#include "onboard.hpp"

#include <utility>

namespace pt {

namespace {
constexpr std::uint8_t kProfileMemoryHost = 0x01;   // 真机实证：活动标识首字节
constexpr int kMaxHeaderScan = 10;                  // 分页防失控
}  // namespace

std::string read_onboard_mode(HidppChannel& link, int dev, int timeout_ms) {
    const auto idx = get_feature_index(link, dev, 0x8100, timeout_ms);
    if (!idx) return "unknown";
    const auto payload = call_feature(link, dev, *idx, 0x20, Bytes{}, timeout_ms);
    if (payload.empty()) return "unknown";
    return payload[0] == 0x01 ? "onboard" : "host";
}

std::pair<bool, std::string> set_onboard_mode(HidppChannel& link, int dev,
                                              const std::string& mode,
                                              int timeout_ms) {
    if (mode != "host" && mode != "onboard") {
        return {false, "未知模式（host/onboard）"};
    }
    const auto idx = get_feature_index(link, dev, 0x8100, timeout_ms);
    if (!idx) return {false, "设备不支持板载配置档 feature (0x8100)"};
    const std::uint8_t param = (mode == "host") ? 0x02 : 0x01;
    const auto payload = call_feature(link, dev, *idx, 0x10,
                                      Bytes{param}, timeout_ms);
    if (payload.empty()) return {false, "切换无应答"};
    const std::string confirm = read_onboard_mode(link, dev, timeout_ms);
    if (confirm == mode) {
        return {true, mode == "host" ? "已切到 host 模式（主机接管，DPI/报告率写入生效）"
                                     : "已切到 onboard 模式"};
    }
    return {false, "验证失败：读回 " + confirm};
}

OnboardInfo read_onboard_info(HidppChannel& link, int dev, int timeout_ms) {
    OnboardInfo info;
    info.mode = read_onboard_mode(link, dev, timeout_ms);
    const auto idx = get_feature_index(link, dev, 0x8100, timeout_ms);
    if (!idx) return info;
    const auto resp = call_feature(link, dev, *idx, 0x00, Bytes{}, timeout_ms, /*long=*/true);
    if (resp.empty()) return info;
    {
        std::string hexed;
        static const char* digits = "0123456789abcdef";
        for (std::uint8_t byte : resp) {
            hexed.push_back(digits[byte >> 4]);
            hexed.push_back(digits[byte & 0xF]);
            hexed.push_back(' ');
        }
        info.raw = hexed;
    }
    if (resp.size() >= 3) {
        info.current_profile = resp[1];
        info.macro = resp[2];
    }
    if (resp.size() >= 6) {
        info.profile_count = resp[3];
        info.buttons = resp[5];
    }

    // headers 分页
    auto chunk = call_feature(link, dev, *idx, 0x50,
                              Bytes{0x00, 0x00, 0x00, 0x00}, timeout_ms, /*long=*/true);
    std::uint8_t mem_flag = 0x00;
    if (!chunk.empty() && (std::equal(chunk.begin(), chunk.begin() + 4, Bytes{0, 0, 0, 0}.begin()) ||
                           (chunk.size() >= 4 && chunk[0] == 0xFF && chunk[1] == 0xFF &&
                            chunk[2] == 0xFF && chunk[3] == 0xFF))) {
        chunk = call_feature(link, dev, *idx, 0x50,
                             Bytes{0x01, 0x00, 0x00, 0x00}, timeout_ms, /*long=*/true);
        mem_flag = 0x01;
    }
    int i = 0;
    const int limit = (info.profile_count ? *info.profile_count : 8) + 2;
    while (!chunk.empty() && chunk.size() >= 3 &&
           !(chunk[0] == 0xFF && chunk[1] == 0xFF) && i < limit) {
        const int sector = (chunk[0] << 8) | chunk[1];
        info.headers.push_back({sector, chunk[2] != 0});
        ++i;
        chunk = call_feature(link, dev, *idx, 0x50,
                             Bytes{mem_flag, 0x00, 0x00,
                                   static_cast<std::uint8_t>((i * 4) & 0xFF)},
                             timeout_ms, /*long=*/true);
    }
    return info;
}

std::pair<bool, std::string> set_active_profile(HidppChannel& link, int dev,
                                                int profile, int memory,
                                                int timeout_ms) {
    const auto idx = get_feature_index(link, dev, 0x8100, timeout_ms);
    if (!idx) return {false, "设备不支持板载配置档 feature (0x8100)"};
    const auto enable = call_feature(link, dev, *idx, 0x10,
                                     Bytes{0x01}, timeout_ms);
    if (enable.empty()) return {false, "启用板载模式无应答"};
    const auto write = call_feature(link, dev, *idx, 0x30,
                                    Bytes{static_cast<std::uint8_t>(memory & 0xFF),
                                          static_cast<std::uint8_t>(profile & 0xFF)},
                                    timeout_ms);
    if (write.empty()) return {false, "写配置档无应答"};
    const auto confirm = call_feature(link, dev, *idx, 0x40, Bytes{}, timeout_ms, /*long=*/true);
    if (confirm.size() < 2) return {false, "读回无应答"};
    if (confirm[0] == (memory & 0xFF) && confirm[1] == (profile & 0xFF)) {
        return {true, "已切到板载配置档 #" + std::to_string(profile) + "（读回验证通过）"};
    }
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02X %02X", confirm[0], confirm[1]);
    return {false, std::string("验证失败：读回标识 ") + buf};
}

}  // namespace pt

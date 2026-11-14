// onboard.hpp — 板载配置档管理（0x8100，Solaar OnboardProfiles 平移）。
// 背景：PRO X2 等新款罗技鼠标无 REPROG feature（0x1B00/1B04 真机确认），
// 逐键重映射由板载配置档承载：每档一套按键映射/DPI/报告率。
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "hidpp.hpp"

namespace pt {

struct OnboardHeader {
    int sector = 0;
    bool enabled = false;
};

struct OnboardInfo {
    std::string mode = "unknown";    // "host" / "onboard" / "unknown"
    std::optional<int> current_profile;
    std::optional<int> macro;
    std::optional<int> profile_count;
    std::optional<int> buttons;      // 可自定义按键数
    std::vector<OnboardHeader> headers;
    std::string raw;
    bool supported() const { return profile_count.has_value(); }
};

// 模式读取（"host"/"onboard"/"unknown"）与切换。
std::string read_onboard_mode(HidppChannel& link, int dev, int timeout_ms = 500);
std::pair<bool, std::string> set_onboard_mode(HidppChannel& link, int dev,
                                              const std::string& mode,
                                              int timeout_ms = 600);

// 全貌读取（模式/当前档/档数/按键数/headers 分页枚举）。
OnboardInfo read_onboard_info(HidppChannel& link, int dev, int timeout_ms = 600);

// 切换活动配置档：funct 0x10[0x01] 启用 + funct 0x30 [memory, profile] + 读回验证。
// 注意：写 profile 会同时启用板载模式；回 Host 用 set_onboard_mode("host")。
std::pair<bool, std::string> set_active_profile(HidppChannel& link, int dev,
                                                int profile, int memory = 0x01,
                                                int timeout_ms = 600);

}  // namespace pt

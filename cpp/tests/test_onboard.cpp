// test_onboard.cpp — 板载配置档测试（0x8100，Solaar OnboardProfiles 平移对照）。
#include "core/onboard.hpp"

#include "doctest.h"
#include "hidpp_scripted.hpp"
#include "test_report_io.hpp"

using pt_test::FakeDev;
using pt_test::HidppScript;
using pt_test::install_fake;

namespace pt {

TEST_CASE("read_onboard_info 解析（host 模式 + 配置档头）") {
    HidppScript script;
    ReportIO link("x");
    FakeDev* dev = install_fake(link, {
        script.gf(1, 0x0D),                       // mode 的 get_feature_index
        script.call(1, 0x0D, 0x20, {0x00}),       // 板载未启用 → host
        script.gf(1, 0x0D),                       // info 的 get_feature_index
        script.call(1, 0x0D, 0x00,
                    {0x01, 0x07, 0x01, 0x05, 0x01, 0x05, 0x10, 0x00,
                     0xFF, 0x0A, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00},
                    true),
        script.call(1, 0x0D, 0x50,
                    {0x01, 0x07, 0x01, 0x00, 0x00, 0x00},
                    true),
    });
    const auto info = read_onboard_info(link, 1);
    CHECK(info.mode == "host");
    CHECK(info.current_profile.value_or(0) == 7);
    CHECK(info.profile_count.value_or(0) == 5);
    CHECK(info.buttons.value_or(0) == 5);
    REQUIRE(info.headers.size() == 1);
    CHECK(info.headers[0].sector == 0x0107);
    CHECK(info.headers[0].enabled);
}

TEST_CASE("set_onboard_mode host 读回验证") {
    HidppScript script;
    ReportIO link("x");
    FakeDev* dev = install_fake(link, {
        script.gf(1, 0x0D),
        script.call(1, 0x0D, 0x10, {0x02}),
        script.gf(1, 0x0D),                        // 读回前的第二次 feature 查询
        script.call(1, 0x0D, 0x20, {0x00}),
    });
    const auto [ok, msg] = set_onboard_mode(link, 1, "host");
    CHECK(ok);
    CHECK(msg.find("主机接管") != std::string::npos);
}

TEST_CASE("set_active_profile 读回验证") {
    HidppScript script;
    ReportIO link("x");
    FakeDev* dev = install_fake(link, {
        script.gf(1, 0x0D),
        script.call(1, 0x0D, 0x10, {0x01}),
        script.call(1, 0x0D, 0x30, {0x01, 0x07}),
        script.call(1, 0x0D, 0x40,
                    {0x01, 0x07, 0x01, 0x05, 0x01, 0x05, 0x10, 0x00},
                    true),
    });
    const auto [ok, msg] = set_active_profile(link, 1, 7);
    CHECK(ok);
    CHECK(msg.find("读回验证通过") != std::string::npos);
}

TEST_CASE("set_active_profile 读回不符报失败") {
    HidppScript script;
    ReportIO link("x");
    FakeDev* dev = install_fake(link, {
        script.gf(1, 0x0D),
        script.call(1, 0x0D, 0x10, {0x01}),
        script.call(1, 0x0D, 0x30, {0x01, 0x07}),
        script.call(1, 0x0D, 0x40,
                    {0x01, 0x05, 0x01, 0x05, 0x01, 0x05, 0x10, 0x00},
                    true),
    });
    const auto [ok, msg] = set_active_profile(link, 1, 7);
    CHECK(!ok);
    CHECK(msg.find("01 05") != std::string::npos);
}

}  // namespace pt

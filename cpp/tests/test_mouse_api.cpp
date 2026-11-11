// test_mouse_api.cpp — M2.5c 测试（tests/test_mouse_api.py 平移：报告率 + 统一 API 模板）。
#include <memory>

#include "doctest.h"
#include "core/mouse_api.hpp"
#include "fake_backend.hpp"
#include "hidpp_scripted.hpp"
#include "test_report_io.hpp"

using namespace pt;
using pt::test::FakeBackend;
using pt_test::b;
using pt_test::Bytes;
using pt_test::FakeDev;
using pt_test::HidDevice;
using pt_test::HidppScript;
using pt_test::install_fake;
using pt_test::ReportIO;

static constexpr int DEV = 1;

// ---------------------------------------------------------------------------
// 报告率（0x8060/0x8061）
// ---------------------------------------------------------------------------

TEST_CASE("get_poll_rates 扩展版") {
    // 0x8061: 功能位 2-byte bitflags，bit0=125Hz … bit6=8000Hz
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {
        script.gf(DEV, 0x09),
        script.call(DEV, 0x09, 0x10, Bytes{0x0F, 0x00}),  // bit0-3 → 125/250/500/1000
    });
    const auto rates = get_poll_rates(link, DEV);
    REQUIRE(rates.size() == 4);
    CHECK(rates[0] == 1000);
    CHECK(rates[1] == 500);
    CHECK(rates[2] == 250);
    CHECK(rates[3] == 125);
}

TEST_CASE("get_poll_rates 基础版回退") {
    // 0x8061 无 → 0x8060: 字节 bitflags bit0=1ms … bit7=8ms
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {
        script.gf(DEV, 0),
        script.gf(DEV, 0x05),
        script.call(DEV, 0x05, 0x00, Bytes{0b00000101, 0x00}),  // 1ms + 3ms
    });
    const auto rates = get_poll_rates(link, DEV);
    REQUIRE(rates.size() == 2);
    CHECK(rates[0] == 1000);
    CHECK(rates[1] == 333);
}

TEST_CASE("read_poll_rate 扩展版") {
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {
        script.gf(DEV, 0x09),
        script.call(DEV, 0x09, 0x20, Bytes{0x03}),  // 索引 3 = 1000Hz
    });
    const auto rate = read_poll_rate(link, DEV);
    REQUIRE(rate.has_value());
    CHECK(*rate == 1000);
}

TEST_CASE("set_poll_rate 扩展版读回验证") {
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {
        script.gf(DEV, 0x09),                                // get_poll_rates
        script.call(DEV, 0x09, 0x10, Bytes{0x0F, 0x00}),
        script.gf(DEV, 0x09),                                // set 的 get_feature_index
        script.call(DEV, 0x09, 0x30, Bytes{0x03}),           // 写 funct 0x30
        script.gf(DEV, 0x09),                                // 读回
        script.call(DEV, 0x09, 0x20, Bytes{0x03}),
    });
    const auto [ok, msg] = set_poll_rate(link, DEV, 1000);
    CHECK(ok);
    CHECK(msg.find("验证通过") != std::string::npos);
}

TEST_CASE("set_poll_rate 不支持") {
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {script.gf(DEV, 0), script.gf(DEV, 0)});
    const auto [ok, msg] = set_poll_rate(link, DEV, 1000);
    CHECK_FALSE(ok);
    CHECK(msg.find("不支持") != std::string::npos);
}

TEST_CASE("set_poll_rate 不在支持列表") {
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {
        script.gf(DEV, 0x09),
        script.call(DEV, 0x09, 0x10, Bytes{0x0F, 0x00}),
    });
    const auto [ok, msg] = set_poll_rate(link, DEV, 8000);
    CHECK_FALSE(ok);
    CHECK(msg.find("不在支持列表") != std::string::npos);
}

// ---------------------------------------------------------------------------
// MouseApi 模板（假后端验证注册/路由机制）
// ---------------------------------------------------------------------------

// 测试期把注册表换成 FakeBackend（对应 Python monkeypatch BACKENDS）
struct RegistrySwap {
    std::vector<BackendFactory> saved;
    RegistrySwap() {
        saved = backend_registry();
        backend_registry() = {[]() -> std::unique_ptr<MouseBackend> {
            return std::make_unique<FakeBackend>();
        }};
    }
    ~RegistrySwap() { backend_registry() = saved; }
};

TEST_CASE("注册表与 detect 路由") {
    RegistrySwap swap;
    (void)swap;
    std::vector<DeviceInfo> ifaces;
    DeviceInfo d;
    d.vendor_id = 0xABCD;
    d.product_id = 0x1234;
    ifaces.push_back(d);
    const auto api = detect(ifaces);
    REQUIRE(api != nullptr);
    CHECK(api->brand() == "Fake");

    DeviceInfo unknown;
    unknown.vendor_id = 0xFFFF;
    unknown.product_id = 0x0001;
    CHECK(detect(std::vector<DeviceInfo>{unknown}) == nullptr);
}

TEST_CASE("MouseApi 完整流程") {
    RegistrySwap swap;
    std::vector<DeviceInfo> ifaces;
    DeviceInfo d;
    d.vendor_id = 0xABCD;
    d.product_id = 0x1234;
    ifaces.push_back(d);
    const auto api = detect(ifaces);
    REQUIRE(api != nullptr);
    const auto caps = api->capabilities();
    REQUIRE(caps.dpi_list.size() == 3);
    CHECK(caps.dpi_list[0] == 400);
    CHECK(caps.dpi_list[2] == 1600);
    REQUIRE(caps.poll_rates.size() == 2);
    CHECK(caps.poll_rates[0] == 1000);
    const auto dpi = api->get_dpi();
    REQUIRE(dpi.x.has_value());
    CHECK(*dpi.x == 800);
    const auto out = api->set_dpi(1600);
    CHECK(out.ok);
    REQUIRE(out.wrote.has_value());
    CHECK(*out.wrote == 1600);
    CHECK(api->get_poll_rate() == 1000);
    const auto [ok, msg] = api->set_poll_rate(500);
    CHECK(ok);
    const auto battery = api->get_battery();
    REQUIRE(battery.has_value());
    REQUIRE(battery->percent.has_value());
    CHECK(*battery->percent == 70);
}

TEST_CASE("MouseApi 拒绝越界 DPI") {
    RegistrySwap swap;
    std::vector<DeviceInfo> ifaces;
    DeviceInfo d;
    d.vendor_id = 0xABCD;
    d.product_id = 0x1234;
    ifaces.push_back(d);
    const auto api = detect(ifaces);
    REQUIRE(api != nullptr);
    const auto out = api->set_dpi(99999);
    CHECK_FALSE(out.ok);
    REQUIRE_FALSE(out.log.empty());
    CHECK(out.log[0].find("超出安全范围") != std::string::npos);
}

TEST_CASE("罗技后端已注册") {
    bool found = false;
    for (const auto& make : backend_registry()) {
        auto backend = make();
        found = found || backend->brand() == "Logitech";
    }
    CHECK(found);
}

TEST_CASE("summary 字典") {
    RegistrySwap swap;
    std::vector<DeviceInfo> ifaces;
    DeviceInfo d;
    d.vendor_id = 0xABCD;
    d.product_id = 0x1234;
    ifaces.push_back(d);
    const auto api = detect(ifaces);
    REQUIRE(api != nullptr);
    const auto s = api->summary();
    CHECK(s["brand"] == "Fake");
    CHECK(s["dpi_list"] == Json::parse("[400, 800, 1600]"));
}

// ---------------------------------------------------------------------------
// 标准电量缩放
// ---------------------------------------------------------------------------

TEST_CASE("电量百分比缩放") {
    ReportField f;
    f.report_type = "feature";
    f.report_id = 3;
    f.usage = 0x850002;
    f.logical_min = 0;
    f.logical_max = 10;
    const auto scaled = scale_percent(f, 5);
    REQUIRE(scaled.has_value());
    CHECK(*scaled == 50);  // 0-10 → 百分比

    ReportField f100 = f;
    f100.logical_max = 100;
    const auto scaled100 = scale_percent(f100, 88);
    REQUIRE(scaled100.has_value());
    CHECK(*scaled100 == 88);  // 已是百分比
}

TEST_CASE("find_battery_fields 匹配 EZ2.4G usage 对") {
    // 键盘页 (0x0006, 0x0020) 用法对（EZ2.4G 实测）
    const Bytes desc = pt_test::bhex(
        "05 06 09 20 A1 01 85 02 09 20 15 00 25 64 75 08 95 01 81 02 C0");
    const auto fields = find_battery_fields(desc);
    REQUIRE(fields.size() == 1);
    REQUIRE(fields[0].logical_max.has_value());
    CHECK(*fields[0].logical_max == 100);
}

// test_battery.cpp — M2.5a 单元测试（tests/test_battery.py 平移，假设备脚本化，离线）。
#include <cstdlib>
#include <map>
#include <memory>

#include "doctest.h"
#include "core/battery.hpp"
#include "hidpp_scripted.hpp"
#include "test_report_io.hpp"

using namespace pt;
using pt_test::b;
using pt_test::bhex;
using pt_test::Bytes;
using pt_test::FakeDev;
using pt_test::HidDevice;
using pt_test::HidppScript;
using pt_test::install_fake;
using pt_test::install_scripted;
using pt_test::ReportIO;
using pt_test::ScriptedDev;

static constexpr int DEV = 0xFF;

// DeviceInfo 构造辅助（对应 test_device_manager.fake_raw + _mk）
static DeviceInfo mk(const std::string& vidpid, int usage_page = 0x0001, int usage = 0x0006,
                     const std::string& product = "Dev") {
    DeviceInfo d;
    d.path = "\\\\?\\" + vidpid;
    d.vendor_id = static_cast<unsigned>(std::strtoul(vidpid.substr(0, 4).c_str(), nullptr, 16));
    d.product_id = static_cast<unsigned>(std::strtoul(vidpid.substr(5, 4).c_str(), nullptr, 16));
    d.product = product;
    d.usage_page = static_cast<unsigned>(usage_page);
    d.usage = static_cast<unsigned>(usage);
    d.bus_type = 1;
    return d;
}

// ---------------------------------------------------------------------------
// hidpp.ping / get_feature_index
// ---------------------------------------------------------------------------

TEST_CASE("ping 2.0 应答") {
    HidppScript script;
    ReportIO io("x");
    install_fake(io, {script.ping(DEV, 0x04, 0x05)});
    const auto [state, version] = ping(io, DEV);
    CHECK(state == "2.0");
    REQUIRE(version.has_value());
    CHECK(*version == doctest::Approx(4.5));
}

TEST_CASE("ping 双通道长报文优先") {
    // 双通道：长报文优先并命中（HidppLink，模拟 PRO X2 长报文专用设备）
    HidppScript script;
    FakeDev* dev = new FakeDev(std::vector<Bytes>{script.ping(DEV, 0x04, 0x05, true)});
    HidppLink link("", "long-only", [dev](const std::string&) {
        return std::unique_ptr<HidDevice>(dev);
    });
    const auto [state, version] = ping(link, DEV);
    CHECK(state == "2.0");
    REQUIRE(version.has_value());
    CHECK(*version == doctest::Approx(4.5));
}

TEST_CASE("ping 1.0 错误应答") {
    HidppScript script;
    ReportIO io("x");
    install_fake(io, {script.err(DEV, 0x01)});
    const auto [state, version] = ping(io, DEV);
    CHECK(state == "1.0");
    REQUIRE(version.has_value());
    CHECK(*version == doctest::Approx(1.0));
}

TEST_CASE("ping 空槽位") {
    HidppScript script;
    ReportIO io("x");
    install_fake(io, {script.err(DEV, 0x08)});
    const auto [state, version] = ping(io, DEV);
    CHECK(state == "empty-slot");
    CHECK_FALSE(version.has_value());
}

TEST_CASE("ping 超时") {
    HidppScript script;
    ReportIO io("x");
    install_fake(io);
    const auto [state, version] = ping(io, DEV);
    CHECK(state == "offline");
    CHECK_FALSE(version.has_value());
}

TEST_CASE("get_feature_index 请求报文与解析") {
    HidppScript script;
    ReportIO io("x");
    FakeDev* dev = install_fake(io, {script.gf(DEV, 0x07)});
    CHECK(get_feature_index(io, DEV, FEATURE_UNIFIED_BATTERY) == 0x07);
    REQUIRE_FALSE(dev->written.empty());
    const Bytes& wire = dev->written[0];
    CHECK(wire[0] == 0x10);
    CHECK(wire[2] == 0x00);
    CHECK(Bytes(wire.begin() + 4, wire.begin() + 6) == Bytes{0x10, 0x04});
}

TEST_CASE("get_feature_index 不支持返回空") {
    HidppScript script;
    ReportIO io("x");
    install_fake(io, {script.gf(DEV, 0)});
    CHECK_FALSE(get_feature_index(io, DEV, FEATURE_UNIFIED_BATTERY).has_value());
}

// ---------------------------------------------------------------------------
// read_battery 三路回退
// ---------------------------------------------------------------------------

TEST_CASE("read_battery unified") {
    // GetFeature(0x1004)→idx07, 调用 funct1 → [discharge=85, level=8, status=1]
    HidppScript script;
    ReportIO io("x");
    install_fake(io, {
        script.gf(DEV, 0x07),
        script.call(DEV, 0x07, 1, Bytes{85, 8, 1}),
    });
    const auto bat = read_battery(io, DEV);
    REQUIRE(bat.has_value());
    REQUIRE(bat->percent.has_value());
    CHECK(*bat->percent == 85);
    CHECK_FALSE(bat->voltage_mv.has_value());
    CHECK(bat->charging);
    CHECK(bat->status_raw == 1);
    CHECK(bat->source == "hidpp-unified");
    CHECK(bat->detail == "统一电量: 85% 充电中");
}

TEST_CASE("read_battery status 回退") {
    // 0x1004 不支持(idx00)，0x1000 → idx03, funct0 → [level=60, next=70, status=0]
    HidppScript script;
    ReportIO io("x");
    install_fake(io, {
        script.gf(DEV, 0),
        script.gf(DEV, 0x03),
        script.call(DEV, 0x03, 0, Bytes{60, 70, 0}),
    });
    const auto bat = read_battery(io, DEV);
    REQUIRE(bat.has_value());
    REQUIRE(bat->percent.has_value());
    CHECK(*bat->percent == 60);
    CHECK(bat->source == "hidpp-status");
}

TEST_CASE("read_battery voltage 回退") {
    // 三个查询 feature 都不支持，最后 0x1001 → idx05, funct0 → [0x0E, 0x2E, 0x80]
    HidppScript script;
    ReportIO io("x");
    install_fake(io, {
        script.gf(DEV, 0),                                  // 0x1004 无
        script.gf(DEV, 0),                                  // 0x1000 无
        script.gf(DEV, 0x05),                               // 0x1001 → 5
        script.call(DEV, 0x05, 0, Bytes{0x0E, 0x2E, 0x80}),
    });
    const auto bat = read_battery(io, DEV);
    REQUIRE(bat.has_value());
    REQUIRE(bat->voltage_mv.has_value());
    CHECK(*bat->voltage_mv == 0x0E2E);
    CHECK(bat->charging);
    CHECK(bat->source == "hidpp-voltage");
}

TEST_CASE("read_battery 无 feature 返回空") {
    HidppScript script;
    ReportIO io("x");
    install_fake(io, {script.gf(DEV, 0), script.gf(DEV, 0), script.gf(DEV, 0)});
    CHECK_FALSE(read_battery(io, DEV).has_value());
}

// ---------------------------------------------------------------------------
// battery 标准页 + scan_all
// ---------------------------------------------------------------------------

// 含 0x85 页 feature 字段的描述符（feature report id=3, 8字节, offset 0）
static const Bytes STD_BATTERY_DESC = bhex(
    "05 85 09 02 A1 01"
    "85 03"
    "09 02 15 00 25 64 75 08 95 01 B1 02"
    "C0"
);

TEST_CASE("find_battery_fields") {
    const auto fields = find_battery_fields(STD_BATTERY_DESC);
    REQUIRE(fields.size() == 1);
    REQUIRE(fields[0].usage.has_value());
    CHECK(*fields[0].usage == 0x850002);
    CHECK(fields[0].report_type == "feature");
}

TEST_CASE("read_standard_battery feature 直读") {
    ReportIO io("x");
    install_scripted(io, {}, {Bytes{0x03, 88, 0, 0, 0, 0, 0, 0}});
    const auto fields = find_battery_fields(STD_BATTERY_DESC);
    const auto [value, detail] = read_standard_battery(io, fields);
    REQUIRE(value.has_value());
    CHECK(*value == 88);
    CHECK(detail.find("raw=88") != std::string::npos);
}

TEST_CASE("scan_all 标准电量页路线") {
    // scan_all 内部自建 ReportIO：假设备经 factory 注入
    ScriptedDev* dev = new ScriptedDev({}, {Bytes{0x03, 73, 0, 0, 0, 0, 0, 0}});
    DevFactory factory = [dev](const std::string&) {
        return std::unique_ptr<HidDevice>(dev);
    };
    std::map<std::string, std::vector<DeviceInfo>> grouped;
    grouped["1A2B:3C4D"] = {mk("1A2B:3C4D", 0x85, 0x002)};
    const auto results = scan_all(grouped, [](const std::string&) { return STD_BATTERY_DESC; },
                                  {"046D"}, factory);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].percent.has_value());
    CHECK(*results[0].percent == 73);
    CHECK(results[0].source == "hid-standard");
    CHECK(results[0].display() == "73%");
}

TEST_CASE("scan_all HID++ 接收器路线") {
    // 槽位1 应答 1.0(err 0x01) 但无电量 feature → 在线但 source=none
    // 工厂每次交付新 FakeDev、共享同一脚本队列（scan_all 内部可能多次开设备）
    HidppScript script;
    auto queue = std::make_shared<std::vector<Bytes>>(std::vector<Bytes>{script.err(0x01, 0x01)});
    DevFactory factory = [queue](const std::string&) {
        return std::unique_ptr<HidDevice>(new FakeDev(queue));
    };
    std::map<std::string, std::vector<DeviceInfo>> grouped;
    grouped["046D:C54D"] = {mk("046D:C54D", 0xFF00, 0x0001, "USB Receiver")};
    const auto results = scan_all(grouped, [](const std::string&) { return Bytes{}; },
                                  {"046D"}, factory);
    REQUIRE(results.size() == 1);
    CHECK(results[0].vidpid == "046D:C54D");
    CHECK(results[0].source == "none");
    CHECK(results[0].detail.find("HID++") != std::string::npos);
}

TEST_CASE("scan_all 接收器自身应答不得短路槽位扫描") {
    // 回归：0xFF（接收器自身）应答但无电量时，必须继续扫槽位。
    // 实测 bug：接收器 0xFF 应答 HID++ 1.0（无电量 feature）短路了槽位扫描，
    // 导致配对在槽位 1 的 PRO X2 电量（92%）永远读不到。
    HidppScript script;
    // 双通道：两条 ReportIO 各持一个 FakeDev、共享同一脚本队列
    auto queue = std::make_shared<std::vector<Bytes>>(std::vector<Bytes>{
        script.err(0xFF, 0x01, true),               // 0xFF 长ping → 1.0（HidppLink 长优先）
        script.gf(0xFF, 0),                         // 0x1004 无
        script.gf(0xFF, 0),                         // 0x1000 无
        script.gf(0xFF, 0),                         // 0x1001 无
        script.ping(1, 4, 2, true),                 // 槽位1 → HID++ 4.2（长报文）
        script.gf(1, 0x07),                         // 0x1004 → idx7
        script.call(1, 0x07, 1, Bytes{92, 8, 0}),   // 92%
    });
    DevFactory factory = [queue](const std::string&) {
        return std::unique_ptr<HidDevice>(new FakeDev(queue));
    };
    std::map<std::string, std::vector<DeviceInfo>> grouped;
    grouped["046D:C54D"] = {mk("046D:C54D", 0xFF00, 0x0001, "USB Receiver"),
                            mk("046D:C54D", 0xFF00, 0x0002, "USB Receiver")};
    const auto results = scan_all(grouped, [](const std::string&) { return Bytes{}; },
                                  {"046D"}, factory);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].percent.has_value());
    CHECK(*results[0].percent == 92);
    CHECK(results[0].detail.find("槽位 1") != std::string::npos);
}

TEST_CASE("BatteryResult display") {
    BatteryResult r;
    r.percent = 88;
    CHECK(r.display() == "88%");
    BatteryResult r2;
    r2.voltage_mv = 3800;
    CHECK(r2.display() == "3800mV");
    BatteryResult r3;
    CHECK(r3.display() == "?");
}

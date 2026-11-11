// test_device_manager.cpp — 任务 1.1 单元测试（tests/test_device_manager.py 平移）。
// 枚举函数全部注入假后端，离线运行、不碰真机。
#include <algorithm>
#include <atomic>

#include "doctest.h"
#include "core/device_manager.hpp"
#include "core/hid_api.hpp"

using namespace pt;

static hid::RawInfo raw(const std::string& path = "\\\\?\\test", unsigned vid = 0x046D,
                        unsigned pid = 0xC54D, const std::string& product = "USB Receiver",
                        int interface = 1, unsigned usage_page = 1, unsigned usage = 0x80) {
    hid::RawInfo r;
    r.path = path;
    r.vendor_id = static_cast<unsigned short>(vid);
    r.product_id = static_cast<unsigned short>(pid);
    r.serial_number = "TESTSER";
    r.release_number = 0x1404;
    r.manufacturer = "Logitech";
    r.product = product;
    r.usage_page = static_cast<unsigned short>(usage_page);
    r.usage = static_cast<unsigned short>(usage);
    r.interface_number = interface;
    r.bus_type = 1;
    return r;
}

static DeviceInfo dev(const std::string& path) { return device_from_raw(raw(path)); }

// ---------------------------------------------------------------------------
// DeviceInfo
// ---------------------------------------------------------------------------

TEST_CASE("from_hid_dict 与 hex 属性") {
    const auto info = device_from_raw(raw());
    CHECK(info.vid_hex() == "046D");
    CHECK(info.pid_hex() == "C54D");
    CHECK(info.product == "USB Receiver");
    CHECK(info.serial_number == "TESTSER");
}

TEST_CASE("path 为空串不出垃圾字符") {
    const auto info = device_from_raw(raw(""));
    CHECK(info.path == "");
    const auto info2 = device_from_raw(raw());
    CHECK(info2.path.rfind("\\\\?\\", 0) == 0);  // bytes path 正确保留
}

// ---------------------------------------------------------------------------
// list_devices / find_devices / snapshot
// ---------------------------------------------------------------------------

TEST_CASE("list_devices") {
    EnumerateFn backend = [] {
        return std::vector<DeviceInfo>{device_from_raw(raw("a")),
                                       device_from_raw(raw("b", 0x046D, 0xC54D, "USB Receiver", 2))};
    };
    const auto devices = list_devices(backend);
    CHECK(devices.size() == 2);
}

TEST_CASE("list_devices 包装错误") {
    EnumerateFn backend = []() -> std::vector<DeviceInfo> { throw std::runtime_error("usb error"); };
    CHECK_THROWS_AS(list_devices(backend), const HIDError&);
}

TEST_CASE("find_devices 过滤") {
    const std::vector<DeviceInfo> all{device_from_raw(raw("a")),
                                      device_from_raw(raw("b", 0x1532, 0x0045, "Razer")),
                                      device_from_raw(raw("c", 0x046D, 0xC088, "G502"))};
    const auto logitech = find_devices(0x046D, std::nullopt, &all);
    CHECK(logitech.size() == 2);
    CHECK(logitech[0].path == "a");
    CHECK(logitech[1].path == "c");

    const auto exact = find_devices(0x046D, 0xC54D, &all);
    REQUIRE(exact.size() == 1);
    CHECK(exact[0].path == "a");

    const auto all_devices = find_devices(std::nullopt, std::nullopt, &all);
    CHECK(all_devices.size() == 3);

    // 传入现成列表则不重新枚举
    const auto again = find_devices(0x1532, std::nullopt, &all);
    REQUIRE(again.size() == 1);
    CHECK(again[0].path == "b");
}

TEST_CASE("snapshot 键是 path") {
    EnumerateFn backend = [] { return std::vector<DeviceInfo>{dev("a"), dev("b")}; };
    const auto snap = snapshot(backend);
    CHECK(snap.count("a") == 1);
    CHECK(snap.count("b") == 1);
    CHECK(snap.size() == 2);
}

// ---------------------------------------------------------------------------
// diff_snapshots（热插拔核心，纯函数）
// ---------------------------------------------------------------------------

TEST_CASE("空快照到两个设备 = 两次 added") {
    DeviceMap new_;
    new_["a"] = dev("a");
    new_["b"] = dev("b");
    const auto events = diff_snapshots({}, new_);
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == "added");
    CHECK(events[0].device.path == "a");
    CHECK(events[1].kind == "added");
    CHECK(events[1].device.path == "b");
}

TEST_CASE("移除事件") {
    DeviceMap old, new_;
    old["a"] = dev("a");
    old["b"] = dev("b");
    new_["b"] = dev("b");
    const auto events = diff_snapshots(old, new_);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == "removed");
    CHECK(events[0].device.path == "a");
}

TEST_CASE("无变化输出为空且顺序稳定") {
    DeviceMap snap;
    snap["a"] = dev("a");
    snap["b"] = dev("b");
    snap["c"] = dev("c");
    CHECK(diff_snapshots(snap, snap).empty());

    DeviceMap old, new_;
    old["b"] = dev("b");
    old["x"] = dev("x");
    new_["a"] = dev("a");
    new_["b"] = dev("b");
    new_["y"] = dev("y");
    const auto events = diff_snapshots(old, new_);
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == "added");
    CHECK(events[0].device.path == "a");
    CHECK(events[1].kind == "added");
    CHECK(events[1].device.path == "y");
    CHECK(events[2].kind == "removed");
    CHECK(events[2].device.path == "x");
}

// ---------------------------------------------------------------------------
// watch_devices
// ---------------------------------------------------------------------------

TEST_CASE("watch_devices 输出事件并停止") {
    // 第 1 轮：基线；第 2 轮：插入 a；第 3 轮：拔出 a 插入 b
    std::vector<std::vector<DeviceInfo>> seq = {
        {dev("old")},
        {dev("old"), dev("a")},
        {dev("old"), dev("b")},
    };
    int n = 0;
    EnumerateFn backend = [&] {
        const int idx = std::min(n++, static_cast<int>(seq.size()) - 1);
        return seq[static_cast<std::size_t>(idx)];
    };
    std::vector<DeviceEvent> events;
    std::atomic<bool> stop{false};
    watch_devices([&](const DeviceEvent& e) {
        events.push_back(e);
        if (events.size() >= 2) stop = true;
    }, 0.01, &stop, backend);
    REQUIRE(events.size() == 3);
    CHECK(events[0].kind == "added");
    CHECK(events[0].device.path == "a");
    CHECK(events[1].kind == "added");
    CHECK(events[1].device.path == "b");
    CHECK(events[2].kind == "removed");
    CHECK(events[2].device.path == "a");
}

TEST_CASE("watch_devices 扛得住瞬时枚举错误") {
    int n = 0;
    EnumerateFn backend = [&]() -> std::vector<DeviceInfo> {
        ++n;
        if (n == 1) return {dev("old")};                              // 基线：只有 old
        if (n == 2) throw std::runtime_error("transient usb error");  // 瞬时失败
        return {dev("old"), dev("a")};                                // 第 3 轮起插入 a
    };
    std::vector<DeviceEvent> events;
    std::atomic<bool> stop{false};
    watch_devices([&](const DeviceEvent& e) {
        events.push_back(e);
        stop = true;
    }, 0.01, &stop, backend);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == "added");
    CHECK(events[0].device.path == "a");
    CHECK(n >= 3);  // 出错那轮没中断监听
}

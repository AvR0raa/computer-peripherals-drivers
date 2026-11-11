// battery.hpp — 全设备电量编排（M2.5a）。
// 三条路径，按优先级：
// 1. 标准 HID Battery System 页（usage page 0x85）——零写入，纯读
// 2. HID++ 2.0（罗技：接收器逐槽位 ping / 直连设备 dev=0xFF）
// 3. 无源 → 标记"需学习模式"
// 来源：Solaar (HID++ 电量) + USB HID Usage Tables Battery System page 0x85。
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"
#include "descriptor.hpp"
#include "device_manager.hpp"
#include "hidpp.hpp"
#include "json_util.hpp"
#include "report_io.hpp"

namespace pt {

constexpr int BATTERY_USAGE_PAGE = 0x85;  // HID Battery System

struct BatteryResult {
    std::string vidpid;
    std::string product;
    std::optional<int> percent;
    std::optional<int> voltage_mv;
    std::string source = "none";  // hid-standard / hidpp-* / vendor-* / none
    std::string detail;

    std::string display() const;
};

// 描述符里可读电量字段：Battery System 页(0x85) + EZ2.4G 实测 usage 对。
std::vector<ReportField> find_battery_fields(const Bytes& descriptor);

// 按字段逻辑范围缩放到 0-100；无范围或已近似百分比则原样钳制。
std::optional<int> scale_percent(const ReportField& field, int raw);

// 读标准 HID 电量字段：feature 字段直接读，input 字段等 200ms 推送。
// 返回 (百分比|原始值, 说明)；读不到返回 (空, 原因)。
std::pair<std::optional<int>, std::string> read_standard_battery(
    ReportIO& io, const std::vector<ReportField>& fields);

// 对全部设备做电量扫描。grouped: {vidpid: [DeviceInfo]}（map 有序 = 按 vidpid 遍历）。
// 罗技设备走 HID++（接收器逐槽位 + 直连 0xFF）；其余先扫厂商协议再扫标准电量页。
// factory: 内部 ReportIO/HidppLink 的设备工厂（测试注入假设备）。
std::vector<BatteryResult> scan_all(
    const std::map<std::string, std::vector<DeviceInfo>>& grouped,
    const ReadDescriptorFn& read_descriptor,
    const std::set<std::string>& hidpp_vids = {"046D"},
    DevFactory factory = hidapi_factory());

}  // namespace pt

// device_manager.hpp — 设备枚举与热插拔监听（M1 只读，不打开设备）。
// 枚举依赖 hidapi（hid_api 层）；热插拔用"快照差分轮询"实现，跨平台无原生依赖。
// 差分逻辑是纯函数，可离线单元测试。
#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"

namespace pt {

// hidapi 调用失败（全模块公共 HID 异常；report_io/hidpp 复用）
struct HIDError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct DeviceInfo {
    // 一个 HID 接口（不是物理设备：一个鼠标可能枚举出多个接口）
    std::string path;             // 平台设备路径（同一快照内的唯一键）
    unsigned vendor_id = 0;
    unsigned product_id = 0;
    std::string serial_number;
    unsigned release_number = 0;  // BCD 版本号
    std::string manufacturer;
    std::string product;
    unsigned usage_page = 0;
    unsigned usage = 0;
    int interface_number = 0;
    int bus_type = 0;

    std::string vid_hex() const { return hex(vendor_id, 4); }
    std::string pid_hex() const { return hex(product_id, 4); }
    std::string vidpid() const { return vid_hex() + ":" + pid_hex(); }
};

// hidapi 原始枚举项 → DeviceInfo（对应 Python from_hid_dict）。
namespace hid { struct RawInfo; }
DeviceInfo device_from_raw(const hid::RawInfo& r);

struct DeviceEvent {
    std::string kind;      // "added" | "removed"
    DeviceInfo device;
};

using DeviceMap = std::map<std::string, DeviceInfo>;  // path → 设备（key 有序，差分输出稳定）

// 枚举后端注入点：空 = 真 hidapi；测试替换为假枚举（等价 Python monkeypatch hid.enumerate）。
using EnumerateFn = std::function<std::vector<DeviceInfo>()>;

// 枚举当前全部 HID 接口。失败抛 HIDError。
std::vector<DeviceInfo> list_devices(const EnumerateFn& backend = {});

// 按 VID/PID 过滤（可只给其一）；devices 缺省时重新枚举。
std::vector<DeviceInfo> find_devices(std::optional<unsigned> vendor_id = std::nullopt,
                                     std::optional<unsigned> product_id = std::nullopt,
                                     const std::vector<DeviceInfo>* devices = nullptr,
                                     const EnumerateFn& backend = {});

// 当前枚举结果转成 {path: DeviceInfo}。
DeviceMap snapshot(const EnumerateFn& backend = {});

// 纯函数：比较两个快照，产出 added/removed 事件。
// added 在前、removed 在后，各自按 path 排序（map 天然有序），保证输出稳定。
std::vector<DeviceEvent> diff_snapshots(const DeviceMap& old, const DeviceMap& new_);

// 阻塞式热插拔监听：每 interval 秒枚举一次做差分。
// on_event 对每个事件同步回调；单次枚举失败（插拔瞬间）跳过本轮不中断。
// stop 置位后循环退出；backend 同 list_devices。
void watch_devices(const std::function<void(const DeviceEvent&)>& on_event,
                   double interval = 1.0,
                   std::atomic<bool>* stop = nullptr,
                   const EnumerateFn& backend = {});

}  // namespace pt

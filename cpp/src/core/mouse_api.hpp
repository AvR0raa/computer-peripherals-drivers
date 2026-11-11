// mouse_api.hpp — 鼠标设置统一 API（模板层，M2.5c）。
// 目标：GUI/CLI 不再直接碰协议字节，只调这一层；新增设备品牌 = 新增一个
// Backend 子类并注册到 backend_registry()，界面零改动。
//
//     auto api = detect(ifaces);             // 自动选后端（罗技/模板）
//     api->capabilities()                    // MouseCapabilities(档位/报告率/电池)
//     api->get_dpi() / api->set_dpi(1600)
//     api->get_poll_rate() / api->set_poll_rate(1000)
//     api->get_battery()
//     api->summary()                         // CLI/GUI 直接渲染的 Json
#pragma once
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "battery.hpp"
#include "common.hpp"
#include "device_manager.hpp"
#include "dpi.hpp"
#include "hidpp.hpp"
#include "json_util.hpp"

namespace pt {

struct MouseCapabilities {
    std::vector<int> dpi_list;
    bool dpi_support = false;
    std::vector<int> poll_rates;      // Hz，从高到低
    bool poll_support = false;
    bool battery_support = false;
    std::vector<std::string> notes;
};

class MouseBackend {
public:
    virtual ~MouseBackend() = default;
    virtual bool matches(const std::string& vid_hex,
                         const std::vector<DeviceInfo>& ifaces) const = 0;
    virtual void build(const std::vector<DeviceInfo>& ifaces) = 0;
    virtual MouseCapabilities capabilities() = 0;
    virtual DpiRead get_dpi() = 0;  // (dpi_x, dpi_y, lod, default)
    virtual DpiOutcome set_dpi(int value) = 0;
    virtual std::optional<int> get_poll_rate() = 0;
    virtual std::pair<bool, std::string> set_poll_rate(int hz) = 0;
    virtual std::optional<BatteryResult> get_battery() = 0;
    virtual std::string brand() const = 0;
};

// 罗技 HID++ 2.0 后端（0x2201/0x2202 DPI + 0x8060/0x8061 报告率 + 电量三路）。
class LogitechBackend : public MouseBackend {
public:
    bool matches(const std::string& vid_hex,
                 const std::vector<DeviceInfo>& ifaces) const override;
    void build(const std::vector<DeviceInfo>& ifaces) override;
    MouseCapabilities capabilities() override;
    DpiRead get_dpi() override;
    DpiOutcome set_dpi(int value) override;
    std::optional<int> get_poll_rate() override;
    std::pair<bool, std::string> set_poll_rate(int hz) override;
    std::optional<BatteryResult> get_battery() override;
    std::string brand() const override { return "Logitech"; }

private:
    std::optional<int> dev();       // 惰性探测：0xFF=有线直连 / 1-7=接收器槽位
    bool online(int attempts = 10);

    std::unique_ptr<HidppLink> link_;
    std::optional<int> dev_resolved_;
    std::vector<int> dpi_cache_;
};

// 后端注册表：新增品牌 = 加一个 Backend 工厂 + 一行注册（模板化扩展点）。
using BackendFactory = std::function<std::unique_ptr<MouseBackend>()>;
std::vector<BackendFactory>& backend_registry();

class MouseApi {
public:
    explicit MouseApi(std::unique_ptr<MouseBackend> backend) : backend_(std::move(backend)) {}

    std::string brand() const { return backend_->brand(); }
    MouseCapabilities capabilities() { return backend_->capabilities(); }
    DpiRead get_dpi() { return backend_->get_dpi(); }

    DpiOutcome set_dpi(int value) {
        if (value < 50 || value > 32000) {
            DpiOutcome out;
            out.log.push_back("拒绝: DPI " + std::to_string(value) + " 超出安全范围");
            return out;
        }
        return backend_->set_dpi(value);
    }

    std::optional<int> get_poll_rate() { return backend_->get_poll_rate(); }
    std::pair<bool, std::string> set_poll_rate(int hz) { return backend_->set_poll_rate(hz); }

    // 后端不支持电量时走通用编排（标准页/厂商协议）。
    std::optional<BatteryResult> get_battery(
        const std::vector<DeviceInfo>* all_ifaces = nullptr);

    // CLI/GUI 直接渲染的 Json：{brand, dpi_list, poll_rates, battery_support, notes}
    Json summary();

private:
    std::unique_ptr<MouseBackend> backend_;
};

// 探测设备可用的鼠标设置后端；不支持返回空。
std::unique_ptr<MouseApi> detect(const std::vector<DeviceInfo>& ifaces);

}  // namespace pt

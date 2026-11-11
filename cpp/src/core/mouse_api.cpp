// mouse_api.cpp — 统一鼠标 API 实现（core/mouse_api.py 平移）。
#include "mouse_api.hpp"

namespace pt {

// ---------------------------------------------------------------------------
// LogitechBackend
// ---------------------------------------------------------------------------

bool LogitechBackend::matches(const std::string& vid_hex,
                              const std::vector<DeviceInfo>& ifaces) const {
    if (vid_hex != "046D") return false;
    for (const auto& d : ifaces)
        if (hex(d.usage_page, 4) == "FF00") return true;
    return false;
}

void LogitechBackend::build(const std::vector<DeviceInfo>& ifaces) {
    // 只建链路，不猜设备索引——有线直连(0xFF)与接收器槽位在首次使用时探测。
    const DeviceInfo* short_if = nullptr;
    const DeviceInfo* long_if = nullptr;
    for (const auto& d : ifaces) {
        if (hex(d.usage_page, 4) == "FF00" && hex(d.usage, 4) == "0001") short_if = &d;
        if (hex(d.usage_page, 4) == "FF00" && hex(d.usage, 4) == "0002") long_if = &d;
    }
    link_ = std::make_unique<HidppLink>(short_if ? short_if->path : std::string(),
                                        long_if ? long_if->path : std::string());
    dev_resolved_.reset();
}

std::optional<int> LogitechBackend::dev() {
    // 自动探测 HID++ 设备索引：先 0xFF（有线直连/接收器自身），再槽位 1-7。
    // 有线鼠标（如 PRO X2 有线模式 C09B）直插时必须走 0xFF；无线配对设备走槽位。
    if (dev_resolved_) return dev_resolved_;
    if (!link_) return std::nullopt;
    for (int cand : {0xFF, 1, 2, 3, 4, 5, 6, 7}) {
        const auto [state, version] = ping(*link_, cand, 66, 400);
        if (state == "2.0" || state == "1.0") {
            dev_resolved_ = cand;
            return cand;
        }
    }
    return std::nullopt;
}

bool LogitechBackend::online(int attempts) {
    if (!link_) return false;
    return ensure_online(*link_, dev().value_or(1), attempts).has_value();
}

MouseCapabilities LogitechBackend::capabilities() {
    MouseCapabilities caps;
    if (!link_) return caps;
    link_->open();
    struct LinkCloser {
        HidppLink& l;
        ~LinkCloser() { l.close(); }
    } closer{*link_};
    dev_resolved_.reset();
    const auto dev_idx = dev();
    if (!dev_idx) {
        caps.notes.push_back("接收器上没有配对/在线设备（睡眠时请移动鼠标，HID++ 流量会唤醒）");
        return caps;
    }
    if (!online()) {
        caps.notes.push_back("设备不在线（睡眠时请移动鼠标，HID++ 流量会唤醒）");
        return caps;
    }
    dpi_cache_ = get_dpi_list(*link_, dev_idx.value_or(1));
    caps.dpi_list = dpi_cache_;
    caps.dpi_support = !caps.dpi_list.empty();
    caps.poll_rates = get_poll_rates(*link_, dev_idx.value_or(1));
    caps.poll_support = !caps.poll_rates.empty();
    if (!caps.dpi_support)
        caps.notes.push_back("未发现 DPI feature（可能开启板载配置档，需 G HUB 切 Host 模式）");
    return caps;
}

DpiRead LogitechBackend::get_dpi() {
    if (!link_) return {};
    link_->open();
    struct LinkCloser {
        HidppLink& l;
        ~LinkCloser() { l.close(); }
    } closer{*link_};
    return read_dpi(*link_, dev().value_or(1));
}

DpiOutcome LogitechBackend::set_dpi(int value) {
    if (!link_) return {};
    DpiOutcome out;
    link_->open();
    {
        struct LinkCloser {
            HidppLink& l;
            ~LinkCloser() { l.close(); }
        } closer{*link_};
        out = pt::set_dpi(*link_, dev().value_or(1), value);
    }
    return out;
}

std::optional<int> LogitechBackend::get_poll_rate() {
    if (!link_) return std::nullopt;
    link_->open();
    struct LinkCloser {
        HidppLink& l;
        ~LinkCloser() { l.close(); }
    } closer{*link_};
    return read_poll_rate(*link_, dev().value_or(1));
}

std::pair<bool, std::string> LogitechBackend::set_poll_rate(int hz) {
    if (!link_) return {false, "无链路"};
    link_->open();
    struct LinkCloser {
        HidppLink& l;
        ~LinkCloser() { l.close(); }
    } closer{*link_};
    return pt::set_poll_rate(*link_, dev().value_or(1), hz);
}

std::optional<BatteryResult> LogitechBackend::get_battery() {
    if (!link_) return std::nullopt;
    std::optional<HidppBattery> b;
    link_->open();
    {
        struct LinkCloser {
            HidppLink& l;
            ~LinkCloser() { l.close(); }
        } closer{*link_};
        b = read_battery(*link_, dev().value_or(1));
    }
    if (!b) return std::nullopt;
    BatteryResult r;
    r.percent = b->percent;
    r.voltage_mv = b->voltage_mv;
    r.source = b->source;
    r.detail = b->detail;
    return r;
}

// ---------------------------------------------------------------------------
// 注册表与统一入口
// ---------------------------------------------------------------------------

std::vector<BackendFactory>& backend_registry() {
    static std::vector<BackendFactory> registry = [] {
        std::vector<BackendFactory> r;
        r.push_back([] { return std::unique_ptr<MouseBackend>(std::make_unique<LogitechBackend>()); });
        return r;
    }();
    return registry;
}

std::unique_ptr<MouseApi> detect(const std::vector<DeviceInfo>& ifaces) {
    if (ifaces.empty()) return nullptr;
    const std::string vid_hex = ifaces[0].vid_hex();
    for (const auto& make : backend_registry()) {
        auto backend = make();
        if (backend->matches(vid_hex, ifaces)) {
            backend->build(ifaces);
            return std::make_unique<MouseApi>(std::move(backend));
        }
    }
    return nullptr;
}

std::optional<BatteryResult> MouseApi::get_battery(const std::vector<DeviceInfo>* all_ifaces) {
    if (auto result = backend_->get_battery()) return result;
    // 后端不支持时走通用编排（标准页/厂商协议）
    if (all_ifaces && !all_ifaces->empty()) {
        std::map<std::string, std::vector<DeviceInfo>> grouped;
        grouped[(*all_ifaces)[0].vidpid()] = *all_ifaces;
        const auto results = scan_all(grouped, [](const std::string&) { return Bytes{}; });
        if (!results.empty()) return results[0];
    }
    return std::nullopt;
}

Json MouseApi::summary() {
    const auto caps = capabilities();
    Json s = Json::object();
    s["brand"] = brand();
    Json dpi_list = Json::array();
    for (int v : caps.dpi_list) dpi_list.push_back(v);
    s["dpi_list"] = dpi_list;
    Json rates = Json::array();
    for (int v : caps.poll_rates) rates.push_back(v);
    s["poll_rates"] = rates;
    s["battery_support"] = caps.battery_support;
    Json notes = Json::array();
    for (const auto& n : caps.notes) notes.push_back(n);
    s["notes"] = notes;
    return s;
}

}  // namespace pt

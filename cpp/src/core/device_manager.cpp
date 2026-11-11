// device_manager.cpp — 设备枚举/差分/监听实现（core/device_manager.py 平移）。
#include "device_manager.hpp"

#include "hid_api.hpp"

namespace pt {

DeviceInfo device_from_raw(const hid::RawInfo& r) {
    DeviceInfo d;
    d.path = r.path;
    d.vendor_id = r.vendor_id;
    d.product_id = r.product_id;
    d.serial_number = r.serial_number;
    d.release_number = r.release_number;
    d.manufacturer = r.manufacturer;
    d.product = r.product;
    d.usage_page = r.usage_page;
    d.usage = r.usage;
    d.interface_number = r.interface_number;
    d.bus_type = r.bus_type;
    return d;
}

static std::vector<DeviceInfo> do_list(const EnumerateFn& backend) {
    try {
        if (backend) return backend();
        std::vector<DeviceInfo> out;
        for (const auto& raw : hid::enumerate()) out.push_back(device_from_raw(raw));
        return out;
    } catch (HIDError&) {
        throw;
    } catch (const std::exception& exc) {  // HIDException / OSError 均可能，统一包装
        throw HIDError(std::string("hid.enumerate() 失败: ") + exc.what());
    }
}

std::vector<DeviceInfo> list_devices(const EnumerateFn& backend) {
    return do_list(backend);
}

std::vector<DeviceInfo> find_devices(std::optional<unsigned> vendor_id,
                                     std::optional<unsigned> product_id,
                                     const std::vector<DeviceInfo>* devices,
                                     const EnumerateFn& backend) {
    const std::vector<DeviceInfo> pool = devices ? *devices : do_list(backend);
    std::vector<DeviceInfo> out;
    for (const auto& d : pool) {
        if (vendor_id && d.vendor_id != *vendor_id) continue;
        if (product_id && d.product_id != *product_id) continue;
        out.push_back(d);
    }
    return out;
}

DeviceMap snapshot(const EnumerateFn& backend) {
    DeviceMap map;
    for (auto& d : do_list(backend)) map[d.path] = std::move(d);
    return map;
}

std::vector<DeviceEvent> diff_snapshots(const DeviceMap& old, const DeviceMap& new_) {
    std::vector<DeviceEvent> events;
    for (const auto& [path, dev] : new_)
        if (!old.count(path)) events.push_back({"added", dev});
    for (const auto& [path, dev] : old)
        if (!new_.count(path)) events.push_back({"removed", dev});
    return events;
}

void watch_devices(const std::function<void(const DeviceEvent&)>& on_event,
                   double interval, std::atomic<bool>* stop, const EnumerateFn& backend) {
    std::atomic<bool> local_stop{false};
    if (!stop) stop = &local_stop;

    // C++17 无 atomic::wait_for：用小步睡眠轮询 stop（粒度 25ms，轮询间隔秒级无影响）
    const auto wait = [&](std::chrono::milliseconds total) {
        const std::chrono::milliseconds step{25};
        auto waited = std::chrono::milliseconds{0};
        while (waited < total) {
            if (stop->load()) return true;
            std::this_thread::sleep_for(step);
            waited += step;
        }
        return stop->load();
    };

    DeviceMap old = snapshot(backend);
    const auto interval_ms = std::chrono::milliseconds(static_cast<long long>(interval * 1000));
    while (!wait(interval_ms)) {
        DeviceMap new_;
        try {
            new_ = snapshot(backend);
        } catch (const HIDError&) {
            continue;  // 瞬时枚举失败：保留旧快照，下轮重试
        }
        for (const auto& event : diff_snapshots(old, new_)) on_event(event);
        old = std::move(new_);
    }
}

}  // namespace pt

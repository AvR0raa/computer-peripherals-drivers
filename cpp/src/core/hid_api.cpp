// hid_api.cpp — hidapi 薄封装实现（仅此文件接触 C API）。
#include "hid_api.hpp"

#include <windows.h>  // WideCharToMultiByte（hidapi.h 自身不含 windows.h）
#include <hidapi.h>

#include "device_manager.hpp"  // HIDError（模块公共异常定义于此）

namespace pt::hid {

namespace {

// hidapi Windows 版的设备字符串是 wchar_t*；统一转 UTF-8 存入 std::string。
std::string wide_to_utf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(n) - 1);  // 去掉结尾 \0
    return out;
}

}  // namespace

std::vector<RawInfo> enumerate() {
    // 无设备/枚举不可用均返回空表，与 Python hid.enumerate() 行为一致
    hid_device_info* list = hid_enumerate(0x0000, 0x0000);
    std::vector<RawInfo> out;
    for (hid_device_info* d = list; d; d = d->next) {
        RawInfo r;
        r.path = d->path ? d->path : "";
        r.vendor_id = d->vendor_id;
        r.product_id = d->product_id;
        r.serial_number = wide_to_utf8(d->serial_number);
        r.release_number = d->release_number;
        r.manufacturer = wide_to_utf8(d->manufacturer_string);
        r.product = wide_to_utf8(d->product_string);
        r.usage_page = d->usage_page;
        r.usage = d->usage;
        r.interface_number = d->interface_number;
        r.bus_type = d->bus_type;
        out.push_back(std::move(r));
    }
    hid_free_enumeration(list);
    return out;
}

void HandleDeleter::operator()(hid_device* dev) const {
    if (dev) hid_close(dev);
}

Handle open_path(const std::string& path) {
    hid_device* dev = hid_open_path(path.c_str());
    if (!dev)
        throw HIDError("打开设备失败 (" + path + ")");
    return Handle(dev);
}

int write(hid_device* dev, const Bytes& data) {
    return hid_write(dev, data.data(), static_cast<std::size_t>(data.size()));
}

int read_timeout(hid_device* dev, std::uint8_t* buf, int size, int timeout_ms) {
    return hid_read_timeout(dev, buf, static_cast<std::size_t>(size), timeout_ms);
}

int send_feature(hid_device* dev, const Bytes& data) {
    return hid_send_feature_report(dev, data.data(), static_cast<std::size_t>(data.size()));
}

int get_feature(hid_device* dev, std::uint8_t* buf, int size) {
    return static_cast<int>(
        hid_get_feature_report(dev, buf, static_cast<std::size_t>(size)));
}

Bytes get_report_descriptor(hid_device* dev) {
    std::uint8_t buf[4096];
    const int n = hid_get_report_descriptor(dev, buf, sizeof buf);
    if (n < 0) return {};
    return Bytes(buf, buf + n);
}

}  // namespace pt::hid

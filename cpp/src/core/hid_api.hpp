// hid_api.hpp — hidapi C 库的极薄 RAII 封装。
// 全项目唯一允许包含 <hidapi.h> 的文件；其余模块通过本层访问设备。
// 语义与 Python 的 hid 模块（hidapi 绑定）一一对应。
#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "common.hpp"

struct hid_device_;              // hidapi 前置声明（hidapi.h 内即此拼法：tag 为 hid_device_）
typedef struct hid_device_ hid_device;

namespace pt::hid {

struct RawInfo {          // hid_device_info 的 std::string 版（测试可手工构造）
    std::string path;     // 平台设备路径
    unsigned short vendor_id = 0, product_id = 0;
    std::string serial_number, manufacturer, product;
    unsigned short release_number = 0, usage_page = 0, usage = 0;
    int interface_number = 0, bus_type = 0;
};

// 枚举全部 HID 接口。hidapi 异常时抛 HIDError。
std::vector<RawInfo> enumerate();

// hid_device 的 RAII 句柄（hid_close 为删除器，杜绝泄漏）。
struct HandleDeleter {
    void operator()(hid_device*) const;
};
using Handle = std::unique_ptr<hid_device, HandleDeleter>;

// 打开设备路径。失败抛 HIDError。
Handle open_path(const std::string& path);

// ---- 以下直接透传 hidapi 语义，返回值含义同 hidapi ----
// write: 成功返回写入字节数（含 report id），失败返回 -1
int write(hid_device* dev, const Bytes& data);
// 阻塞读 input report（不剥 report id），返回实际字节数，超时返回 0
int read_timeout(hid_device* dev, std::uint8_t* buf, int size, int timeout_ms);
// 写 feature report，失败返回 -1
int send_feature(hid_device* dev, const Bytes& data);
// 读 feature report（buf[0] 须为 report_id），失败返回 -1，返回实际字节数
int get_feature(hid_device* dev, std::uint8_t* buf, int size);
// 读报告描述符（Windows: hid_get_report_descriptor）。失败返回空。
Bytes get_report_descriptor(hid_device* dev);

}  // namespace pt::hid

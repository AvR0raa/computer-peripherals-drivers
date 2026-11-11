// probe.hpp — vendor 配置通道只读探测（M5 多设备测试支持）。
// 安全边界：只发 get_feature_report（读取），绝不写入。
// 对未知/未适配设备的 vendor 自定义接口，读出描述符里声明的 feature
// report 原始内容 —— 这就是配置通道的快照，也是 M4 学习模式的输入。
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "descriptor.hpp"
#include "device_manager.hpp"
#include "report_io.hpp"

namespace pt {

// 描述符里声明的 feature report ID（去重保序；无 feature 字段时返回 [0]）。
std::vector<int> feature_report_ids(const DescriptorInfo& info);

struct ProbeResult {
    int usage_page = 0;
    int usage = 0;
    std::optional<int> descriptor_len;                   // 描述符读取失败时为空
    std::vector<int> feature_ids;
    std::map<std::string, std::optional<Bytes>> reads;   // "3"->字节 | "input_passive"->字节
    std::optional<std::string> error;
    std::optional<std::string> note;
};

// 探测单个接口：读描述符 → 找 feature report ID → 逐个读原始内容。
ProbeResult probe_interface(const std::string& path, int usage_page, int usage,
                            const ReadDescriptorFn& read_descriptor,
                            DevFactory factory = hidapi_factory());

// 只挑 vendor 自定义页（>=0xFF00）的接口做探测；没有则空列表。
std::vector<ProbeResult> probe_vendor_interfaces(const std::vector<DeviceInfo>& ifaces,
                                                 const ReadDescriptorFn& read_descriptor,
                                                 DevFactory factory = hidapi_factory());

// 识别 rawhid 风格配置通道：usage=0x0000 且带 64 字节 input+output。
// 国产键盘（ROYUAN/迈从/美嘉獅等 OEM 方案）最常见的配置接口形态。
// 只做被动 input 读取（零风险），不写入任何报文。
std::vector<ProbeResult> suspect_rawhid_interfaces(const std::vector<DeviceInfo>& ifaces,
                                                   const ReadDescriptorFn& read_descriptor,
                                                   DevFactory factory = hidapi_factory());

}  // namespace pt

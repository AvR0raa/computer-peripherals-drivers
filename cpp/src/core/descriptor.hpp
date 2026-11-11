// descriptor.hpp — HID 报告描述符解析（纯逻辑，无 I/O）。
// 解析 USB HID Report Descriptor 条目流（main/global/local items），
// 产出字段列表与集合树。参考规范：USB HID 1.11 §6.2.2。
// 契约：core/descriptor.py 的 1:1 平移，字段语义见 ARCHITECTURE.md。
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common.hpp"

namespace pt {

struct DescriptorError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ReportField {
    std::string report_type;               // "input" | "output" | "feature"
    int report_id = 0;                     // 0 = 描述符未使用 Report ID
    int offset = 0;                        // 报文体（不含 ID 字节）内 bit 偏移
    int size = 0;                          // 每个 item 的 bit 数（Report Size）
    int count = 0;                         // item 重复次数（Report Count）
    std::optional<std::uint32_t> usage;    // 第一个 32 位 usage；无数值用法但有范围时 = usage_min
    std::vector<std::uint32_t> usages;     // 全部已解析 usage（按声明顺序）
    std::optional<std::uint32_t> usage_min;  // 数组型字段范围（已按当前 Usage Page 解析）
    std::optional<std::uint32_t> usage_max;
    std::optional<int> logical_min;        // 逻辑范围（电量值缩放用）
    std::optional<int> logical_max;
    int flags = 0;                         // 主条目原始 flags（bit0 Data/Const, bit1 Array/Var, bit2 Abs/Rel）
    bool is_constant = false;              // True = 填充/常量位
};

struct CollectionNode;
// 集合子节点：字段或子集合。unique_ptr 承担所有权，杜绝裸指针。
using Child = std::variant<ReportField, std::unique_ptr<CollectionNode>>;

struct CollectionNode {
    std::string kind;                      // "Application"/"Physical"/... 未知 → "0x07"
    std::optional<std::uint32_t> usage;    // 声明集合时的第一个 usage
    std::vector<Child> children;
    ~CollectionNode();                     // 递归类型：析构放 .cpp（类型完整处）
};

struct DescriptorInfo {
    std::set<std::uint32_t> usage_pages;             // 出现过的 Usage Page
    std::vector<std::uint32_t> application_usages;   // 顶层集合 usage（顺序保留）
    std::vector<std::unique_ptr<CollectionNode>> collections;
    std::vector<ReportField> fields;                 // 全部字段（含常量填充位）
    bool has_vendor_page = false;                    // 含 0xFF00+ 厂商自定义页
    std::map<std::pair<std::string, int>, int> report_bits;  // (type, id) -> 报文体总 bit 数
};

// 解析原始描述符字节流。支持短条目（0/1/2/4 字节数据）、长条目（0xFE 跳过）、
// Report ID、Push/Pop 全局栈。语法错误抛 DescriptorError（带出错偏移）。
DescriptorInfo parse_report_descriptor(const Bytes& data);

// 人读摘要行（CLI 逐行打印）。
std::vector<std::string> summary(const DescriptorInfo& info);

// 32 位 usage → "0x00010002 (Mouse)" 风格人读字符串。
std::string format_usage(std::uint32_t usage);

}  // namespace pt

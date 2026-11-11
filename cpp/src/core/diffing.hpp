// diffing.hpp — 学习模式差分分析（任务 4.2）。
// 对比两次录制（改配置前/后），按 (source, kind, report_id) 分组，
// 取各组最后一条记录做逐字节对比，自动标出变化的字节位置和值。
// 对应 plan.md 验收："改DPI前后"自动指出第4字节 0x20→0x40。
#pragma once
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "common.hpp"
#include "recorder.hpp"

namespace pt {

using GroupKey = std::tuple<std::string, std::string, int>;  // (source, kind, report_id)

struct ByteChange {
    int offset = 0;
    std::optional<int> before;  // 空 = before 缺该字节（长度变化/新组）
    std::optional<int> after;
};

struct GroupDiff {
    std::string source;
    std::string kind;
    int report_id = 0;
    int before_len = 0;
    int after_len = 0;
    std::vector<ByteChange> changes;
    bool is_new = false;       // before 中无此组（改配置后才开始出现的报文）
    bool is_volatile = false;  // 疑似数据流噪声（同一组内容五花八门，如移动数据）
};

// 返回内容种类 >= threshold 的组（疑似持续数据流，如鼠标移动）。
// 这些组的前后"差异"大概率是噪声而非配置变化，差分结果里仅做标记。
std::set<GroupKey> volatile_groups(const std::vector<CaptureEntry>& entries, int threshold = 8);

// 按组最后一条记录做字节级对比。变化组按 (source, kind, report_id) 排序。
std::vector<GroupDiff> diff_sessions(const std::vector<CaptureEntry>& before,
                                     const std::vector<CaptureEntry>& after);

// 人读格式（GUI/CLI 直接逐行展示）。
std::vector<std::string> format_diff(const GroupDiff& diff);

}  // namespace pt

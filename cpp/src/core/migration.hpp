// migration.hpp — 配置迁移（M-C6，plan.md §6）。
//
// 场景：用户在旧鼠标上调好的设置（DPI/报告率），换新鼠标（甚至换品牌）后
// 自动把"意图"迁移过去。核心是 **intent 翻译**：搬运的是设备无关的意图值，
// 不做原始字节搬运。
//
// 规则（plan §6）：
// 1. 能力交集迁移：意图值在新设备档位表中 → 直接用；否则就近取档
// 2. 类型守卫：仅鼠标设置参与（MouseApi 本身即鼠标层，守卫天然成立）
// 3. 回滚保底：执行走 MouseApi.set_dpi/set_poll_rate（内建读回验证+回滚）
// 4. 人工确认：先出 MigrationPlan 预览，确认后再 execute
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "mouse_api.hpp"
#include "profiles.hpp"

namespace pt::migration {

// 设备无关的意图层（旧设备"调好的东西"）。
struct DeviceIntent {
    std::optional<int> dpi;
    std::optional<int> poll_rate;    // Hz
    // RGB / 按键等后续扩展（需要对应能力描述后接入）
};

// 从旧设备的"当前设置"采集意图（MouseApi 读取值 → 意图）。
DeviceIntent intent_from_read(const DpiRead& dpi, std::optional<int> poll_rate);

// 从旧设备的档案采集意图（profile features 里带 dpi/poll 语义的参数）。
// 没有可识别项返回空意图。
DeviceIntent intent_from_profile(const DeviceProfile& profile);

// 就近取档：supported 中与 target 距离最小的值；并列取较小值（保守）。
// supported 为空返回 target 本身（调用方应先判空）。
int nearest(const std::vector<int>& supported, int target);

struct PlannedWrite {
    std::string setting;    // "dpi" | "poll_rate"
    int from = 0;           // 意图值
    int to = 0;             // 就近取档后实际将写入的值
};

struct MigrationPlan {
    std::vector<PlannedWrite> writes;   // 能落地的迁移项（预览给用户确认）
    std::vector<std::string> skipped;   // 未迁移项及原因
};

// 生成迁移计划（纯函数，不碰设备）：意图 ∩ 新设备能力，就近取档。
MigrationPlan plan_migration(const DeviceIntent& intent, const MouseCapabilities& caps);

struct ExecutedWrite {
    std::string setting;
    bool ok = false;
    std::string msg;
};

// 执行迁移：按计划逐项走 MouseApi 安全写（set_dpi/set_poll_rate 内建读回+回滚）。
std::vector<ExecutedWrite> execute_migration(MouseApi& api, const MigrationPlan& plan);

}  // namespace pt::migration

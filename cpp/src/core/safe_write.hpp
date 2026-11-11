// safe_write.hpp — 写命令安全机制（任务 2.4）。
// 原则（plan.md 风险表）：全部写命令走"读旧值 → 写新值 → 读回验证"，
// 验证失败且手上有旧值时立即回滚旧值再读回；全程记录完整 hex 日志。
// 两条路径：
//   report == "feature" : send_feature + get_feature 自读自验（无需 read 功能）
//   report == "output"  : 需配对 read 功能，用 execute_feature 读前后值
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "json_util.hpp"
#include "report_io.hpp"

namespace pt {

struct WriteOutcome {
    bool ok = false;                    // 写流程完成（读回拿到响应且验证通过/无 expect）
    bool verified = false;              // 读回与 expect 一致；无 expect 时恒为 False（不可验证）
    std::optional<Bytes> wrote;
    std::optional<Bytes> readback;
    std::optional<Bytes> old_value;
    bool rolled_back = false;
    std::vector<std::string> log;       // 每步一行，含完整 hex
};

// expect: {字节索引: 期望值}，读回比对用。
using ByteExpect = std::map<int, int>;

// 执行一次带安全网的写。
// expect 给了才具备回滚触发条件（没有期望值就无法判断写错）。
// rollback_params: output 路径回滚时的旧值参数（feature 路径直接回写旧字节流）。
WriteOutcome safe_write(ReportIO& io, const Json& write_feature,
                        const Json* read_feature = nullptr,
                        const Params* new_params = nullptr,
                        const ByteExpect* expect = nullptr,
                        const Params* rollback_params = nullptr,
                        int timeout_ms = 600);

}  // namespace pt

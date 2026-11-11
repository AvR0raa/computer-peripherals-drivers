// wizard.hpp — 学习模式引导入库（任务 4.3）。
// 差分结果 → 协议草稿（kind=read 监听/快照型，标记 learned）→ 合并进 db。
// 草稿一律 unverified：条目带 "learned" 佐证块（来源接口、变化字节），
// 用户真机验证后再由后续任务翻转 verified。
// 安全边界：只生成"读/监听"型功能，永不生成写命令。
#pragma once
#include <string>
#include <vector>

#include "diffing.hpp"
#include "json_util.hpp"

namespace pt {

// 学习草稿功能 id 前缀（合并入库时据此识别学习产物）
inline constexpr const char* LEARNED_PREFIX = "learned_";

// 把差分结果转成协议功能草稿。
// label_match: {录制目标 label: {"usage_page": "FF21", "usage": "0001"}}
//              —— 用于给草稿写 feature 级 match。
// 跳过 volatile 组（数据流噪声不能当配置协议）。
Json build_drafts(const std::vector<GroupDiff>& diffs, const std::string& note = "",
                  const Json& label_match = Json(), int max_changes = 64);

// 组装一个设备的 protocol 块。transport.match 取传入 match 或占位。
Json build_protocol_block(const Json& match, const std::vector<Json>& drafts);

// 把学习草稿合并进 db["devices"][vid_pid]，返回该设备条目。
// - 新设备：创建条目（brand 未知留空、source=learning-mode、verified=False）
// - 已有 protocol：按 feature id 合并（同 id 覆盖，其余追加），
//   不动 protocol["name"]，不降级已有 verified=True
Json& merge_into_db(Json& db, const std::string& vid_pid, const Json& protocol_block,
                    const std::string& note = "");

}  // namespace pt

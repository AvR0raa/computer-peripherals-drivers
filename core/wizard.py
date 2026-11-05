"""学习模式引导入库（任务 4.3）。

差分结果 → 协议草稿（kind=read 监听/快照型，标记 learned）→ 合并进 db。
草稿一律 unverified：条目带 "learned" 佐证块（来源接口、变化字节），
用户真机验证后再由后续任务翻转 verified。

安全边界：只生成"读/监听"型功能，永不生成写命令。
"""
from __future__ import annotations

from core.diffing import GroupDiff

# 学习草稿功能 id 前缀（合并入库时据此识别学习产物）
LEARNED_PREFIX = "learned_"


def build_drafts(
    diffs: list[GroupDiff],
    note: str = "",
    label_match: dict | None = None,
    max_changes: int = 64,
) -> list[dict]:
    """把差分结果转成协议功能草稿。

    label_match: {录制目标 label: {"usage_page": "FF21", "usage": "0001"}}
                 —— 用于给草稿写 feature 级 match。
    跳过 volatile 组（数据流噪声不能当配置协议）。
    """
    drafts: list[dict] = []
    label_match = label_match or {}
    for d in diffs:
        if d.volatile:
            continue
        if d.kind == "feature_read":
            report = "feature"
            template = [d.report_id]
        elif d.kind == "input":
            report = "input"           # 监听型：零写入
            template = []
        else:
            continue
        changes = [
            [c.offset, c.before, c.after]
            for c in d.changes[:max_changes]
        ]
        min_len = max(d.after_len, 1)
        draft = {
            "id": f"{LEARNED_PREFIX}{d.source.lower().replace(':', '_').replace(' ', '_')}_{d.report_id}",
            "label": note or "学习模式捕获",
            "kind": "read",
            "report": report,
            "template": template,
            "response": {
                "min_len": min_len,
                "check": [None] * min_len,   # 通配：草稿阶段不做严格匹配
            },
            "learned": {
                "source": d.source,
                "changes": changes,
                "is_new": d.is_new,
                "before_len": d.before_len,
                "after_len": d.after_len,
            },
        }
        match = label_match.get(d.source)
        if match:
            draft["match"] = dict(match)
        drafts.append(draft)
    return drafts


def build_protocol_block(match: dict, drafts: list[dict]) -> dict:
    """组装一个设备的 protocol 块。transport.match 取第一个草稿的 match 或占位。"""
    return {
        "name": "learned",
        "transport": {
            "match": dict(match) if match else {"usage_page": "0000", "usage": "0000"},
            "response_timeout_ms": 600,
        },
        "features": drafts,
    }


def merge_into_db(db: dict, vid_pid: str, protocol_block: dict, note: str | None = None) -> dict:
    """把学习草稿合并进 db["devices"][vid_pid]，返回该设备条目。

    - 新设备：创建条目（brand 未知留空、source=learning-mode、verified=False）
    - 已有 protocol：按 feature id 合并（同 id 覆盖，其余追加），
      不动 protocol["name"]，不降级已有 verified=True
    """
    entry = db["devices"].setdefault(vid_pid, {})
    entry.setdefault("brand", "Unknown")
    entry["source"] = "learning-mode"
    if entry.get("verified") is not True:
        entry["verified"] = False
    if note:
        entry["note"] = f"学习模式: {note}"

    existing = entry.get("protocol")
    if existing is None:
        entry["protocol"] = protocol_block
        return entry
    by_id = {f.get("id"): f for f in existing.get("features", [])}
    for feature in protocol_block["features"]:
        by_id[feature["id"]] = feature       # 同 id 覆盖（重学以新为准）
    existing["features"] = list(by_id.values())
    return entry

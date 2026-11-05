"""学习模式差分分析（任务 4.2）。

对比两次录制（改配置前/后），按 (source, kind, report_id) 分组，
取各组最后一条记录做逐字节对比，自动标出变化的字节位置和值。

对应 plan.md 验收："改DPI前后"自动指出第4字节 0x20→0x40。
"""
from __future__ import annotations

from dataclasses import dataclass

from core.recorder import CaptureEntry


@dataclass(frozen=True)
class ByteChange:
    offset: int
    before: int | None      # None = before 缺该字节（长度变化/新组）
    after: int | None


@dataclass(frozen=True)
class GroupDiff:
    source: str
    kind: str
    report_id: int
    before_len: int
    after_len: int
    changes: tuple[ByteChange, ...]
    is_new: bool            # before 中无此组（改配置后才开始出现的报文）
    volatile: bool          # 疑似数据流噪声（同一组内容五花八门，如移动数据）


def _hex_to_bytes(data: str) -> bytes:
    return bytes.fromhex(data.replace(" ", "")) if data else b""


def _last_by_group(entries: list[CaptureEntry]) -> dict:
    last: dict[tuple, CaptureEntry] = {}
    for e in entries:
        if e.kind == "error":
            continue
        last[(e.source, e.kind, e.report_id)] = e
    return last


def volatile_groups(entries: list[CaptureEntry], threshold: int = 8) -> set:
    """返回内容种类 >= threshold 的组（疑似持续数据流，如鼠标移动）。

    这些组的前后"差异"大概率是噪声而非配置变化，差分结果里仅做标记。
    """
    seen: dict[tuple, set] = {}
    for e in entries:
        if e.kind == "error" or not e.data:
            continue
        seen.setdefault((e.source, e.kind, e.report_id), set()).add(e.data)
    return {k for k, values in seen.items() if len(values) >= threshold}


def diff_sessions(
    before: list[CaptureEntry],
    after: list[CaptureEntry],
) -> list[GroupDiff]:
    """按组最后一条记录做字节级对比。变化组按 (source, kind, report_id) 排序。"""
    last_before = _last_by_group(before)
    last_after = _last_by_group(after)
    noise = volatile_groups(before) | volatile_groups(after)

    diffs: list[GroupDiff] = []
    for key, entry_after in sorted(last_after.items()):
        e_before = last_before.get(key)
        a_data = _hex_to_bytes(entry_after.data)
        if e_before is None:
            changes = tuple(ByteChange(i, None, b) for i, b in enumerate(a_data))
            diffs.append(GroupDiff(key[0], key[1], key[2], 0, len(a_data),
                                   changes, is_new=True, volatile=key in noise))
            continue
        b_data = _hex_to_bytes(e_before.data)
        if b_data == a_data:
            continue
        changes = []
        for i in range(max(len(b_data), len(a_data))):
            bv = b_data[i] if i < len(b_data) else None
            av = a_data[i] if i < len(a_data) else None
            if bv != av:
                changes.append(ByteChange(i, bv, av))
        diffs.append(GroupDiff(key[0], key[1], key[2], len(b_data), len(a_data),
                               tuple(changes), is_new=False, volatile=key in noise))
    return diffs


def _byte_text(value: int | None) -> str:
    return "--" if value is None else f"0x{value:02X}"


def format_diff(diff: GroupDiff) -> list[str]:
    """人读格式（GUI/CLI 直接逐行展示）。"""
    head = f"{diff.source} {diff.kind} id={diff.report_id}"
    marks = []
    if diff.is_new:
        marks.append("新报文组")
    if diff.volatile:
        marks.append("疑似噪声流")
    prefix = f"{head}{'（' + '/'.join(marks) + '）' if marks else ''}"
    if diff.is_new:
        return [f"{prefix}: {diff.after_len} 字节",
                f"  内容: {diff.changes and ''}{' '.join(f'{c.after:02X}' for c in diff.changes if c.after is not None)}"]
    lines = [f"{prefix}: {len(diff.changes)} 字节变化 "
             f"（{diff.before_len}B → {diff.after_len}B）"]
    for c in diff.changes[:16]:   # 单组最多展示 16 个变化字节，防刷屏
        lines.append(f"  [{c.offset:3d}] {_byte_text(c.before)} → {_byte_text(c.after)}")
    if len(diff.changes) > 16:
        lines.append(f"  … 还有 {len(diff.changes) - 16} 个变化字节")
    return lines

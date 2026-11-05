"""任务 4.2 单元测试：core/diffing.py（纯函数，离线运行）。"""
from core.diffing import ByteChange, diff_sessions, format_diff, volatile_groups
from core.recorder import CaptureEntry


def entry(ts=1.0, source="FF21", kind="input", report_id=237, data="01 02 03 04"):
    return CaptureEntry(ts=ts, source=source, kind=kind, report_id=report_id, data=data)


# ---------------------------------------------------------------------------
# 核心差分
# ---------------------------------------------------------------------------

def test_byte_change_located():
    """验收场景：改DPI前后自动指出变化字节。"""
    before = [entry(data="0e ed 20 00 00")]
    after = [entry(data="0e ed 40 00 00")]
    diffs = diff_sessions(before, after)
    assert len(diffs) == 1
    d = diffs[0]
    assert not d.is_new and not d.volatile
    assert d.changes == (ByteChange(2, 0x20, 0x40),)


def test_multiple_changes_one_group():
    before = [entry(data="aa 11 22 33")]
    after = [entry(data="aa 11 99 33")]
    diffs = diff_sessions(before, after)
    assert [c.offset for c in diffs[0].changes] == [2]
    # 两个字段一起变
    diffs = diff_sessions(before, [entry(data="bb 11 99 33")])
    assert [(c.offset, c.before, c.after) for c in diffs[0].changes] == [
        (0, 0xAA, 0xBB), (2, 0x22, 0x99)]


def test_unchanged_groups_skipped():
    before = [entry(data="01"), entry(ts=2, source="rawhid", kind="input",
                                       report_id=0, data="02")]
    after = [entry(data="01"), entry(ts=2, source="rawhid", kind="input",
                                     report_id=0, data="02")]
    assert diff_sessions(before, after) == []


def test_new_group_in_after():
    before = []
    after = [entry(kind="feature_read", report_id=170, data="aa bb")]
    diffs = diff_sessions(before, after)
    assert len(diffs) == 1
    d = diffs[0]
    assert d.is_new
    assert d.before_len == 0 and d.after_len == 2
    assert d.changes == (ByteChange(0, None, 0xAA), ByteChange(1, None, 0xBB))


def test_length_change_reported():
    before = [entry(data="01 02 03")]
    after = [entry(data="01 02 03 04")]
    diffs = diff_sessions(before, after)
    assert diffs[0].changes == (ByteChange(3, None, 0x04),)


def test_last_entry_per_group_used():
    """同组多次记录取最后一条（feature_read 手动快照语义）。"""
    before = [entry(ts=1, kind="feature_read", data="01 02"),
              entry(ts=2, kind="feature_read", data="01 09")]
    after = [entry(ts=3, kind="feature_read", data="01 02"),
             entry(ts=4, kind="feature_read", data="01 05")]
    diffs = diff_sessions(before, after)
    assert diffs[0].changes == (ByteChange(1, 0x09, 0x05),)


def test_error_entries_ignored():
    before = [entry(data="01"), entry(ts=2, kind="error", data="boom")]
    after = [entry(data="02")]
    diffs = diff_sessions(before, after)
    assert len(diffs) == 1
    assert diffs[0].changes == (ByteChange(0, 0x01, 0x02),)


def test_empty_data_treated_as_empty_bytes():
    diffs = diff_sessions([entry(data="")], [entry(data="01")])
    assert diffs[0].changes == (ByteChange(0, None, 0x01),)


# ---------------------------------------------------------------------------
# 噪声识别
# ---------------------------------------------------------------------------

def test_volatile_groups_detected():
    stream = [entry(ts=i, data=f"{i:02x}") for i in range(10)]   # 10 种内容
    assert volatile_groups(stream) == {("FF21", "input", 237)}


def test_stable_group_not_volatile():
    stable = [entry(ts=1, data="aa"), entry(ts=2, data="aa")]
    assert volatile_groups(stable) == set()


def test_volatile_marked_in_diff():
    noise = [entry(ts=i, data=f"{i:02x}") for i in range(10)]
    diffs = diff_sessions(noise, noise + [entry(ts=99, data="ff")])
    # 组已被标记 volatile（内容 >= 8 种），仍给出 diff 但带标记
    assert diffs[0].volatile
    lines = format_diff(diffs[0])
    assert "疑似噪声流" in lines[0]


# ---------------------------------------------------------------------------
# 人读格式
# ---------------------------------------------------------------------------

def test_format_diff_normal():
    diffs = diff_sessions([entry(data="0e ed 20")], [entry(data="0e ed 40")])
    lines = format_diff(diffs[0])
    assert lines[0].startswith("FF21 input id=237")
    assert "1 字节变化" in lines[0]
    assert "[  2] 0x20 → 0x40" in lines[1]


def test_format_diff_new_group():
    diffs = diff_sessions([], [entry(kind="feature_read", report_id=170, data="aa bb")])
    lines = format_diff(diffs[0])
    assert "新报文组" in lines[0]
    assert "2 字节" in lines[0]
    assert "AA BB" in lines[1]       # hex 大写展示


def test_format_diff_truncates_many_changes():
    big_before = " ".join(["00"] * 20)
    big_after = " ".join(["ff"] * 20)
    diffs = diff_sessions([entry(data=big_before)], [entry(data=big_after)])
    lines = format_diff(diffs[0])
    assert len(lines) == 18          # 头 + 16 变化 + 省略行
    assert "还有 4 个变化字节" in lines[-1]

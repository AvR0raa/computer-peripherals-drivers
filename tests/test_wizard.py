"""任务 4.3 单元测试：core/wizard.py。"""
from core.diffing import diff_sessions, volatile_groups
from core.fingerprint import validate_db
from core.recorder import CaptureEntry
from core.wizard import (
    LEARNED_PREFIX,
    build_drafts,
    build_protocol_block,
    merge_into_db,
)


def entry(ts=1.0, source="FF21", kind="input", report_id=237, data="01 02"):
    return CaptureEntry(ts=ts, source=source, kind=kind, report_id=report_id, data=data)


LABEL_MATCH = {"FF21": {"usage_page": "FF21", "usage": "0001"}}


def test_draft_from_input_diff():
    diffs = diff_sessions([], [entry(data="0e ed 40 00")])
    drafts = build_drafts(diffs, note="DPI 800→1600", label_match=LABEL_MATCH)
    assert len(drafts) == 1
    d = drafts[0]
    assert d["id"].startswith(LEARNED_PREFIX)
    assert d["kind"] == "read" and d["report"] == "input"
    assert d["template"] == []                      # 监听型零写入
    assert d["match"] == {"usage_page": "FF21", "usage": "0001"}
    assert d["response"]["check"] == [None] * 4     # 通配
    assert d["learned"]["changes"] == [
        [0, None, 0x0E], [1, None, 0xED], [2, None, 0x40], [3, None, 0x00]]
    assert d["learned"]["is_new"] is True
    assert d["label"] == "DPI 800→1600"


def test_draft_from_feature_read_diff():
    diffs = diff_sessions(
        [entry(ts=1, kind="feature_read", report_id=237, data="ed 01 20")],
        [entry(ts=2, kind="feature_read", report_id=237, data="ed 01 40")])
    drafts = build_drafts(diffs, note="改DPI")
    d = drafts[0]
    assert d["report"] == "feature"
    assert d["template"] == [237]
    assert d["learned"]["changes"] == [[2, 0x20, 0x40]]
    assert d["learned"]["is_new"] is False


def test_volatile_diff_skipped():
    noise = [entry(ts=i, data=f"{i:02x}") for i in range(10)]
    diffs = diff_sessions(noise, noise)
    assert build_drafts(diffs) == []


def test_draft_id_sanitized():
    diffs = diff_sessions([], [entry(ts=1, source="41E4:2116 UP:0001", data="aa")])
    drafts = build_drafts(diffs)
    assert drafts[0]["id"] == f"{LEARNED_PREFIX}41e4_2116_up_0001_237"


def test_protocol_block_uses_first_match():
    drafts = build_drafts(
        diff_sessions([], [entry(data="aa")]), label_match=LABEL_MATCH)
    block = build_protocol_block(LABEL_MATCH["FF21"], drafts)
    assert block["name"] == "learned"
    assert block["transport"]["match"] == {"usage_page": "FF21", "usage": "0001"}
    assert block["features"] == drafts


def test_protocol_block_placeholder_match():
    drafts = build_drafts(diff_sessions([], [entry(data="aa")]))
    block = build_protocol_block(None, drafts)
    assert block["transport"]["match"] == {"usage_page": "0000", "usage": "0000"}


def test_merge_into_new_device():
    db = {"$schema": "protocol-db/v2", "brands": {}, "devices": {}}
    drafts = build_drafts(
        diff_sessions([], [entry(data="aa")]), label_match=LABEL_MATCH, note="测试")
    block = build_protocol_block(LABEL_MATCH["FF21"], drafts)
    merged = merge_into_db(db, "291D:385D", block, note="DPI 改动")
    assert merged["source"] == "learning-mode"
    assert merged["verified"] is False
    assert merged["note"] == "学习模式: DPI 改动"
    validate_db(db)                                  # 合并结果必须过校验


def test_merge_into_existing_protocol_merges_features():
    db = {"$schema": "protocol-db/v2", "brands": {}, "devices": {
        "046D:C54D": {
            "brand": "Logitech", "source": "hidapi-observed", "verified": True,
            "protocol": {"name": "hidpp", "transport": {
                "match": {"usage_page": "FF00", "usage": "0001"}},
                "features": [{"id": "ping", "label": "Ping", "kind": "command",
                              "report": "output", "template": [16]}]},
        }}}
    drafts = build_drafts(
        diff_sessions([], [entry(ts=1, source="FF00", data="aa")]),
        label_match={"FF00": {"usage_page": "FF00", "usage": "0001"}})
    block = build_protocol_block({"usage_page": "FF00", "usage": "0001"}, drafts)
    merged = merge_into_db(db, "046D:C54D", block)
    assert merged["protocol"]["name"] == "hidpp"         # 不动已有协议名
    assert merged["verified"] is True                    # 不降级
    ids = [f["id"] for f in merged["protocol"]["features"]]
    assert "ping" in ids and any(i.startswith(LEARNED_PREFIX) for i in ids)
    validate_db(db)


def test_relearn_overrides_same_id():
    db = {"$schema": "protocol-db/v2", "brands": {}, "devices": {}}
    drafts1 = build_drafts(diff_sessions([], [entry(ts=1, data="aa")]), note="第一次")
    merge_into_db(db, "291D:385D", build_protocol_block(None, drafts1))
    dev_entry = db["devices"]["291D:385D"]
    old_id = dev_entry["protocol"]["features"][0]["id"]
    drafts2 = build_drafts(diff_sessions([], [entry(ts=2, data="bb")]), note="第二次")
    merge_into_db(db, "291D:385D", build_protocol_block(None, drafts2))
    dev_entry = db["devices"]["291D:385D"]
    assert len(dev_entry["protocol"]["features"]) == 1       # 同 id 覆盖不重复
    assert dev_entry["protocol"]["features"][0]["label"] == "第二次"
    assert old_id == dev_entry["protocol"]["features"][0]["id"]

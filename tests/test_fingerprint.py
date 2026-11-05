"""任务 1.3 + M2.1 单元测试：core/fingerprint.py + 真实 db.json 完整性。"""
import json

import pytest

from core.device_manager import DeviceInfo
from core.fingerprint import (
    DEFAULT_DB_PATH,
    DeviceStatus,
    FingerprintResult,
    ProtocolDBError,
    fingerprint,
    load_db,
    save_db,
    validate_db,
)


def device(vid=0x046D, pid=0xC54D) -> DeviceInfo:
    return DeviceInfo(
        path="\\\\?\\fake", vendor_id=vid, product_id=pid, serial_number="",
        release_number=0, manufacturer="", product="", usage_page=1, usage=2,
        interface_number=0, bus_type=1,
    )


@pytest.fixture
def db() -> dict:
    return {
        "$schema": "protocol-db/v1",
        "brands": {
            "046D": {"name": "Logitech", "name_zh": "罗技", "sdk": ["GHSDK"]},
            "25A7": {"name": "Dareu", "name_zh": "达尔优"},
        },
        "devices": {
            "046D:C54D": {
                "brand": "Logitech", "product": "USB Receiver",
                "source": "hidapi-observed", "verified": False,
            }
        },
    }


# ---------------------------------------------------------------------------
# 逻辑测试（内存 db）
# ---------------------------------------------------------------------------

def test_exact_pid_match_is_known(db):
    result = fingerprint(device(0x046D, 0xC54D), db)
    assert result.status is DeviceStatus.KNOWN
    assert result.brand == "Logitech" and result.brand_zh == "罗技"
    assert result.entry is not None and result.entry["product"] == "USB Receiver"


def test_brand_with_sdk_is_sdk_status(db):
    result = fingerprint(device(0x046D, 0x9999), db)
    assert result.status is DeviceStatus.SDK
    assert result.sdk == ("GHSDK",)
    assert result.entry is None


def test_brand_without_sdk_is_unknown_but_named(db):
    result = fingerprint(device(0x25A7, 0x0001), db)
    assert result.status is DeviceStatus.UNKNOWN
    assert result.brand == "Dareu" and result.brand_zh == "达尔优"
    assert result.sdk == ()


def test_totally_unknown_vid(db):
    result = fingerprint(device(0xDEAD, 0xBEEF), db)
    assert result.status is DeviceStatus.UNKNOWN
    assert result.brand is None
    assert "学习模式" in result.detail


def test_load_db_validates_schema(tmp_path):
    bad = tmp_path / "db.json"
    bad.write_text('{"brands": {}, "devices": {}}', encoding="utf-8")
    with pytest.raises(ProtocolDBError):
        load_db(bad)


def test_load_db_rejects_missing_file(tmp_path):
    with pytest.raises(ProtocolDBError):
        load_db(tmp_path / "nope.json")


def test_load_db_rejects_bad_device_key(tmp_path):
    bad = tmp_path / "db.json"
    bad.write_text(
        '{"$schema": "protocol-db/v1", "brands": {},'
        ' "devices": {"NOTAKEY": {}}}',
        encoding="utf-8",
    )
    with pytest.raises(ProtocolDBError):
        load_db(bad)


# ---------------------------------------------------------------------------
# schema v2 protocol 块校验
# ---------------------------------------------------------------------------

VALID_PROTOCOL = {
    "name": "hidpp",
    "transport": {"match": {"usage_page": "FF00", "usage": "0001"}, "short_len": 7},
    "features": [{
        "id": "ping", "label": "Ping", "kind": "command", "report": "output",
        "template": [16, "{device_index}", 0, "{request_id}", 0, 0, "{mark}"],
        "params": {"device_index": {"type": "int", "default": 255, "min": 0, "max": 255}},
        "response": {"min_len": 7,
                     "check": [16, "{device_index}", 0, "{request_id}", None, None, "{mark}"],
                     "extract": {"major": 4, "minor": 5}},
    }],
}


def _write_db(tmp_path, devices):
    path = tmp_path / "db.json"
    path.write_text(json.dumps({
        "$schema": "protocol-db/v2", "brands": {}, "devices": devices,
    }), encoding="utf-8")
    return path


def test_v2_valid_protocol_loads(tmp_path):
    db = load_db(_write_db(tmp_path, {"046D:C54D": {"brand": "Logitech",
                                                    "protocol": VALID_PROTOCOL}}))
    assert db["devices"]["046D:C54D"]["protocol"]["name"] == "hidpp"


@pytest.mark.parametrize("mutate, fragment", [
    (lambda f: f.update(template=[999]), "template"),
    (lambda f: f.update(template=["{1bad}"]), "template"),
    (lambda f: f.update(kind="write"), "read_from"),
    (lambda f: f.update(report="serial"), "report"),   # input 已合法，换一个非法值
    (lambda f: f["params"].update(x={"type": "string"}), "params"),
])
def test_v2_invalid_features_rejected(tmp_path, mutate, fragment):
    import copy
    protocol = copy.deepcopy(VALID_PROTOCOL)
    mutate(protocol["features"][0])
    path = _write_db(tmp_path, {"046D:C54D": {"protocol": protocol}})
    with pytest.raises(ProtocolDBError, match=fragment):
        load_db(path)


def test_v2_bad_transport_rejected(tmp_path):
    import copy
    protocol = copy.deepcopy(VALID_PROTOCOL)
    protocol["transport"]["match"]["usage_page"] = "XYZ"
    path = _write_db(tmp_path, {"046D:C54D": {"protocol": protocol}})
    with pytest.raises(ProtocolDBError, match="usage_page"):
        load_db(path)


def test_v2_bad_response_check_rejected(tmp_path):
    import copy
    protocol = copy.deepcopy(VALID_PROTOCOL)
    protocol["features"][0]["response"]["check"][4] = "nope"
    path = _write_db(tmp_path, {"046D:C54D": {"protocol": protocol}})
    with pytest.raises(ProtocolDBError, match="check"):
        load_db(path)


def test_real_db_v2_loads():
    db = load_db(DEFAULT_DB_PATH)
    protocol = db["devices"]["046D:C54D"]["protocol"]
    assert protocol["features"][0]["id"] == "ping"


# ---------------------------------------------------------------------------
# save_db / input 监听型 / feature 级 match（M4.3）
# ---------------------------------------------------------------------------

def test_save_db_roundtrip(tmp_path):
    path = tmp_path / "db.json"
    db = {"$schema": "protocol-db/v2", "brands": {"046D": {"name": "Logitech"}},
          "devices": {}}
    save_db(db, path)
    loaded = load_db(path)
    assert loaded["brands"]["046D"]["name"] == "Logitech"
    assert not (tmp_path / "db.json.tmp").exists()   # 原子替换后无残留


def test_save_db_rejects_invalid(tmp_path):
    path = tmp_path / "db.json"
    with pytest.raises(ProtocolDBError):
        save_db({"$schema": "bad", "brands": {}, "devices": {}}, path)
    assert not path.exists()                          # 校验失败不落盘


def test_input_report_accepted(tmp_path):
    path = tmp_path / "db.json"
    db = {"$schema": "protocol-db/v2", "brands": {}, "devices": {
        "291D:385D": {"protocol": {
            "name": "learned",
            "transport": {"match": {"usage_page": "FF21", "usage": "0001"}},
            "features": [{
                "id": "learned_x", "label": "监听", "kind": "read",
                "report": "input", "template": [],
                "response": {"min_len": 4, "check": [None] * 4},
            }],
        }}}}
    save_db(db, path)                                 # 不抛即通过
    assert load_db(path)["devices"]["291D:385D"]["protocol"]["features"][0]["report"] == "input"


def test_feature_level_match_validated(tmp_path):
    import copy
    protocol = copy.deepcopy(VALID_PROTOCOL)
    protocol["features"][0]["match"] = {"usage_page": "XYZ", "usage": "0001"}
    path = _write_db(tmp_path, {"046D:C54D": {"protocol": protocol}})
    with pytest.raises(ProtocolDBError, match="match"):
        load_db(path)


def test_feature_level_match_ok(tmp_path):
    import copy
    protocol = copy.deepcopy(VALID_PROTOCOL)
    protocol["features"][0]["match"] = {"usage_page": "FF21", "usage": "0001"}
    path = _write_db(tmp_path, {"046D:C54D": {"protocol": protocol}})
    db = load_db(path)
    assert db["devices"]["046D:C54D"]["protocol"]["features"][0]["match"]["usage_page"] == "FF21"


# ---------------------------------------------------------------------------
# 真实 db.json 完整性 + 真机观察到的设备命中
# ---------------------------------------------------------------------------

def test_real_db_loads_and_matches():
    db = load_db(DEFAULT_DB_PATH)
    # 罗技 Unifying 接收器（本机实测存在）
    result = fingerprint(device(0x046D, 0xC54D), db)
    assert result.status is DeviceStatus.KNOWN

    # MCHOSE V9 PRO（本机实测存在的游戏鼠标）
    result = fingerprint(device(0x291D, 0x385D), db)
    assert result.status is DeviceStatus.KNOWN
    assert result.entry["note"].startswith("游戏鼠标")


def test_real_db_known_pid_but_brand_sdk_wins_exact():
    """精确条目优先于品牌 SDK 路线。"""
    db = load_db(DEFAULT_DB_PATH)
    exact = fingerprint(device(0x046D, 0xC54D), db)
    assert exact.status is DeviceStatus.KNOWN
    assert exact.entry is not None

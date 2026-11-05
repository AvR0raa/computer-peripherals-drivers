"""任务 1.3 单元测试：core/fingerprint.py + 真实 db.json 完整性。"""
import pytest

from core.device_manager import DeviceInfo
from core.fingerprint import (
    DEFAULT_DB_PATH,
    DeviceStatus,
    FingerprintResult,
    ProtocolDBError,
    fingerprint,
    load_db,
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

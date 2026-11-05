"""M2.5a 单元测试：core/hidpp.py + core/battery.py（假设备脚本化，离线）。"""
import pytest

import core.report_io as rio
from core.battery import BatteryResult, find_battery_fields, read_standard_battery, scan_all
from core.hidpp import (
    FEATURE_UNIFIED_BATTERY,
    HidppBattery,
    HidppLink,
    get_feature_index,
    ping,
    read_battery,
)
from core.report_io import ReportIO
from tests.hidpp_scripted import HidppScript
from tests.test_device_manager import fake_raw
from tests.test_report_io import FakeDev
from tests.test_safe_write import ScriptedDev

DEV = 0xFF


@pytest.fixture
def install(monkeypatch):
    def _install(dev):
        monkeypatch.setattr(rio, "_open_device", lambda path: dev)
        return dev
    return _install


# ---------------------------------------------------------------------------
# hidpp.ping / get_feature_index
# ---------------------------------------------------------------------------

def test_ping_v2_reply(install):
    script = HidppScript()
    install(FakeDev(responses=[script.ping(DEV, 0x04, 0x05)]))
    io = ReportIO(path=b"x")
    state, version = ping(io, DEV)
    assert (state, version) == ("2.0", 4.5)


def test_ping_v2_long_channel(install):
    """双通道：长报文优先并命中（HidppLink，模拟 PRO X2 长报文专用设备）。"""
    script = HidppScript()
    install(FakeDev(responses=[script.ping(DEV, 0x04, 0x05, long=True)]))
    io = HidppLink(None, b"long-only")
    state, version = ping(io, DEV)
    assert (state, version) == ("2.0", 4.5)


def test_ping_hidpp10_error_reply(install):
    script = HidppScript()
    install(FakeDev(responses=[script.err(DEV, 0x01)]))
    io = ReportIO(path=b"x")
    assert ping(io, DEV) == ("1.0", 1.0)


def test_ping_empty_slot(install):
    script = HidppScript()
    install(FakeDev(responses=[script.err(DEV, 0x08)]))
    io = ReportIO(path=b"x")
    assert ping(io, DEV) == ("empty-slot", None)


def test_ping_timeout(install):
    script = HidppScript()
    install(FakeDev(responses=[]))
    io = ReportIO(path=b"x")
    assert ping(io, DEV) == ("offline", None)


def test_get_feature_index(install):
    script = HidppScript()
    dev = install(FakeDev(responses=[script.gf(DEV, 0x07)]))
    io = ReportIO(path=b"x")
    assert get_feature_index(io, DEV, FEATURE_UNIFIED_BATTERY) == 0x07
    wire = dev.written[0]
    assert wire[0] == 0x10 and wire[2] == 0x00 and wire[4:6] == bytes([0x10, 0x04])


def test_get_feature_index_unsupported(install):
    script = HidppScript()
    install(FakeDev(responses=[script.gf(DEV, 0)]))
    io = ReportIO(path=b"x")
    assert get_feature_index(io, DEV, FEATURE_UNIFIED_BATTERY) is None


# ---------------------------------------------------------------------------
# read_battery 三路回退
# ---------------------------------------------------------------------------

def test_read_battery_unified(install):
    # GetFeature(0x1004)→idx07, 调用 funct1 → [discharge=85, level=8, status=1]
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, 0x07),
        script.call(DEV, 0x07, 1, bytes([85, 8, 1])),
    ]))
    io = ReportIO(path=b"x")
    b = read_battery(io, DEV)
    assert b == HidppBattery(percent=85, voltage_mv=None, charging=True,
                             status_raw=1, source="hidpp-unified",
                             detail="统一电量: 85% 充电中")


def test_read_battery_status_fallback(install):
    # 0x1004 不支持(idx00)，0x1000 → idx03, funct0 → [level=60, next=70, status=0]
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, 0),
        script.gf(DEV, 0x03),
        script.call(DEV, 0x03, 0, bytes([60, 70, 0])),
    ]))
    io = ReportIO(path=b"x")
    b = read_battery(io, DEV)
    assert b.percent == 60 and b.source == "hidpp-status"


def test_read_battery_voltage_fallback(install):
    # 三个查询 feature 都不支持，最后 0x1001 → idx05, funct0 → [0x0E, 0x2E, 0x80]
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, 0),                                        # 0x1004 无
        script.gf(DEV, 0),                                        # 0x1000 无
        script.gf(DEV, 0x05),                                     # 0x1001 → 5
        script.call(DEV, 0x05, 0, bytes([0x0E, 0x2E, 0x80])),
    ]))
    io = ReportIO(path=b"x")
    b = read_battery(io, DEV)
    assert b.voltage_mv == 0x0E2E and b.charging is True and b.source == "hidpp-voltage"


def test_read_battery_none_when_no_features(install):
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, 0),
        script.gf(DEV, 0),
        script.gf(DEV, 0),
    ]))
    io = ReportIO(path=b"x")
    assert read_battery(io, DEV) is None


# ---------------------------------------------------------------------------
# battery 标准页 + scan_all
# ---------------------------------------------------------------------------

# 含 0x85 页 feature 字段的描述符（feature report id=3, 8字节, offset 0）
STD_BATTERY_DESC = bytes.fromhex(
    "05 85 09 02 A1 01"
    "85 03"
    "09 02 15 00 25 64 75 08 95 01 B1 02"
    "C0"
)


def test_find_battery_fields():
    fields = find_battery_fields(STD_BATTERY_DESC)
    assert len(fields) == 1
    assert fields[0].usage == 0x850002
    assert fields[0].report_type == "feature"


def test_read_standard_battery_feature(install):
    install(ScriptedDev(feature_reads=[[0x03, 88, 0, 0, 0, 0, 0, 0]]))
    fields = find_battery_fields(STD_BATTERY_DESC)
    io = ReportIO(path=b"x")
    value, detail = read_standard_battery(io, fields)
    assert value == 88
    assert "raw=88" in detail


def _mk(vidpid, usage_page=0x0001, usage=0x0006, product="Dev"):
    vid, pid = int(vidpid[:4], 16), int(vidpid[5:], 16)
    import core.device_manager as dm
    return dm.DeviceInfo.from_hid_dict(
        fake_raw(f"\\\\?\\{vidpid}", vid=vid, pid=pid, product=product,
                 usage_page=usage_page, usage=usage))


def test_scan_all_standard_route(install, monkeypatch):
    install(ScriptedDev(feature_reads=[[0x03, 73, 0, 0, 0, 0, 0, 0]]))
    grouped = {"1A2B:3C4D": [_mk("1A2B:3C4D", usage_page=0x85, usage=0x002)]}
    results = scan_all(grouped, lambda path: STD_BATTERY_DESC)
    assert len(results) == 1
    r = results[0]
    assert r.percent == 73 and r.source == "hid-standard"
    assert r.display == "73%"


def test_scan_all_hidpp_receiver_route(install):
    # 槽位1 应答 1.0(err 0x01) 但无电量 feature → 在线但 source=none
    script = HidppScript()
    install(FakeDev(responses=[script.err(0x01, 0x01)]))
    grouped = {"046D:C54D": [_mk("046D:C54D", usage_page=0xFF00, usage=0x0001, product="USB Receiver")]}
    results = scan_all(grouped, lambda path: b"")
    r = results[0]
    assert r.vidpid == "046D:C54D"
    assert r.source == "none"
    assert "HID++" in r.detail


def test_battery_result_display():
    assert BatteryResult("a", "p", percent=88).display == "88%"
    assert BatteryResult("a", "p", voltage_mv=3800).display == "3800mV"
    assert BatteryResult("a", "p").display == "?"

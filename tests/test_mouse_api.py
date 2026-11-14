"""M2.5c 测试：报告率(0x8060/0x8061) + 统一鼠标设置 API 模板。"""
import pytest

from core.hidpp import (
    HidppLink,
    get_poll_rates,
    read_poll_rate,
    set_poll_rate,
)
from core.mouse_api import (
    BACKENDS,
    DpiOutcome,
    LogitechBackend,
    MouseApi,
    MouseCapabilities,
    detect,
)
from core.report_io import ReportIO
from tests.hidpp_scripted import HidppScript
from tests.test_report_io import FakeDev

DEV = 1


@pytest.fixture
def install(monkeypatch):
    def _install(dev):
        monkeypatch.setattr("core.report_io._open_device", lambda path: dev)
        return dev
    return _install


import pytest


# ---------------------------------------------------------------------------
# 报告率
# ---------------------------------------------------------------------------

def test_get_poll_rates_extended(install):
    """0x8061: 功能位 2-byte bitflags，bit0=125Hz … bit6=8000Hz。"""
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, 0x09),
        script.call(DEV, 0x09, 0x10, bytes([0x00, 0x0F])),   # bit0-3 → 125/250/500/1000
    ]))
    link = ReportIO(path=b"x")
    assert get_poll_rates(link, DEV) == [1000, 500, 250, 125]


def test_get_poll_rates_basic_fallback(install):
    """0x8061 无 → 0x8060: 字节 bitflags bit0=1ms … bit7=8ms。"""
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, 0),
        script.gf(DEV, 0x05),
        script.call(DEV, 0x05, 0x00, bytes([0b00000101, 0x00])),  # 1ms + 3ms
    ]))
    link = ReportIO(path=b"x")
    assert get_poll_rates(link, DEV) == [1000, 333]


def test_read_poll_rate_extended(install):
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, 0x09),
        script.call(DEV, 0x09, 0x20, bytes([0x03])),          # 索引 3 = 1000Hz
    ]))
    link = ReportIO(path=b"x")
    assert read_poll_rate(link, DEV) == 1000


def test_set_poll_rate_extended_verified(install):
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, 0x09),                                  # get_poll_rates
        script.call(DEV, 0x09, 0x10, bytes([0x00, 0x0F])),
        script.gf(DEV, 0x09),                                  # set 的 get_feature_index
        script.call(DEV, 0x09, 0x30, bytes([0x03])),           # 写 funct 0x30
        script.gf(DEV, 0x09),                                  # 读回
        script.call(DEV, 0x09, 0x20, bytes([0x03])),
    ]))
    link = ReportIO(path=b"x")
    ok, msg = set_poll_rate(link, DEV, 1000)
    assert ok and "验证通过" in msg


def test_set_poll_rate_unsupported(install):
    script = HidppScript()
    install(FakeDev(responses=[script.gf(DEV, 0), script.gf(DEV, 0)]))
    link = ReportIO(path=b"x")
    ok, msg = set_poll_rate(link, DEV, 1000)
    assert not ok and "不支持" in msg


def test_set_poll_rate_not_in_list(install):
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, 0x09),
        script.call(DEV, 0x09, 0x10, bytes([0x00, 0x0F])),
    ]))
    link = ReportIO(path=b"x")
    ok, msg = set_poll_rate(link, DEV, 8000)
    assert not ok and "不在支持列表" in msg


# ---------------------------------------------------------------------------
# MouseApi 模板（假后端验证注册/路由机制）
# ---------------------------------------------------------------------------

class FakeBackend:
    """模板测试替身：模拟一个非罗技品牌后端。"""

    def __init__(self):
        self._dpi = 800

    def matches(self, vid_hex, ifaces):
        return vid_hex == "ABCD"

    def build(self, ifaces):
        pass

    def capabilities(self):
        return MouseCapabilities(dpi_list=[400, 800, 1600], dpi_support=True,
                                 poll_rates=[1000, 500], poll_support=True,
                                 battery_support=True)

    def get_dpi(self):
        return (self._dpi, None, None, None)

    def set_dpi(self, value):
        old = self._dpi
        self._dpi = value
        return DpiOutcome(ok=value == 1600 or True, wrote=value, readback=value,
                          log=[f"读旧值 {old}", "写", f"读回 {value}", "验证: 通过"])

    def get_poll_rate(self):
        return 1000

    def set_poll_rate(self, hz):
        return True, f"已设 {hz}Hz"

    def get_battery(self):
        from core.battery import BatteryResult
        return BatteryResult(vidpid="ABCD:1234", percent=70)

    @property
    def brand(self):
        return "Fake"


def test_backend_registry_and_detect(monkeypatch):
    monkeypatch.setattr("core.mouse_api.BACKENDS", [FakeBackend])
    ifaces = [type("I", (), {"vid_hex": "ABCD", "pid_hex": "1234"})()]
    api = detect(ifaces)
    assert api is not None and api.brand == "Fake"
    api_none = detect([type("I", (), {"vid_hex": "FFFF", "pid_hex": "1"})()])
    assert api_none is None


def test_mouse_api_flow(monkeypatch):
    monkeypatch.setattr("core.mouse_api.BACKENDS", [FakeBackend])
    ifaces = [type("I", (), {"vid_hex": "ABCD", "pid_hex": "1234"})()]
    api = detect(ifaces)
    caps = api.capabilities()
    assert caps.dpi_list == [400, 800, 1600]
    assert caps.poll_rates == [1000, 500]
    dpi, _y, _l, _d = api.get_dpi()
    assert dpi == 800
    out = api.set_dpi(1600)
    assert out.ok and out.wrote == 1600
    assert api.get_poll_rate() == 1000
    ok, msg = api.set_poll_rate(500)
    assert ok
    assert api.get_battery().percent == 70


def test_mouse_api_rejects_bad_dpi(monkeypatch):
    monkeypatch.setattr("core.mouse_api.BACKENDS", [FakeBackend])
    ifaces = [type("I", (), {"vid_hex": "ABCD", "pid_hex": "1234"})()]
    api = detect(ifaces)
    out = api.set_dpi(99999)
    assert not out.ok and "超出安全范围" in out.log[0]


def test_logitech_backend_registered():
    assert LogitechBackend in BACKENDS


def test_summary_dict(monkeypatch):
    monkeypatch.setattr("core.mouse_api.BACKENDS", [FakeBackend])
    ifaces = [type("I", (), {"vid_hex": "ABCD", "pid_hex": "1234"})()]
    api = detect(ifaces)
    s = api.summary()
    assert s["brand"] == "Fake" and s["dpi_list"] == [400, 800, 1600]


# ---------------------------------------------------------------------------
# 标准电量缩放
# ---------------------------------------------------------------------------

def test_battery_percent_scaling():
    from core.battery import _scale_percent
    from core.descriptor import ReportField
    f = ReportField(report_type="feature", report_id=3, offset=0, size=8, count=1,
                    usage=0x850002, logical_min=0, logical_max=10)
    assert _scale_percent(f, 5) == 50        # 0-10 → 百分比
    f100 = ReportField(report_type="feature", report_id=3, offset=0, size=8, count=1,
                       usage=0x850002, logical_min=0, logical_max=100)
    assert _scale_percent(f100, 88) == 88    # 已是百分比


def test_find_battery_fields_matches_ez24g_pairs():
    # 键盘页 (0x0006, 0x0020) 用法对（EZ2.4G 实测）
    desc = bytes.fromhex(
        "05 06 09 20 A1 01 85 02 09 20 15 00 25 64 75 08 95 01 81 02 C0")
    from core.battery import find_battery_fields
    fields = find_battery_fields(desc)
    assert len(fields) == 1 and fields[0].logical_max == 100

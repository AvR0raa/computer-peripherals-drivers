"""M2.5b 单元测试：core/dpi.py（脚本化 HID++ 应答，sw_id 对齐，离线）。"""
import pytest

from core.dpi import DpiOutcome, get_dpi_list, read_dpi, set_dpi
from core.report_io import ReportIO
from tests.hidpp_scripted import HidppScript
from tests.test_report_io import FakeDev

DEV = 1
IDX = 0x08          # 假设 0x2201 被分配到 index 8


@pytest.fixture
def install(monkeypatch):
    def _install(dev):
        monkeypatch.setattr("core.report_io._open_device", lambda path: dev)
        return dev
    return _install


def dpi_list_payload(values, page=0):
    """GetSensorDpiList 应答 payload: [X] + BE16 序列 + 0x0000 结尾。"""
    body = bytes([page])
    for v in values:
        body += bytes([(v >> 8) & 0xFF, v & 0xFF])
    body += b"\x00\x00"
    return body[:16].ljust(16, b"\x00")


def test_get_dpi_list_plain_values(install):
    script = HidppScript()
    dev = install(FakeDev(responses=[
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 1, dpi_list_payload([100, 200, 400]), long=True),
    ]))
    link = ReportIO(path=b"x")
    assert get_dpi_list(link, DEV) == [100, 200, 400]
    assert dev.written[1][0] == 0x11                             # 长报文请求
    assert (dev.written[1][3] >> 4) == 1                         # funct 1


def test_get_dpi_list_step_encoding(install):
    # 档位表: [400] + 步进编码(step=50, last=800) → 展开 400,450,...,800
    step_val = 0xE000 | 50
    payload = bytes([0]) + bytes([0x01, 0x90]) + \
        bytes([(step_val >> 8) & 0xFF, step_val & 0xFF]) + bytes([0x03, 0x20]) + b"\x00\x00"
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 1, payload.ljust(16, b"\x00"), long=True),
    ]))
    link = ReportIO(path=b"x")
    result = get_dpi_list(link, DEV)
    assert result[0] == 400 and result[-1] == 800
    assert result == list(range(400, 801, 50))


def test_get_dpi_list_unsupported(install):
    script = HidppScript()
    install(FakeDev(responses=[script.gf(DEV, 0)]))
    link = ReportIO(path=b"x")
    assert get_dpi_list(link, DEV) == []


def test_read_dpi_short_reply(install):
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, bytes([0x00, 0x03, 0x20])),      # [X, 03, 20] = 800
    ]))
    link = ReportIO(path=b"x")
    dpi, dpi_y, lod, default = read_dpi(link, DEV)
    assert dpi == 800 and dpi_y is None and lod is None and default is None


def test_read_dpi_long_reply_with_default(install):
    # 短请求、长应答: payload [X, hi, lo, defHi, defLo, ...]
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, bytes([0x00, 0x03, 0x20, 0x01, 0x2C]), long=True),
    ]))
    link = ReportIO(path=b"x")
    dpi, _y, _l, default = read_dpi(link, DEV)
    assert dpi == 800 and default == 300


def test_read_dpi_zero_falls_back_to_default(install):
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, bytes([0x00, 0x00, 0x00, 0x01, 0x2C]), long=True),
    ]))
    link = ReportIO(path=b"x")
    dpi, _y, _l, _d = read_dpi(link, DEV)
    assert dpi == 300                                            # 0 → 默认值


# ---------------------------------------------------------------------------
# 0x2202 Extended Adjustable DPI（PRO X2 等新款，Solaar #2207）
# ---------------------------------------------------------------------------

def test_read_dpi_extended_feature(install):
    # 0x2201 不支持 → 0x2202 分配 idx 9，能力 byte2 bit0=Y bit1=LOD
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, 0),                                        # 0x2201 无
        script.gf(DEV, 0x09),                                     # 0x2202 → 9
        script.call(DEV, 0x09, 1, bytes([0x00, 0x00, 0x03])),     # GetInfo: Y=1 LOD=1
        script.call(DEV, 0x09, 5, bytes([0x00, 0x0B, 0xB8, 0x00, 0x00,
                                        0x0B, 0xB8, 0x00, 0x00, 0x02]), long=True),
    ]))
    link = ReportIO(path=b"x")
    dpi, dpi_y, lod, _d = read_dpi(link, DEV)
    assert dpi == 3000 and dpi_y == 3000 and lod == 2


def test_get_dpi_list_extended(install):
    step_val = 0xE000 | 50
    payload = bytes([0, 0, 0]) + bytes([0x01, 0x90]) + \
        bytes([(step_val >> 8) & 0xFF, step_val & 0xFF]) + bytes([0x03, 0x20]) + b"\x00\x00"
    script = HidppScript()
    install(FakeDev(responses=[
        script.gf(DEV, 0),                                        # 0x2201 无
        script.gf(DEV, 0x09),                                     # 0x2202 → 9
        script.call(DEV, 0x09, 1, bytes([0x00, 0x00, 0x00])),     # GetInfo
        script.call(DEV, 0x09, 2, payload.ljust(16, b"\x00"), long=True),
    ]))
    link = ReportIO(path=b"x")
    result = get_dpi_list(link, DEV)
    assert result == list(range(400, 801, 50))


# ---------------------------------------------------------------------------
# set_dpi 安全写
# ---------------------------------------------------------------------------

def test_set_dpi_success(install):
    # set_dpi: detect(GF) → read(detect GF + read) → set → read(detect GF + read)
    script = HidppScript()
    dev = install(FakeDev(responses=[
        script.gf(DEV, IDX),
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, bytes([0x00, 0x03, 0x20])),      # 读旧 800
        script.call(DEV, IDX, 3, bytes([0x00, 0x06, 0x40])),      # SetSensorDpi ACK
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, bytes([0x00, 0x06, 0x40])),      # 读回 1600
    ]))
    link = ReportIO(path=b"x")
    out = set_dpi(link, DEV, 1600)
    assert out.ok and out.wrote == 1600 and out.old == 800
    assert out.readback == 1600 and not out.rolled_back
    assert out.feature == "0x2201"
    set_wire = dev.written[3]
    assert set_wire[0] == 0x10 and set_wire[2] == IDX
    assert (set_wire[3] >> 4) == 3                               # funct 3
    assert set_wire[4:7] == bytes([0x00, 0x06, 0x40])            # [0, 1600hi, 1600lo]


def test_set_dpi_verify_fail_rollback(install):
    script = HidppScript()
    dev = install(FakeDev(responses=[
        script.gf(DEV, IDX),
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, bytes([0x00, 0x03, 0x20])),      # 读旧 800
        script.call(DEV, IDX, 3, bytes([0x00, 0x06, 0x40])),      # 写 1600
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, bytes([0x00, 0x03, 0x20])),      # 读回=旧值 → 失败
        script.call(DEV, IDX, 3, bytes([0x00, 0x03, 0x20])),      # 回滚写
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, bytes([0x00, 0x03, 0x20])),      # 读回 800
    ]))
    link = ReportIO(path=b"x")
    out = set_dpi(link, DEV, 1600)
    assert not out.ok and out.rolled_back is True
    assert any("回滚" in line for line in out.log)


def test_set_dpi_rejects_out_of_range(install):
    install(FakeDev())
    link = ReportIO(path=b"x")
    out = set_dpi(link, DEV, 99999)
    assert not out.ok and out.wrote is None
    assert "超出安全范围" in out.log[0]


def test_set_dpi_no_feature(install):
    script = HidppScript()
    install(FakeDev(responses=[script.gf(DEV, 0), script.gf(DEV, 0)]))
    link = ReportIO(path=b"x")
    out = set_dpi(link, DEV, 1600)
    assert not out.ok
    assert "0x2201/0x2202" in out.log[0]


def test_set_dpi_extended_writes_full_xyz_lod(install):
    """回归（真机实证 2026-11-14）：0x2202 Y+LOD 设备必须写三段式 [idx, X, Y, LOD]，
    缺 LOD 会被 PRO X2 静默忽略（读回不变 → 曾被误判为板载配置档拦截）。"""
    script = HidppScript()
    dev = install(FakeDev(responses=[
        # --- set_dpi._detect: 0x2201 miss → 0x2202 hit → GetInfo ---
        script.gf(DEV, 0),
        script.gf(DEV, 0x09),
        script.call(DEV, 0x09, 1, bytes([0x00, 0x00, 0x03])),   # Y=1 LOD=1
        # --- read_dpi._detect + funct5 读旧 X=800 Y=800 LOD=2 ---
        script.gf(DEV, 0),
        script.gf(DEV, 0x09),
        script.call(DEV, 0x09, 1, bytes([0x00, 0x00, 0x03])),
        script.call(DEV, 0x09, 5,
                    bytes([0x00, 0x03, 0x20, 0x00, 0x03, 0x20, 0x00, 0x00, 0x00, 0x02]),
                    long=True),
        # --- funct6 写 X=Y=1600 LOD=2 ---
        script.call(DEV, 0x09, 6,
                    bytes([0x00, 0x06, 0x40, 0x06, 0x40, 0x02, 0x00, 0x00]),
                    long=True),
        # --- read_dpi._detect + funct5 读回 1600 ---
        script.gf(DEV, 0),
        script.gf(DEV, 0x09),
        script.call(DEV, 0x09, 1, bytes([0x00, 0x00, 0x03])),
        script.call(DEV, 0x09, 5,
                    bytes([0x00, 0x06, 0x40, 0x00, 0x06, 0x40, 0x00, 0x00, 0x00, 0x02]),
                    long=True),
    ]))
    link = ReportIO(path=b"x")
    out = set_dpi(link, DEV, 1600)
    assert out.ok and out.wrote == 1600 and out.feature == "0x2202"
    # 找到 funct6 写入线材：必须 6 字节三段式（X=Y=1600，LOD=2 原值保持）
    set_wires = [w for w in dev.written if len(w) >= 10 and (w[3] >> 4) == 6]
    assert len(set_wires) == 1
    assert set_wires[0][4:10] == bytes([0x00, 0x06, 0x40, 0x06, 0x40, 0x02])

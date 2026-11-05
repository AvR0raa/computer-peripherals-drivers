"""M2.5b 单元测试：core/dpi.py（假设备脚本化 HID++ 应答，离线）。

脚本约定: feature 查询序列按 GetFeature→业务调用的顺序回放。
"""
import pytest

from core.dpi import DpiOutcome, get_dpi_list, read_dpi, set_dpi
from core.report_io import ReportIO
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
    """构造 GetSensorDpiList 应答 payload: [X] + BE16 序列 + 0x0000 结尾。"""
    body = bytes([page])
    for v in values:
        body += bytes([(v >> 8) & 0xFF, v & 0xFF])
    body += b"\x00\x00"
    return body[:16].ljust(16, b"\x00")


def test_get_dpi_list_plain_values(install):
    dev = install(FakeDev(responses=[
        [0x10, DEV, 0x00, 0x05, IDX, 0, 0],                    # GetFeature(0x2201) → 8
        [0x11, DEV, IDX, 0x15] + list(dpi_list_payload([100, 200, 400])),
    ]))
    link = ReportIO(path=b"x")
    assert get_dpi_list(link, DEV) == [100, 200, 400]
    assert dev.written[1][:4] == bytes([0x11, DEV, IDX, 0x15])   # 长报文请求


def test_get_dpi_list_step_encoding(install):
    # 档位表: [400] + 步进编码(step=50, last=800) → 展开 400,450,...,800
    step_val = 0xE000 | 50
    payload = bytes([0]) + bytes([0x01, 0x90]) + \
        bytes([(step_val >> 8) & 0xFF, step_val & 0xFF]) + bytes([0x03, 0x20]) + b"\x00\x00"
    install(FakeDev(responses=[
        [0x10, DEV, 0x00, 0x05, IDX, 0, 0],
        [0x11, DEV, IDX, 0x15] + list(payload.ljust(16, b"\x00")),
    ]))
    link = ReportIO(path=b"x")
    result = get_dpi_list(link, DEV)
    assert result[0] == 400 and result[-1] == 800
    assert result == list(range(400, 801, 50))


def test_get_dpi_list_unsupported(install):
    install(FakeDev(responses=[[0x10, DEV, 0x00, 0x05, 0x00, 0, 0]]))
    link = ReportIO(path=b"x")
    assert get_dpi_list(link, DEV) == []


def test_read_dpi_short_reply(install):
    install(FakeDev(responses=[
        [0x10, DEV, 0x00, 0x05, IDX, 0, 0],
        [0x10, DEV, IDX, 0x25, 0x00, 0x03, 0x20],              # payload [X, 03, 20] = 800
    ]))
    link = ReportIO(path=b"x")
    dpi, default = read_dpi(link, DEV)
    assert dpi == 800 and default is None


def test_read_dpi_long_reply_with_default(install):
    # 短请求、长应答: payload [X, hi, lo, defHi, defLo, ...]
    install(FakeDev(responses=[
        [0x10, DEV, 0x00, 0x05, IDX, 0, 0],
        [0x11, DEV, IDX, 0x25, 0x00, 0x03, 0x20, 0x01, 0x2C] + [0] * 11,
    ]))
    link = ReportIO(path=b"x")
    dpi, default = read_dpi(link, DEV)
    assert dpi == 800 and default == 300


def test_read_dpi_zero_falls_back_to_default(install):
    install(FakeDev(responses=[
        [0x10, DEV, 0x00, 0x05, IDX, 0, 0],
        [0x11, DEV, IDX, 0x25, 0x00, 0x00, 0x00, 0x01, 0x2C] + [0] * 11,
    ]))
    link = ReportIO(path=b"x")
    dpi, _ = read_dpi(link, DEV)
    assert dpi == 300                                          # 0 → 默认值


def test_set_dpi_success(install):
    # set_dpi 内部多次 get_feature_index: GF, (读旧: GF+read), SET, (读回: GF+read)
    dev = install(FakeDev(responses=[
        [0x10, DEV, 0x00, 0x05, IDX, 0, 0],                    # get_feature_index
        [0x10, DEV, 0x00, 0x05, IDX, 0, 0],                    # read_dpi 内 GF
        [0x10, DEV, IDX, 0x25, 0x00, 0x03, 0x20],              # 读旧 800
        [0x10, DEV, IDX, 0x35, 0x00, 0x06, 0x40],              # SetSensorDpi ACK
        [0x10, DEV, 0x00, 0x05, IDX, 0, 0],                    # 读回内 GF
        [0x10, DEV, IDX, 0x25, 0x00, 0x06, 0x40],              # 读回 1600
    ]))
    link = ReportIO(path=b"x")
    out = set_dpi(link, DEV, 1600)
    assert out.ok and out.wrote == 1600 and out.old == 800
    assert out.readback == 1600 and not out.rolled_back
    # 写入线材: [0x10, dev, idx, 0x35, 0x00, 0x06, 0x40]
    assert dev.written[3] == bytes([0x10, DEV, IDX, 0x35, 0x00, 0x06, 0x40])


def test_set_dpi_verify_fail_rollback(install):
    # 读旧 800 → 写 1600 → 读回仍 800 → 回滚 → 读回 800
    install(FakeDev(responses=[
        [0x10, DEV, 0x00, 0x05, IDX, 0, 0],
        [0x10, DEV, 0x00, 0x05, IDX, 0, 0],
        [0x10, DEV, IDX, 0x25, 0x00, 0x03, 0x20],              # 读旧 800
        [0x10, DEV, IDX, 0x35, 0x00, 0x06, 0x40],              # 写 1600
        [0x10, DEV, 0x00, 0x05, IDX, 0, 0],
        [0x10, DEV, IDX, 0x25, 0x00, 0x03, 0x20],              # 读回=旧值 → 验证失败
        [0x10, DEV, 0x00, 0x05, IDX, 0, 0],                    # 回滚读回内 GF
        [0x10, DEV, IDX, 0x35, 0x00, 0x03, 0x20],              # 回滚写
        [0x10, DEV, 0x00, 0x05, IDX, 0, 0],                    # 回滚读回内 GF
        [0x10, DEV, IDX, 0x25, 0x00, 0x03, 0x20],              # 读回 800
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

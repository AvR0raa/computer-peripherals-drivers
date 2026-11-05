"""任务 2.2 单元测试：core/report_io.py（假设备注入，离线运行）。"""
import pytest

import core.report_io as rio
from core.report_io import (
    HistoryEntry,
    RenderError,
    ReportError,
    ReportIO,
    extract_fields,
    match_response,
    render_template,
)


# ---------------------------------------------------------------------------
# render_template（对应验收：模板渲染正确）
# ---------------------------------------------------------------------------

def test_render_int_only():
    assert render_template([16, 255, 0], {}) == b"\x10\xff\x00"


def test_render_with_params():
    template = [16, "{device_index}", 0, "{request_id}", 0, 0, "{mark}"]
    params = {"device_index": 255, "request_id": 16, "mark": 66}
    assert render_template(template, params) == b"\x10\xff\x00\x10\x00\x00\x42"


def test_render_missing_param_raises():
    with pytest.raises(RenderError, match="未提供"):
        render_template(["{nope}"], {})


def test_render_out_of_range_raises():
    with pytest.raises(RenderError):
        render_template([256], {})
    with pytest.raises(RenderError):
        render_template(["{x}"], {"x": -1})
    with pytest.raises(RenderError):
        render_template([1.5], {})


def test_render_bad_placeholder_raises():
    with pytest.raises(RenderError):
        render_template(["{1bad}"], {})


# ---------------------------------------------------------------------------
# match_response / extract_fields
# ---------------------------------------------------------------------------

def test_match_exact_and_wildcard():
    check = [16, 255, None, 16]
    assert match_response(b"\x10\xff\xab\x10", check, {})
    assert not match_response(b"\x11\xff\xab\x10", check, {})
    assert not match_response(b"\x10\xff\xab", check)          # 长度不足


def test_match_param_ref_and_any_of():
    params = {"device_index": 255, "mark": 66}
    check = [16, "{device_index}", [0, 143], None, None, None, "{mark}"]
    assert match_response(b"\x10\xff\x00\x10\x01\x02\x42", check, params)
    assert match_response(b"\x10\xff\x8f\x10\x01\x02\x42", check, params)
    assert not match_response(b"\x10\xff\x00\x10\x01\x02\x99", check, params)


def test_extract_fields():
    out = extract_fields(b"\x10\xff\x00\x10\x02\x50\x42", {"major": 4, "minor": 5})
    assert out == {"major": 2, "minor": 0x50}
    assert extract_fields(b"\x00", {"x": 9}) == {"x": None}    # 越界给 None


# ---------------------------------------------------------------------------
# ReportIO（假设备）
# ---------------------------------------------------------------------------

class FakeDev:
    """按脚本回放的假 hidapi 设备。"""

    def __init__(self, responses=None):
        self.responses = list(responses or [])
        self.written = []
        self.features_sent = []
        self.closed = False

    def open_path(self, path):
        assert isinstance(path, bytes)

    def close(self):
        self.closed = True

    def write(self, data):
        self.written.append(bytes(data))
        return len(data)

    def read(self, length, timeout_ms):
        return self.responses.pop(0) if self.responses else []

    def send_feature_report(self, data):
        self.features_sent.append(bytes(data))
        return len(data)

    def get_feature_report(self, report_id, size):
        return [report_id] + [0xAA] * (size - 1)


@pytest.fixture
def install(monkeypatch):
    def _install(responses=None):
        dev = FakeDev(responses)
        monkeypatch.setattr(rio, "_open_device", lambda path: dev)
        return dev
    return _install


def test_call_returns_matching_response(install):
    noise = [0xA1, 0x01, 0x20, 0x30, 0x00, 0x00, 0x00]          # 鼠标移动推送，应被忽略
    reply = [0x10, 0xFF, 0x00, 0x10, 0x02, 0x50, 0x42]          # ping 响应
    install(responses=[noise, reply])
    io = ReportIO(path=b"fake")
    params = {"device_index": 255, "request_id": 16, "mark": 66}
    check = [16, "{device_index}", 0, "{request_id}", None, None, "{mark}"]
    with io:
        result = io.call(b"\x10\xff\x00\x10\x00\x00\x42", check, params, timeout_ms=300)
    assert result == bytes(reply)
    assert io.history[0].direction == "tx"
    assert io.history[-1].direction == "rx"


def test_call_timeout_returns_none(install):
    install(responses=[])
    io = ReportIO(path=b"fake")
    with io:
        result = io.call(b"\x10\xff\x00\x10\x00\x00\x42", [16], {}, timeout_ms=80)
    assert result is None


def test_call_none_check_returns_first(install):
    install(responses=[[1, 2, 3]])
    io = ReportIO(path=b"fake")
    with io:
        assert io.call(b"\x00", None, {}, timeout_ms=200) == bytes([1, 2, 3])


def test_history_subscription(install):
    install(responses=[[9, 9]])
    seen = []
    io = ReportIO(path=b"fake", on_history=seen.append)
    with io:
        io.call(b"\x00", None, {}, timeout_ms=200)
    assert [e.direction for e in seen] == ["tx", "rx"]
    assert seen[0].data == b"\x00"
    assert isinstance(seen[0], HistoryEntry)


def test_feature_roundtrip(install):
    dev = install()
    io = ReportIO(path=b"fake")
    with io:
        io.send_feature(b"\x07\x01\x02")
        out = io.get_feature(report_id=7, size=4)
    assert dev.features_sent == [b"\x07\x01\x02"]
    assert out == bytes([7, 0xAA, 0xAA, 0xAA])


def test_open_failure_raises(monkeypatch):
    def boom(path):
        raise OSError("no such device")
    monkeypatch.setattr(rio, "_open_device", boom)
    io = ReportIO(path=b"ghost")
    with pytest.raises(ReportError, match="打开设备失败"):
        io.open()


def test_write_error_raises(monkeypatch):
    dev = FakeDev()
    dev.write = lambda data: -1
    monkeypatch.setattr(rio, "_open_device", lambda path: dev)
    io = ReportIO(path=b"fake")
    with io, pytest.raises(ReportError):
        io.write_output(b"\x00")


def test_close_is_idempotent(install):
    install()
    io = ReportIO(path=b"fake")
    io.open()
    io.close()
    io.close()
    assert io._dev is None

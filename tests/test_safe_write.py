"""任务 2.4 单元测试：core/safe_write.py（假设备脚本化收发，离线）。"""
import pytest

import core.report_io as rio
from core.report_io import ReportIO
from core.safe_write import safe_write
from tests.test_report_io import FakeDev


class ScriptedDev(FakeDev):
    """feature report 读走脚本队列（test_report_io.FakeDev 是固定填充）。"""

    def __init__(self, responses=None, feature_reads=None):
        super().__init__(responses)
        self.feature_reads = list(feature_reads or [])   # 每项 = get_feature_report 完整返回
        self.features_sent = []

    def get_feature_report(self, report_id, size):
        if self.feature_reads:
            return list(self.feature_reads.pop(0))
        return [report_id] + [0x00] * (size - 1)

    def send_feature_report(self, data):
        self.features_sent.append(bytes(data))
        return len(data)


@pytest.fixture
def install(monkeypatch):
    def _install(responses=None, feature_reads=None):
        dev = ScriptedDev(responses, feature_reads)
        monkeypatch.setattr(rio, "_open_device", lambda path: dev)
        return dev
    return _install


# ---- schema 片段 ----

FEATURE_WRITE = {
    "id": "set_dpi", "kind": "write", "report": "feature",
    "template": [0x07, "{reg}", "{value}", 0x00, 0x00],
    "params": {
        "reg": {"type": "int", "default": 0x10, "min": 0, "max": 255},
        "value": {"type": "int", "default": 0, "min": 0, "max": 255},
    },
}
READ_OLD = [0x07, 0x10, 0x20, 0x00, 0x00]
READ_NEW_OK = [0x07, 0x10, 0x40, 0x00, 0x00]

OUTPUT_WRITE = {
    "id": "set_dpi", "kind": "write", "report": "output",
    "template": [16, "{device_index}", 0, 33, "{value}", 0, 66],
    "params": {
        "device_index": {"type": "int", "default": 255, "min": 0, "max": 255},
        "value": {"type": "int", "default": 0, "min": 0, "max": 255},
    },
}
OUTPUT_READ = {
    "id": "read_dpi", "kind": "read", "report": "output",
    "template": [16, "{device_index}", 0, 32, 0, 0, 66],
    "params": {"device_index": {"type": "int", "default": 255, "min": 0, "max": 255}},
    "response": {
        "check": [16, "{device_index}", 0, 32, None, None, 66],
        "extract": {"value": 4},
    },
}
OLD_RSP = bytes([16, 255, 0, 32, 0x20, 0, 66])
NEW_RSP = bytes([16, 255, 0, 32, 0x40, 0, 66])


# ---- 拒绝条件 ----

def test_reject_non_write_feature(install):
    dev = install()
    io = ReportIO(path=b"fake")
    result = safe_write(io, {"id": "x", "kind": "command", "report": "feature",
                             "template": [1]}, new_params={})
    assert not result.ok and result.wrote is None
    assert "不是 write" in result.log[0]
    assert dev.features_sent == []


def test_reject_output_without_read_feature(install):
    dev = install()
    io = ReportIO(path=b"fake")
    result = safe_write(io, OUTPUT_WRITE, read_feature=None, new_params={"value": 1})
    assert not result.ok
    assert "必须配对 read" in result.log[0]


# ---- feature report 路径 ----

def test_feature_write_verified(install):
    dev = install(feature_reads=[READ_OLD, READ_NEW_OK])
    io = ReportIO(path=b"fake")
    result = safe_write(io, FEATURE_WRITE, new_params={"value": 0x40},
                        expect={2: 0x40})
    assert result.ok and result.verified and not result.rolled_back
    assert result.old_value == bytes(READ_OLD)
    assert result.readback == bytes(READ_NEW_OK)
    assert dev.features_sent == [b"\x07\x10\x40\x00\x00"]       # 只写一次
    assert any("读旧值" in line for line in result.log)
    assert any("验证: 通过" in line for line in result.log)


def test_feature_write_fail_then_rollback(install):
    # 读回仍是旧值 → 验证失败 → 回滚旧值 → 读回确认
    dev = install(feature_reads=[READ_OLD, READ_OLD, READ_OLD])
    io = ReportIO(path=b"fake")
    result = safe_write(io, FEATURE_WRITE, new_params={"value": 0x40},
                        expect={2: 0x40})
    assert result.ok is False and result.verified is False
    assert result.rolled_back is True
    assert dev.features_sent == [b"\x07\x10\x40\x00\x00", bytes(READ_OLD)]
    assert any("回滚成功" in line for line in result.log)


def test_feature_write_without_expect_is_unverified_but_ok(install):
    dev = install(feature_reads=[READ_OLD, READ_NEW_OK])
    io = ReportIO(path=b"fake")
    result = safe_write(io, FEATURE_WRITE, new_params={"value": 0x40})
    assert result.ok is True and result.verified is False       # 已写入未验证
    assert any("无法自动验证" in line for line in result.log)
    assert dev.features_sent == [b"\x07\x10\x40\x00\x00"]       # 无 expect 不触发回滚


def test_feature_write_template_error_aborts(install):
    dev = install()
    io = ReportIO(path=b"fake")
    result = safe_write(io, FEATURE_WRITE, new_params={"value": 999}, expect={2: 1})
    assert not result.ok and result.wrote is None
    assert dev.features_sent == []


# ---- output + read 功能配对路径 ----

def test_output_write_verified(install):
    dev = install(responses=[OLD_RSP, NEW_RSP])
    io = ReportIO(path=b"fake")
    result = safe_write(io, OUTPUT_WRITE, read_feature=OUTPUT_READ,
                        new_params={"value": 0x40}, expect={4: 0x40})
    assert result.ok and result.verified and not result.rolled_back
    assert result.old_value == OLD_RSP and result.readback == NEW_RSP
    # 写入请求出现在两次读请求之间
    writes = [w for w in dev.written if w[3] == 33]
    assert writes == [bytes([16, 255, 0, 33, 0x40, 0, 66])]


def test_output_write_fail_rollback_with_params(install):
    dev = install(responses=[OLD_RSP, OLD_RSP, OLD_RSP])
    io = ReportIO(path=b"fake")
    result = safe_write(io, OUTPUT_WRITE, read_feature=OUTPUT_READ,
                        new_params={"value": 0x40}, expect={4: 0x40},
                        rollback_params={"value": 0x20})
    assert result.verified is False
    assert result.rolled_back is True
    rollback_writes = [w for w in dev.written if w[3] == 33 and w[4] == 0x20]
    assert len(rollback_writes) == 1
    assert any("回滚成功" in line for line in result.log)


def test_output_write_fail_no_rollback_params(install):
    dev = install(responses=[OLD_RSP, OLD_RSP])
    io = ReportIO(path=b"fake")
    result = safe_write(io, OUTPUT_WRITE, read_feature=OUTPUT_READ,
                        new_params={"value": 0x40}, expect={4: 0x40})
    assert result.verified is False and result.rolled_back is False
    assert any("未提供 rollback_params" in line for line in result.log)


def test_output_write_no_old_value_cannot_rollback(install):
    # 第一次读就无响应 → 无旧值 → 不回滚
    install(responses=[[], [], []])
    io = ReportIO(path=b"fake")
    result = safe_write(io, OUTPUT_WRITE, read_feature=OUTPUT_READ,
                        new_params={"value": 0x40}, expect={4: 0x40})
    assert result.old_value is None
    assert result.rolled_back is False
    assert any("无旧值可回滚" in line for line in result.log)

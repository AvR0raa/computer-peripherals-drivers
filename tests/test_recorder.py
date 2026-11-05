"""任务 4.1 单元测试：core/recorder.py（假设备 + 真线程，离线运行）。"""
import time

import pytest

import core.report_io as rio
from core.recorder import CaptureEntry, Recorder, RecorderTarget
from tests.test_report_io import FakeDev


def wait_until(pred, timeout: float = 3.0) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        if pred():
            return True
        time.sleep(0.02)
    return False


class DyingDev(FakeDev):
    """read 抛异常的假设备（模拟设备拔出/接口死亡）。"""

    def read(self, length, timeout_ms):
        raise OSError("device gone")


@pytest.fixture
def install(monkeypatch):
    def _install(dev):
        if isinstance(dev, dict):
            # ReportIO.open() 传入的是 encode 后的 bytes path
            monkeypatch.setattr(rio, "_open_device",
                                lambda path: dev[path.decode("utf-8")])
        else:
            monkeypatch.setattr(rio, "_open_device", lambda path: dev)
        return dev
    return _install


def test_recorder_captures_input_entries(install):
    install(FakeDev(responses=[[0xED, 1, 2], [0xED, 3, 4]]))
    rec = Recorder([RecorderTarget(label="FF21", path="\\\\?\\x")])
    rec.start()
    ok = wait_until(lambda: len(rec.entries()) >= 2)
    rec.stop()
    assert ok, f"等待输入记录超时, 实得 {rec.entries()}"
    entries = [e for e in rec.entries() if e.kind == "input"]
    assert entries[0].data == "ed 01 02"
    assert entries[1].data == "ed 03 04"
    assert entries[0].report_id == 0xED
    assert entries[0].ts > 0 and entries[1].ts >= entries[0].ts


def test_feature_snapshot_at_start_and_manual(install):
    install(FakeDev(responses=[[0xED] * 3] * 10))
    rec = Recorder([RecorderTarget(label="FF21", path="\\\\?\\x", feature_ids=(237,))])
    rec.start()
    wait_until(lambda: any(e.kind == "feature_read" for e in rec.entries()))
    before = rec.snapshot_features()
    rec.stop()
    entries = rec.entries()
    starts = [e for e in entries if e.kind == "feature_read"]
    assert len(starts) >= 2          # 起始 1 次 + 手动 1 次
    assert before == 1
    assert starts[0].report_id == 237
    assert starts[0].data.startswith("ed aa aa")


def test_snapshot_feature_failure_recorded_as_error(install):
    dev = FakeDev()
    dev.get_feature_report = lambda report_id, size: []     # 空响应
    install(dev)
    rec = Recorder([RecorderTarget(label="FF22", path="\\\\?\\x", feature_ids=(170,))])
    count = rec.snapshot_features()
    assert count == 1
    entry = rec.entries()[0]
    assert entry.kind == "feature_read" and entry.data == ""   # 无响应存空串


def test_read_error_kills_target_with_error_entry(install):
    install(DyingDev())
    rec = Recorder([RecorderTarget(label="raw", path="\\\\?\\x")])
    rec.start()
    ok = wait_until(lambda: any(e.kind == "error" for e in rec.entries()))
    rec.stop()
    assert ok
    assert "input 读取异常" in rec.entries()[-1].data


def test_open_failure_recorded_as_error(install):
    def boom(path):
        raise OSError("no such device")
    monkeypatch = pytest.MonkeyPatch()
    monkeypatch.setattr(rio, "_open_device", boom)
    try:
        rec = Recorder([RecorderTarget(label="ghost", path="\\\\?\\none")])
        rec.start()
        assert wait_until(lambda: any(e.kind == "error" for e in rec.entries()))
        rec.stop()
        assert "打开失败" in rec.entries()[0].data
    finally:
        monkeypatch.undo()


def test_start_twice_rejected(install):
    install(FakeDev())
    rec = Recorder([RecorderTarget(label="x", path="\\\\?\\x")])
    rec.start()
    try:
        with pytest.raises(RuntimeError):
            rec.start()
    finally:
        rec.stop()


def test_stop_terminates_threads(install):
    install(FakeDev())
    rec = Recorder([RecorderTarget(label="x", path="\\\\?\\x")])
    rec.start()
    assert rec.is_recording
    rec.stop(timeout=3.0)
    assert not rec.is_recording
    assert rec._threads == []


def test_jsonl_roundtrip(install, tmp_path):
    install(FakeDev(responses=[[0xED, 1, 2]]))
    rec = Recorder([RecorderTarget(label="FF21", path="\\\\?\\x")])
    rec.start()
    wait_until(lambda: rec.entries())
    rec.stop()

    path = tmp_path / "session.jsonl"
    written = rec.save_jsonl(path)
    loaded = Recorder.load_jsonl(path)
    assert written == len(rec.entries()) == len(loaded)
    assert loaded[0].data == rec.entries()[0].data
    assert loaded[0].kind == "input"
    assert isinstance(loaded[0], CaptureEntry)


def test_jsonl_empty_lines_tolerated(install, tmp_path):
    install(FakeDev())
    rec = Recorder([RecorderTarget(label="x", path="\\\\?\\x")])
    path = tmp_path / "s.jsonl"
    path.write_text('\n{"ts": 1.0, "source": "a", "kind": "input", "report_id": 5, "data": "aa"}\n\n',
                    encoding="utf-8")
    loaded = Recorder.load_jsonl(path)
    assert len(loaded) == 1 and loaded[0].report_id == 5


def test_max_entries_overflow_stops(install):
    install(FakeDev(responses=[[1, 2]] * 50))
    rec = Recorder([RecorderTarget(label="x", path="\\\\?\\x")], max_entries=3)
    rec.start()
    wait_until(lambda: rec.overflowed)
    rec.stop()
    assert rec.overflowed
    assert len(rec.entries()) == 3


def test_multiple_targets_isolated(install):
    # 每个目标一个独立假设备（按 path 注入），label 区分来源
    install({
        "\\\\?\\a": FakeDev(responses=[[0xED, 1]]),
        "\\\\?\\b": FakeDev(responses=[[0xEE, 2]]),
    })
    rec = Recorder([
        RecorderTarget(label="FF21", path="\\\\?\\a"),
        RecorderTarget(label="rawhid", path="\\\\?\\b"),
    ])
    rec.start()
    wait_until(lambda: len([e for e in rec.entries() if e.kind == "input"]) >= 2)
    rec.stop()
    sources = {e.source for e in rec.entries() if e.kind == "input"}
    assert sources == {"FF21", "rawhid"}

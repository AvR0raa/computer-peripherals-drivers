"""任务 1.1 单元测试：core/device_manager.py。

hidapi 相关函数全部用 monkeypatch 假枚举，离线运行、不碰真机。
"""
import threading

import pytest

import core.device_manager as dm
from core.device_manager import (
    DeviceEvent,
    DeviceInfo,
    HIDError,
    diff_snapshots,
    find_devices,
    list_devices,
    snapshot,
    watch_devices,
)


def fake_raw(path="\\\\?\\test", vid=0x046D, pid=0xC54D, product="USB Receiver",
             interface=1, usage_page=1, usage=0x80, **extra) -> dict:
    d = {
        "path": path.encode() if isinstance(path, str) else path,
        "vendor_id": vid,
        "product_id": pid,
        "serial_number": "TESTSER",
        "release_number": 0x1404,
        "manufacturer_string": "Logitech",
        "product_string": product,
        "usage_page": usage_page,
        "usage": usage,
        "interface_number": interface,
        "bus_type": 1,
    }
    d.update(extra)
    return d


def patch_enumerate(monkeypatch, sequence):
    """让 hid.enumerate 依次返回 sequence 中的每一项。"""
    calls = {"n": 0}

    def fake_enumerate():
        idx = min(calls["n"], len(sequence) - 1)
        calls["n"] += 1
        return sequence[idx]

    monkeypatch.setattr(dm.hid, "enumerate", fake_enumerate)


# ---------------------------------------------------------------------------
# DeviceInfo
# ---------------------------------------------------------------------------

def test_from_hid_dict_and_hex_properties():
    info = DeviceInfo.from_hid_dict(fake_raw())
    assert info.vid_hex == "046D"
    assert info.pid_hex == "C54D"
    assert info.product == "USB Receiver"
    assert info.serial_number == "TESTSER"


def test_from_hid_dict_tolerates_none_and_str_path():
    info = DeviceInfo.from_hid_dict(fake_raw(path=None, serial_number=None))
    assert info.path == ""       # path 为 None 时兜底为空串，不崩、不出 "None"
    assert info.serial_number == ""
    info2 = DeviceInfo.from_hid_dict(fake_raw())
    assert info2.path.startswith("\\\\?\\")     # bytes path 正确解码


# ---------------------------------------------------------------------------
# list_devices / find_devices / snapshot
# ---------------------------------------------------------------------------

def test_list_devices(monkeypatch):
    patch_enumerate(monkeypatch, [[fake_raw("a"), fake_raw("b", interface=2)]])
    devices = list_devices()
    assert len(devices) == 2
    assert all(isinstance(d, DeviceInfo) for d in devices)


def test_list_devices_wraps_error(monkeypatch):
    def boom():
        raise OSError("usb error")
    monkeypatch.setattr(dm.hid, "enumerate", boom)
    with pytest.raises(HIDError):
        list_devices()


def test_find_devices_filters(monkeypatch):
    patch_enumerate(monkeypatch, [[
        fake_raw("a"),
        fake_raw("b", vid=0x1532, pid=0x0045, product="Razer"),
        fake_raw("c", pid=0xC088, product="G502"),
    ]])
    logitech = find_devices(vendor_id=0x046D)
    assert {d.path for d in logitech} == {"a", "c"}
    exact = find_devices(vendor_id=0x046D, product_id=0xC54D)
    assert [d.path for d in exact] == ["a"]
    all_devices = find_devices()
    assert len(all_devices) == 3
    # 传入现成列表则不重新枚举
    again = find_devices(vendor_id=0x1532, devices=all_devices)
    assert [d.path for d in again] == ["b"]


def test_snapshot_keys_are_paths(monkeypatch):
    patch_enumerate(monkeypatch, [[fake_raw("a"), fake_raw("b")]])
    snap = snapshot()
    assert set(snap) == {"a", "b"}


# ---------------------------------------------------------------------------
# diff_snapshots（热插拔核心，纯函数）
# ---------------------------------------------------------------------------

def _dev(path):
    return DeviceInfo.from_hid_dict(fake_raw(path))


def test_diff_empty_to_two_adds():
    events = diff_snapshots({}, {"a": _dev("a"), "b": _dev("b")})
    assert [e.kind for e in events] == ["added", "added"]
    assert [e.device.path for e in events] == ["a", "b"]


def test_diff_removal():
    events = diff_snapshots({"a": _dev("a"), "b": _dev("b")}, {"b": _dev("b")})
    assert [(e.kind, e.device.path) for e in events] == [("removed", "a")]


def test_diff_no_change_and_stable_order():
    snap = {"a": _dev("a"), "b": _dev("b"), "c": _dev("c")}
    assert diff_snapshots(snap, dict(snap)) == []
    old = {"b": _dev("b"), "x": _dev("x")}
    new = {"b": _dev("b"), "a": _dev("a"), "y": _dev("y")}
    events = diff_snapshots(old, new)
    assert [(e.kind, e.device.path) for e in events] == [
        ("added", "a"), ("added", "y"), ("removed", "x"),
    ]


# ---------------------------------------------------------------------------
# watch_devices
# ---------------------------------------------------------------------------

def test_watch_devices_emits_events_and_stops(monkeypatch):
    # 第 1 轮：基线；第 2 轮：插入 a；第 3 轮：拔出 a 插入 b
    patch_enumerate(monkeypatch, [
        [fake_raw("old")],
        [fake_raw("old"), fake_raw("a")],
        [fake_raw("old"), fake_raw("b")],
    ])
    events: list[DeviceEvent] = []

    def on_event(event):
        events.append(event)
        if len(events) >= 2:
            stop.set()

    stop = threading.Event()
    watch_devices(on_event, interval=0.01, stop=stop)
    assert [(e.kind, e.device.path) for e in events] == [
        ("added", "a"), ("added", "b"), ("removed", "a"),
    ]


def test_watch_devices_survives_enumerate_error(monkeypatch):
    calls = {"n": 0}
    real_snap = {"old": _dev("old")}

    def flaky_enumerate():
        calls["n"] += 1
        if calls["n"] == 1:
            return [fake_raw("old")]              # 基线：只有 old
        if calls["n"] == 2:
            raise OSError("transient usb error")  # 第 2 轮瞬时失败
        return [fake_raw("old"), fake_raw("a")]   # 第 3 轮起插入 a

    monkeypatch.setattr(dm.hid, "enumerate", flaky_enumerate)
    events: list[DeviceEvent] = []

    def on_event(event):
        events.append(event)
        stop.set()

    stop = threading.Event()
    watch_devices(on_event, interval=0.01, stop=stop)
    assert [(e.kind, e.device.path) for e in events] == [("added", "a")]
    assert calls["n"] >= 3   # 出错那轮没中断监听

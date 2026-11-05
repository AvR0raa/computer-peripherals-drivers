"""设备枚举与热插拔监听（M1 只读，不打开设备）。

枚举依赖 hidapi（hid 模块）；热插拔用"快照差分轮询"实现，
跨平台无原生依赖。差分逻辑是纯函数，可离线单元测试。
"""
from __future__ import annotations

import threading
from dataclasses import dataclass

import hid


class HIDError(RuntimeError):
    """hidapi 调用失败。"""


@dataclass(frozen=True)
class DeviceInfo:
    """一个 HID 接口（不是物理设备：一个鼠标可能枚举出多个接口）。"""

    path: str              # 平台设备路径（同一快照内的唯一键）
    vendor_id: int
    product_id: int
    serial_number: str
    release_number: int    # BCD 版本号
    manufacturer: str
    product: str
    usage_page: int
    usage: int
    interface_number: int
    bus_type: int

    @classmethod
    def from_hid_dict(cls, raw: dict) -> "DeviceInfo":
        path = raw.get("path") or b""
        if isinstance(path, bytes):
            path = path.decode("utf-8", "replace")
        return cls(
            path=str(path),
            vendor_id=int(raw.get("vendor_id", 0)),
            product_id=int(raw.get("product_id", 0)),
            serial_number=str(raw.get("serial_number") or ""),
            release_number=int(raw.get("release_number", 0)),
            manufacturer=str(raw.get("manufacturer_string") or ""),
            product=str(raw.get("product_string") or ""),
            usage_page=int(raw.get("usage_page", 0)),
            usage=int(raw.get("usage", 0)),
            interface_number=int(raw.get("interface_number", 0)),
            bus_type=int(raw.get("bus_type", 0)),
        )

    @property
    def vid_hex(self) -> str:
        return f"{self.vendor_id:04X}"

    @property
    def pid_hex(self) -> str:
        return f"{self.product_id:04X}"


@dataclass(frozen=True)
class DeviceEvent:
    """热插拔事件。"""

    kind: str              # "added" | "removed"
    device: DeviceInfo


def list_devices() -> list[DeviceInfo]:
    """枚举当前全部 HID 接口。失败抛 HIDError。"""
    try:
        raw_list = hid.enumerate()
    except Exception as exc:  # HIDException / OSError 均可能
        raise HIDError(f"hid.enumerate() 失败: {exc}") from exc
    return [DeviceInfo.from_hid_dict(raw) for raw in raw_list]


def find_devices(
    vendor_id: int | None = None,
    product_id: int | None = None,
    devices: list[DeviceInfo] | None = None,
) -> list[DeviceInfo]:
    """按 VID/PID 过滤（可只给其一）；devices 缺省时重新枚举。"""
    pool = devices if devices is not None else list_devices()
    return [
        d for d in pool
        if (vendor_id is None or d.vendor_id == vendor_id)
        and (product_id is None or d.product_id == product_id)
    ]


def snapshot() -> dict[str, DeviceInfo]:
    """当前枚举结果转成 {path: DeviceInfo}。"""
    return {d.path: d for d in list_devices()}


def diff_snapshots(
    old: dict[str, DeviceInfo],
    new: dict[str, DeviceInfo],
) -> list[DeviceEvent]:
    """纯函数：比较两个快照，产出 added/removed 事件。

    added 在前、removed 在后，各自按 path 排序，保证输出稳定。
    """
    added = [DeviceEvent("added", new[p]) for p in sorted(new.keys() - old.keys())]
    removed = [DeviceEvent("removed", old[p]) for p in sorted(old.keys() - new.keys())]
    return added + removed


def watch_devices(
    on_event: "callable",
    interval: float = 1.0,
    stop: threading.Event | None = None,
) -> None:
    """阻塞式热插拔监听：每 interval 秒枚举一次做差分。

    on_event(DeviceEvent) 对每个事件同步回调；回调抛异常会中断监听。
    单次枚举失败（设备插拔瞬间的瞬时错误）跳过本轮，不中断监听。
    stop 为 threading.Event，置位后循环退出。
    """
    stop_event = stop if stop is not None else threading.Event()
    old = snapshot()
    while not stop_event.wait(interval):
        try:
            new = snapshot()
        except HIDError:
            continue  # 瞬时枚举失败：保留旧快照，下轮重试
        for event in diff_snapshots(old, new):
            on_event(event)
        old = new

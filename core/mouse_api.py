"""鼠标设置统一 API（模板层，M2.5c）。

目标：GUI/CLI 不再直接碰协议字节，只调这一层；新增设备品牌 = 新增一个
Backend 子类并注册到 BACKENDS，界面零改动。

    api = MouseApi.detect(ifaces)          # 自动选后端（罗技/模板）
    api.caps                               # MouseCapabilities(档位/报告率/电池)
    api.get_dpi() / api.set_dpi(1600)
    api.get_poll_rate() / api.set_poll_rate(1000)
    api.get_battery()
    api.summary()                          # CLI/GUI 直接渲染的 dict

新后端模板（复制即用）：

    class MyVendorBackend(MouseBackend):
        def matches(self, vid_hex, ifaces) -> bool: ...
        def capabilities(self) -> MouseCapabilities: ...
        def get_dpi(self) -> ... / set_dpi(self, v) -> DpiResult
        def get_poll_rate(self) -> int|None / set_poll_rate(self, hz) -> (bool, str)
        def get_battery(self) -> BatteryResult|None
"""
from __future__ import annotations

from dataclasses import dataclass, field

from core.battery import BatteryResult, scan_all
from core.dpi import DpiOutcome, get_dpi_list, read_dpi, set_dpi
from core.hidpp import (
    HidppLink,
    ensure_online,
    get_poll_rates,
    read_battery,
    read_poll_rate,
    set_poll_rate,
)


@dataclass
class MouseCapabilities:
    dpi_list: list[int] = field(default_factory=list)
    dpi_support: bool = False
    poll_rates: list[int] = field(default_factory=list)      # Hz，从高到低
    poll_support: bool = False
    battery_support: bool = False
    notes: list[str] = field(default_factory=list)


@dataclass
class DpiResult:
    ok: bool
    value: int | None = None
    readback: int | None = None
    rolled_back: bool = False
    log: list[str] = field(default_factory=list)


class MouseBackend:
    """后端模板：子类实现全部方法并注册。"""

    def matches(self, vid_hex: str, ifaces: list) -> bool:
        raise NotImplementedError

    def build(self, ifaces: list) -> None:
        raise NotImplementedError

    def capabilities(self) -> MouseCapabilities:
        raise NotImplementedError

    def get_dpi(self) -> tuple[int | None, int | None, int | None, int | None]:
        """(dpi_x, dpi_y, lod, default)。"""
        raise NotImplementedError

    def set_dpi(self, value: int) -> DpiOutcome:
        raise NotImplementedError

    def get_poll_rate(self) -> int | None:
        raise NotImplementedError

    def set_poll_rate(self, hz: int) -> tuple[bool, str]:
        raise NotImplementedError

    def get_battery(self) -> BatteryResult | None:
        raise NotImplementedError

    @property
    def brand(self) -> str:
        raise NotImplementedError


class LogitechBackend(MouseBackend):
    """罗技 HID++ 2.0 后端（0x2201/0x2202 DPI + 0x8060/0x8061 报告率 + 电量三路）。"""

    def __init__(self):
        self._link: HidppLink | None = None
        self._dev_resolved: int | None = None   # 惰性探测：0xFF=有线直连 / 1-7=接收器槽位
        self._dpi_cache: list[int] = []

    def matches(self, vid_hex: str, ifaces: list) -> bool:
        return vid_hex == "046D" and any(
            f"{d.usage_page:04X}" == "FF00" for d in ifaces)

    def build(self, ifaces: list) -> None:
        """只建链路，不猜设备索引——有线直连(0xFF)与接收器槽位在首次使用时探测。"""
        short = next((d for d in ifaces
                      if f"{d.usage_page:04X}" == "FF00" and f"{d.usage:04X}" == "0001"), None)
        long_if = next((d for d in ifaces
                        if f"{d.usage_page:04X}" == "FF00" and f"{d.usage:04X}" == "0002"), None)
        self._link = HidppLink(short.path if short else None,
                               long_if.path if long_if else None)
        self._dev_resolved = None

    def _dev(self) -> int | None:
        """自动探测 HID++ 设备索引：先 0xFF（有线直连/接收器自身），再槽位 1-7。

        有线鼠标（如 PRO X2 有线模式 C09B）直插时必须走 0xFF；
        无线配对设备走槽位。探测一次后缓存。
        """
        if self._dev_resolved is not None:
            return self._dev_resolved
        if self._link is None:
            return None
        from core.hidpp import ping
        for cand in (0xFF, 1, 2, 3, 4, 5, 6, 7):
            state, _ = ping(self._link, cand, timeout_ms=400)
            if state in ("2.0", "1.0"):
                self._dev_resolved = cand
                return cand
        return None

    def _online(self, attempts: int = 10) -> bool:
        if self._link is None:
            return False
        return ensure_online(self._link, self._dev or 1, attempts=attempts) is not None

    def capabilities(self) -> MouseCapabilities:
        caps = MouseCapabilities()
        if self._link is None:
            return caps
        with self._link:
            self._dev_resolved = None
            dev = self._dev()
            if dev is None:
                caps.notes.append("接收器上没有配对/在线设备（睡眠时请移动鼠标，HID++ 流量会唤醒）")
                return caps
            if not self._online():
                caps.notes.append("设备不在线（睡眠时请移动鼠标，HID++ 流量会唤醒）")
                return caps
            self._dpi_cache = get_dpi_list(self._link, self._dev() or 1)
            caps.dpi_list = self._dpi_cache
            caps.dpi_support = bool(caps.dpi_list)
            caps.poll_rates = get_poll_rates(self._link, self._dev() or 1)
            caps.poll_support = bool(caps.poll_rates)
        if not caps.dpi_support:
            caps.notes.append("未发现 DPI feature（可能开启板载配置档，需 G HUB 切 Host 模式）")
        return caps

    def get_dpi(self):
        with self._link:
            return read_dpi(self._link, self._dev() or 1)

    def set_dpi(self, value: int) -> DpiOutcome:
        with self._link:
            outcome = set_dpi(self._link, self._dev() or 1, value)
        return DpiOutcome(ok=outcome.ok, wrote=outcome.wrote,
                          readback=outcome.readback,
                          rolled_back=outcome.rolled_back, log=outcome.log)

    def get_poll_rate(self) -> int | None:
        with self._link:
            return read_poll_rate(self._link, self._dev() or 1)

    def set_poll_rate(self, hz: int) -> tuple[bool, str]:
        with self._link:
            return set_poll_rate(self._link, self._dev() or 1, hz)

    def get_battery(self) -> BatteryResult | None:
        with self._link:
            b = read_battery(self._link, self._dev() or 1)
        if b is None:
            return None
        return BatteryResult(vidpid="", percent=b.percent, voltage_mv=b.voltage_mv,
                             source=b.source, detail=b.detail)

    @property
    def brand(self) -> str:
        return "Logitech"


# 后端注册表：新增品牌 = 加一个 Backend 子类 + 一行注册（模板化扩展点）
BACKENDS: list[type] = [LogitechBackend]


def detect(ifaces: list) -> MouseApi | None:
    """探测设备可用的鼠标设置后端；不支持返回 None。"""
    if not ifaces:
        return None
    vid_hex = ifaces[0].vid_hex
    for backend_cls in BACKENDS:
        backend = backend_cls()
        if backend.matches(vid_hex, ifaces):
            backend.build(ifaces)
            return MouseApi(backend)
    return None


class MouseApi:
    """统一入口：GUI/CLI 只 import 这一个模块。"""

    def __init__(self, backend: MouseBackend):
        self.backend = backend

    @property
    def brand(self) -> str:
        return self.backend.brand

    def capabilities(self) -> MouseCapabilities:
        return self.backend.capabilities()

    def get_dpi(self):
        return self.backend.get_dpi()

    def set_dpi(self, value: int) -> DpiOutcome:
        if not (50 <= value <= 32000):
            return DpiOutcome(ok=False, log=[f"拒绝: DPI {value} 超出安全范围"])
        return self.backend.set_dpi(value)

    def get_poll_rate(self) -> int | None:
        return self.backend.get_poll_rate()

    def set_poll_rate(self, hz: int) -> tuple[bool, str]:
        return self.backend.set_poll_rate(hz)

    def get_battery(self, all_ifaces: list | None = None) -> BatteryResult | None:
        result = self.backend.get_battery()
        if result is not None:
            return result
        # 后端不支持时走通用编排（标准页/厂商协议）
        ifaces = all_ifaces or []
        if ifaces:
            grouped = {f"{ifaces[0].vid_hex}:{ifaces[0].pid_hex}": ifaces}
            results = scan_all(grouped, read_descriptor=_noop_reader)
            return results[0] if results else None
        return None

    def summary(self) -> dict:
        caps = self.capabilities()
        return {
            "brand": self.brand,
            "dpi_list": caps.dpi_list,
            "poll_rates": caps.poll_rates,
            "battery_support": caps.battery_support,
            "notes": caps.notes,
        }


def _noop_reader(path):
    return None

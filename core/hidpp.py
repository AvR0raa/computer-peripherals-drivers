"""HID++ 2.0 最小客户端（电量/在线探测用）。

报文布局移植自 Solaar (lib/logitech_receiver/base.py + hidpp20*.py)，来源已标注：
- 短报文: [0x10, dev, reqHi, reqLo, p0, p1, p2]，reqHi=reqHi(featIdx)、reqLo=(funct<<4)|sw_id
- feature 查询: Root(0x0000) funct 0，params=[featIdHi, featIdLo]，应答首字节=feature index(0=无)
- 电量 feature: 0x1000 BatteryStatus(funct0: level,next,status)
                0x1001 BatteryVoltage(funct0: vHi,vLo,flags)
                0x1004 UnifiedBattery(funct1: discharge,level,status)
- ping: [0x10, dev, 0x00, 0x10|sw_id, 0, 0, mark]，应答 [.., major, minor, mark]；
  错误应答 [0x10, dev, 0x8F, reqHi, reqLo, errCode, 0]
"""
from __future__ import annotations

import time as _time
from dataclasses import dataclass

from core.report_io import ReportIO, ReportError, match_response

SW_ID = 5    # 兼容常量保留；实际 sw_id 每请求轮换（见 _next_sw_id）

_sw_state = {"n": 0}


def _next_sw_id() -> int:
    """每请求轮换 sw_id(1-7)：应答回显 funct|sw_id，轮换让匹配无歧义。

    （实测 C54D+PRO X2：固定 sw_id 时前一个慢应答会被误配给下一个请求）
    """
    _sw_state["n"] = (_sw_state["n"] % 7) + 1
    return _sw_state["n"]

FEATURE_ROOT = 0x0000
FEATURE_BATTERY_STATUS = 0x1000
FEATURE_BATTERY_VOLTAGE = 0x1001
FEATURE_UNIFIED_BATTERY = 0x1004
FEATURE_ADJUSTABLE_DPI = 0x2201
FEATURE_EXTENDED_ADJUSTABLE_DPI = 0x2202
FEATURE_REPORT_RATE = 0x8060
FEATURE_EXTENDED_REPORT_RATE = 0x8061

# 0x8061 Extended Report Rate: 索引式档位（Solaar ExtendedReportRate choices_universe）
POLL_RATE_BY_INDEX = {0: 125, 1: 250, 2: 500, 3: 1000, 4: 2000, 5: 4000, 6: 8000}
POLL_INDEX_BY_HZ = {v: k for k, v in POLL_RATE_BY_INDEX.items()}

# BatteryStatus 语义（Solaar hidpp20_constants.BatteryStatus 子集）
STATUS_NAMES = {0: "放电中", 1: "充电中", 2: "接近充满", 4: "已充满", 5: "电量不足(?)"}


@dataclass
class HidppBattery:
    percent: int | None
    voltage_mv: int | None
    charging: bool | None
    status_raw: int | None
    source: str          # "hidpp-unified" | "hidpp-status" | "hidpp-voltage"
    detail: str


class HidppLink:
    """HID++ 短/长报文双通道封装。

    接收器把 0x10(短)/0x11(长) 报文分在两个 FF00 接口上（C54D 实测：
    FF00:0001=短 6B，FF00:0002=长 19B）。本类按报文 ID 自动路由；
    单接口设备（或测试假设备）只给 short_path 即可，两种报文都走它。
    """

    def __init__(self, short_path=None, long_path=None):
        self.short_io = ReportIO(short_path) if short_path else None
        self.long_io = ReportIO(long_path) if long_path else None

    def open(self):
        for io in (self.short_io, self.long_io):
            if io is not None:
                io.open()

    def close(self):
        for io in (self.short_io, self.long_io):
            if io is not None:
                io.close()

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *exc):
        self.close()

    def call_multi(self, wire: bytes, checks, params, timeout_ms):
        """按 wire[0] 选发送通道；双通道时两路输入轮询等应答。

        HID++ 的应答长度与请求无关（短请求的长应答会出现在长通道），
        所以双通道设备必须两边都听。单通道设备行为与 ReportIO 相同。
        """
        tx = self.long_io if (wire[0] == 0x11 and self.long_io is not None) else self.short_io
        if tx is None:
            raise ReportError("HID++ 缺少所需通道（短/长）")
        if self.long_io is None or self.short_io is None:
            return tx.call_multi(wire, checks, params, timeout_ms)

        tx.write_output(wire)
        deadline = _time.monotonic() + timeout_ms / 1000
        while True:
            remaining_ms = int((deadline - _time.monotonic()) * 1000)
            if remaining_ms <= 0:
                return None
            for io in (self.short_io, self.long_io):
                response = io.read_input(timeout_ms=min(remaining_ms, 30))
                if response is not None:
                    for label, check in checks:
                        if check is None or match_response(response, check, params or {}):
                            return response, label


def _call_feature(
    link,
    dev: int,
    feature_index: int,
    funct: int = 0,
    params: bytes = b"",
    timeout_ms: int = 500,
    long: bool | None = None,
) -> bytes | None:
    """执行一次 feature 调用，返回应答 payload（req-id 回显之后的部分）。

    link: ReportIO（单通道）或 HidppLink（双通道自动路由）。
    long=None 时按参数长度自动选择（>3 字节走长报文）。
    错误应答（0x8F）或超时返回 None。
    """
    req_lo = ((funct & 0x0F) << 4) | _next_sw_id()
    if long is None:
        long = len(params) > 3
    if long:
        wire = (bytes([0x11, dev, feature_index, req_lo]) + params[:16]).ljust(20, b"\x00")
    else:
        wire = (bytes([0x10, dev, feature_index, req_lo]) + params[:3]).ljust(7, b"\x00")
    # 应答报文长度与请求无关（短请求可能得长应答），check 兼容两种 rid
    check = [[0x10, 0x11], dev, feature_index, req_lo]
    err_check = [[0x10, 0x11], dev, 0x8F]
    result = link.call_multi(wire, [("primary", check), ("err", err_check)], {}, timeout_ms)
    if result is None or result[1] == "err":
        return None
    return result[0][4:20]


def get_feature_index(io: ReportIO, dev: int, feature_id: int, timeout_ms: int = 500) -> int | None:
    """Root.GetFeature：查询 feature_id 分配的 index；设备不支持返回 None。"""
    payload = _call_feature(
        io, dev, FEATURE_ROOT, 0,
        bytes([(feature_id >> 8) & 0xFF, feature_id & 0xFF]),
        timeout_ms,
    )
    if payload is None:
        return None
    index = payload[0]
    return index if index else None


def ping(link, dev: int, mark: int = 66, timeout_ms: int = 500) -> tuple[str, float | None]:
    """HID++ ping。返回 (状态, 协议版本)。

    双通道时先试长报文（新款设备如 PRO X2 只应答长报文，实测 C54D 槽位），
    错误 0x08(UNKNOWN_DEVICE) 则换通道重试；单通道只试短报文。
    注意：实测 PRO X2 的 ping 应答不回显 mark 字节，故校验只到 reqLo。
    """
    attempts: list[bool] = []
    if isinstance(link, HidppLink) and link.long_io is not None:
        attempts.append(True)
    attempts.append(False)

    seen_err: int | None = None
    for long in attempts:
        rid = 0x11 if long else 0x10
        sw = _next_sw_id()
        wire = bytes([rid, dev, 0x00, 0x10 | sw, 0, 0, mark]).ljust(
            20 if long else 7, b"\x00")
        checks = [
            ("2.0", [rid, dev, 0x00, 0x10 | sw, None, None]),
            ("err", [rid, dev, 0x8F]),
        ]
        result = link.call_multi(wire, checks, {}, timeout_ms)
        if result is None:
            continue
        response, label = result
        if label == "2.0":
            return "2.0", response[4] + response[5] / 10.0
        error_code = response[5] if len(response) > 5 else 0
        if error_code == 0x01:      # INVALID_SUB_ID_COMMAND → HID++ 1.0 设备
            return "1.0", 1.0
        seen_err = error_code
    return ("empty-slot" if seen_err == 0x08 else "offline", None)


def ensure_online(link, dev: int, attempts: int = 8, interval: float = 1.2,
                  timeout_ms: int = 600) -> float | None:
    """反复 ping 直到设备在线（HID++ 流量本身会唤醒睡眠设备，fwupd #7011 佐证）。

    返回协议版本；全程不在线返回 None。
    """
    for _ in range(attempts):
        state, version = ping(link, dev, timeout_ms=timeout_ms)
        if state == "2.0":
            return version
        if attempts > 1:
            _time.sleep(interval)
    return None


# ---------------------------------------------------------------------------
# 报告率（0x8060 基础版 / 0x8061 扩展版，Solaar ReportRate + ExtendedReportRate）
# ---------------------------------------------------------------------------

def get_poll_rates(link, dev: int, timeout_ms: int = 500) -> list[int]:
    """支持的报告率列表（Hz，从高到低）。不支持返回 []。"""
    idx = get_feature_index(link, dev, FEATURE_EXTENDED_REPORT_RATE, timeout_ms)
    if idx:
        payload = _call_feature(link, dev, idx, 0x10, timeout_ms=timeout_ms)
        if payload is not None and len(payload) >= 2:
            flags = payload[0] | (payload[1] << 8)
            rates = [POLL_RATE_BY_INDEX[i] for i in range(7) if (flags >> i) & 0x01]
            return sorted(rates, reverse=True)
    idx = get_feature_index(link, dev, FEATURE_REPORT_RATE, timeout_ms)
    if idx:
        payload = _call_feature(link, dev, idx, 0x00, timeout_ms=timeout_ms)
        if payload is not None and len(payload) >= 1:
            flags = payload[0]
            rates = [1000 // (i + 1) for i in range(8) if (flags >> i) & 0x01]
            return sorted(rates, reverse=True)
    return []


def read_poll_rate(link, dev: int, timeout_ms: int = 500) -> int | None:
    """当前报告率（Hz）。不支持返回 None。"""
    idx = get_feature_index(link, dev, FEATURE_EXTENDED_REPORT_RATE, timeout_ms)
    if idx:
        payload = _call_feature(link, dev, idx, 0x20, timeout_ms=timeout_ms)
        if payload is not None and len(payload) >= 1:
            return POLL_RATE_BY_INDEX.get(payload[0])
    idx = get_feature_index(link, dev, FEATURE_REPORT_RATE, timeout_ms)
    if idx:
        payload = _call_feature(link, dev, idx, 0x10, timeout_ms=timeout_ms)
        if payload is not None and len(payload) >= 1 and payload[0]:
            return 1000 // payload[0]        # 基础版: 字节 = ms
    return None


def set_poll_rate(link, dev: int, hz: int, timeout_ms: int = 600) -> tuple[bool, str]:
    """设置报告率；读回验证。返回 (是否成功, 说明)。"""
    rates = get_poll_rates(link, dev, timeout_ms)
    if not rates:
        return False, "设备不支持报告率调节（0x8060/0x8061 均无）"
    if hz not in rates:
        return False, f"报告率 {hz}Hz 不在支持列表 {rates}"
    idx = get_feature_index(link, dev, FEATURE_EXTENDED_REPORT_RATE, timeout_ms)
    if idx:
        index = POLL_INDEX_BY_HZ[hz]
        payload = _call_feature(link, dev, idx, 0x30, bytes([index]), timeout_ms)
        confirm = read_poll_rate(link, dev, timeout_ms)
        if confirm == hz:
            return True, f"报告率已设为 {hz}Hz（读回验证通过）"
        return False, f"验证失败：读回 {confirm}Hz（板载配置档开启时写入可能不生效）"
    idx = get_feature_index(link, dev, FEATURE_REPORT_RATE, timeout_ms)
    if idx:
        ms = 1000 // hz
        _call_feature(link, dev, idx, 0x20, bytes([ms]), timeout_ms)
        confirm = read_poll_rate(link, dev, timeout_ms)
        if confirm == hz:
            return True, f"报告率已设为 {hz}Hz（读回验证通过）"
        return False, f"验证失败：读回 {confirm}Hz"
    return False, "无可用报告率 feature"


def read_battery(io: ReportIO, dev: int = 0xFF, timeout_ms: int = 500) -> HidppBattery | None:
    """读电量：Unified(0x1004) → Status(0x1000) → Voltage(0x1001) 三路回退。

    全部不可用返回 None。
    """
    # 1) Unified Battery 0x1004 funct 1: [discharge, level, status]
    idx = get_feature_index(io, dev, FEATURE_UNIFIED_BATTERY, timeout_ms)
    if idx:
        payload = _call_feature(io, dev, idx, 1, timeout_ms=timeout_ms)
        if payload is not None:
            discharge, level, status = payload[0], payload[1], payload[2]
            approx = {8: "满", 4: "良好", 2: "低", 1: "危急"}.get(level, f"0x{level:02X}")
            percent = discharge if discharge else None
            return HidppBattery(
                percent=percent, voltage_mv=None,
                charging=status in (1, 2), status_raw=status,
                source="hidpp-unified",
                detail=f"统一电量: {percent if percent is not None else approx + '(档位)'}%"
                       f"{' 充电中' if status in (1, 2) else ''}",
            )

    # 2) Battery Status 0x1000 funct 0: [level, next, status]
    idx = get_feature_index(io, dev, FEATURE_BATTERY_STATUS, timeout_ms)
    if idx:
        payload = _call_feature(io, dev, idx, 0, timeout_ms=timeout_ms)
        if payload is not None:
            level, _next, status = payload[0], payload[1], payload[2]
            return HidppBattery(
                percent=level or None, voltage_mv=None,
                charging=status in (1, 2), status_raw=status,
                source="hidpp-status",
                detail=f"电量 {level}% ({STATUS_NAMES.get(status, f'0x{status:02X}')})",
            )

    # 3) Battery Voltage 0x1001 funct 0: [vHi, vLo, flags]
    idx = get_feature_index(io, dev, FEATURE_BATTERY_VOLTAGE, timeout_ms)
    if idx:
        payload = _call_feature(io, dev, idx, 0, timeout_ms=timeout_ms)
        if payload is not None:
            voltage = (payload[0] << 8) | payload[1]
            flags = payload[2]
            return HidppBattery(
                percent=None, voltage_mv=voltage,
                charging=bool(flags & 0x80), status_raw=flags,
                source="hidpp-voltage",
                detail=f"电压 {voltage} mV{' 充电中' if flags & 0x80 else ''}",
            )

    return None

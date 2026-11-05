"""鼠标 DPI 读写（M2.5b）— HID++ Adjustable DPI 0x2201 / Extended 0x2202。

布局移植自 Solaar settings_templates（AdjustableDpi + ExtendedAdjustableDpi +
produce_dpi_list）：
0x2201 Adjustable DPI:
- GetSensorDpiList: funct 1，params [sensorIdx, direction, page]；
  应答数据段 = 大端 16 位 DPI 序列直到 0x0000；val>>13==0b111 为步进编码
  [0xE000|step, last]，展开 range(prev+step, last+1, step)
- GetSensorDpi:  funct 2，payload [X, dpiHi, dpiLo, ...] → DPI = BE16@offset1
- SetSensorDpi:  funct 3，params [sensorIdx, dpiHi, dpiLo]
0x2202 Extended Adjustable DPI（PRO X2 等新款用这个，Solaar #2207/#2229）:
- GetInfo:       funct 1，params [0x00]；payload[2] bit0=Y轴 bit1=LOD
- GetDpiList:    funct 2，params [0x00, axis, page]；数据从 payload[3:] 起
- GetDpi:        funct 5，payload X@[1:3](0→默认@[3:5])，Y@[5:7](0→@[7:9])，LOD@[9]
- SetDpi:        funct 6，params [0x00, Xhi, Xlo, ...]

注意（Solaar 原话）：设备开"板载配置档(Onboard Profiles)"时主机侧 DPI 可能
不生效——需在 G HUB 里切到 Host 模式再调。
写安全：读旧值 → 写新值 → 读回验证 → 不匹配回滚旧值（plan.md 风险表）。
"""
from __future__ import annotations

from dataclasses import dataclass, field

from core.hidpp import (
    FEATURE_ADJUSTABLE_DPI,
    FEATURE_EXTENDED_ADJUSTABLE_DPI,
    _call_feature,
    get_feature_index,
)


@dataclass
class DpiOutcome:
    ok: bool
    wrote: int | None = None
    old: int | None = None
    readback: int | None = None
    rolled_back: bool = False
    feature: str = ""           # "0x2201" | "0x2202"
    log: list[str] = field(default_factory=list)


def _parse_dpi_values(raw: bytes) -> list[int]:
    """BE16 序列 + 0x0000 结尾 + 0b111 步进编码展开（Solaar produce_dpi_list）。"""
    values: list[int] = []
    i = 0
    while i + 1 < len(raw):
        val = (raw[i] << 8) | raw[i + 1]
        if val == 0:
            break
        if val >> 13 == 0b111:
            step = val & 0x1FFF
            if i + 3 >= len(raw):
                break
            last = (raw[i + 2] << 8) | raw[i + 3]
            if values and last > values[-1]:
                values.extend(range(values[-1] + step, last + 1, step))
            i += 4
        else:
            values.append(val)
            i += 2
    return values


def _dpi_list_standard(link, dev: int, idx: int, timeout_ms: int) -> list[int]:
    """0x2201 funct 1 分页列表（ignore=1）。"""
    raw = b""
    for page in range(0x100):
        payload = _call_feature(link, dev, idx, 1, bytes([0x00, 0x00, page]),
                                timeout_ms, long=True)
        if payload is None:
            break
        raw += payload[1:]
        if raw[-2:] == b"\x00\x00":
            break
    return _parse_dpi_values(raw)


def _dpi_list_extended(link, dev: int, idx: int, axis: int, timeout_ms: int) -> list[int]:
    """0x2202 funct 2 分页列表（ignore=3，params [0, axis, page]）。"""
    raw = b""
    for page in range(0x100):
        payload = _call_feature(link, dev, idx, 2, bytes([0x00, axis, page]),
                                timeout_ms, long=True)
        if payload is None:
            break
        raw += payload[3:]
        if raw[-2:] == b"\x00\x00":
            break
    return _parse_dpi_values(raw)


def _detect(link, dev: int, timeout_ms: int):
    """探测设备用哪个 DPI feature。返回 (name, index, y_axis, lod) 或 None。"""
    idx = get_feature_index(link, dev, FEATURE_ADJUSTABLE_DPI, timeout_ms)
    if idx:
        return ("0x2201", idx, False, False)
    idx = get_feature_index(link, dev, FEATURE_EXTENDED_ADJUSTABLE_DPI, timeout_ms)
    if idx:
        y = lod = False
        payload = _call_feature(link, dev, idx, 1, bytes([0x00]), timeout_ms)
        if payload is not None and len(payload) >= 3:
            y = bool(payload[2] & 0x01)
            lod = bool(payload[2] & 0x02)
        return ("0x2202", idx, y, lod)
    return None


def get_dpi_list(link, dev: int, timeout_ms: int = 500) -> list[int]:
    """设备支持的 DPI 档位表（含步进区间展开）。不支持返回 []。"""
    det = _detect(link, dev, timeout_ms)
    if det is None:
        return []
    name, idx, _y, _lod = det
    if name == "0x2201":
        return _dpi_list_standard(link, dev, idx, timeout_ms)
    return _dpi_list_extended(link, dev, idx, 0, timeout_ms)


def read_dpi(link, dev: int, timeout_ms: int = 500) -> tuple:
    """读当前 DPI，返回 (dpi_x, dpi_y, lod, 默认dpi)。失败返回全 None。"""
    det = _detect(link, dev, timeout_ms)
    if det is None:
        return (None, None, None, None)
    name, idx, has_y, has_lod = det
    if name == "0x2201":
        payload = _call_feature(link, dev, idx, 2, timeout_ms=timeout_ms)
        if payload is None or len(payload) < 3:
            return (None, None, None, None)
        dpi = (payload[1] << 8) | payload[2]
        default = None
        if len(payload) >= 5:
            d = (payload[3] << 8) | payload[4]
            default = d or None
        if dpi == 0:
            dpi = default                      # Solaar: 0 → 用默认值
        return (dpi, None, None, default)
    # 0x2202 funct 5: X@[1:3](0→默认@[3:5])，Y@[5:7](0→@[7:9])，LOD@[9]
    payload = _call_feature(link, dev, idx, 5, timeout_ms=timeout_ms)
    if payload is None or len(payload) < 3:
        return (None, None, None, None)
    dx = (payload[1] << 8) | payload[2]
    if dx == 0 and len(payload) >= 5:
        dx = (payload[3] << 8) | payload[4]
    dy = None
    if has_y and len(payload) >= 7:
        v = (payload[5] << 8) | payload[6]
        if v == 0 and len(payload) >= 9:
            v = (payload[7] << 8) | payload[8]
        dy = v or None
    lod_val = None
    if has_lod and len(payload) >= 10:
        lod_val = payload[9]
    return (dx or None, dy, lod_val, None)


def set_dpi(link, dev: int, new_dpi: int, timeout_ms: int = 600) -> DpiOutcome:
    """带安全网的 DPI 写入：读旧 → 写 → 读回验证 → 失败回滚。"""
    out = DpiOutcome(ok=False)
    if not (50 <= new_dpi <= 32000):
        out.log.append(f"拒绝: DPI {new_dpi} 超出安全范围 (50-32000)")
        return out

    det = _detect(link, dev, timeout_ms)
    if det is None:
        out.log.append("设备不支持 Adjustable DPI (0x2201/0x2202)")
        return out
    name, idx, has_y, _lod = det
    out.feature = name
    write_funct = 3 if name == "0x2201" else 6

    old, _dy, _lod, default = read_dpi(link, dev, timeout_ms)
    out.old = old
    out.log.append(f"读旧值: {old} DPI ({name})" + (f"（默认 {default}）" if default else ""))

    params = bytes([0x00, (new_dpi >> 8) & 0xFF, new_dpi & 0xFF])
    if name == "0x2202" and has_y and old is not None:
        # X/Y 双轴设备：Y 轴跟随 X（Solaar 整体写语义的简化）
        params = params + bytes([(new_dpi >> 8) & 0xFF, new_dpi & 0xFF])
    payload = _call_feature(link, dev, idx, write_funct, params, timeout_ms)
    out.wrote = new_dpi
    out.log.append(f"写: SetDpi({new_dpi}) → {'ACK' if payload is not None else '无应答'}")

    readback, _y2, _l2, _d2 = read_dpi(link, dev, timeout_ms)
    out.readback = readback
    out.log.append(f"读回: {readback} DPI")
    if readback == new_dpi:
        out.ok = True
        out.log.append("验证: 通过")
        return out

    out.log.append(f"验证: 失败（期望 {new_dpi}）——板载配置档开启时主机写入可能不生效")
    if old is not None:
        rollback = bytes([0x00, (old >> 8) & 0xFF, old & 0xFF])
        if name == "0x2202" and has_y:
            rollback += bytes([(old >> 8) & 0xFF, old & 0xFF])
        _call_feature(link, dev, idx, write_funct, rollback, timeout_ms)
        confirm, _y3, _l3, _d3 = read_dpi(link, dev, timeout_ms)
        out.rolled_back = confirm == old
        out.log.append(f"回滚{'' if out.rolled_back else '失败'}: 读回 {confirm}")
    return out

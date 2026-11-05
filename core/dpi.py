"""鼠标 DPI 读写（M2.5b）— HID++ Adjustable DPI 0x2201。

布局移植自 Solaar settings_templates.AdjustableDpi + produce_dpi_list：
- GetSensorDpiList: funct 1，params [sensorIdx, direction, page]；
  应答数据段 = 大端 16 位 DPI 序列直到 0x0000；val>>13==0b111 为步进编码
  [0xE000|step, last]，展开 range(prev+step, last+1, step)
- GetSensorDpi:  funct 2，payload [X, dpiHi, dpiLo, ...] → DPI = BE16@offset1
- SetSensorDpi:  funct 3，params [sensorIdx, dpiHi, dpiLo]

写安全：读旧值 → 写新值 → 读回验证 → 不匹配回滚旧值（plan.md 风险表）。
"""
from __future__ import annotations

from dataclasses import dataclass, field

from core.hidpp import FEATURE_ADJUSTABLE_DPI, _call_feature, get_feature_index


@dataclass
class DpiOutcome:
    ok: bool
    wrote: int | None = None
    old: int | None = None
    readback: int | None = None
    rolled_back: bool = False
    log: list[str] = field(default_factory=list)


def get_dpi_list(link, dev: int, timeout_ms: int = 500) -> list[int]:
    """设备支持的 DPI 档位表（含步进区间展开）。不支持返回 []。"""
    idx = get_feature_index(link, dev, FEATURE_ADJUSTABLE_DPI, timeout_ms)
    if not idx:
        return []
    raw = b""
    for page in range(0x100):
        payload = _call_feature(link, dev, idx, 1, bytes([0x00, 0x00, page]),
                                timeout_ms, long=True)
        if payload is None:
            break
        raw += payload[1:]                     # Solaar ignore=1（首字节非数据）
        if raw[-2:] == b"\x00\x00":
            break
    values: list[int] = []
    i = 0
    while i + 1 < len(raw):
        val = (raw[i] << 8) | raw[i + 1]
        if val == 0:
            break
        if val >> 13 == 0b111:                 # 步进编码: [0xE000|step, last]
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


def read_dpi(link, dev: int, timeout_ms: int = 500) -> tuple[int | None, int | None]:
    """读当前 DPI，返回 (dpi, 默认dpi)。读取失败返回 (None, None)。"""
    idx = get_feature_index(link, dev, FEATURE_ADJUSTABLE_DPI, timeout_ms)
    if not idx:
        return None, None
    payload = _call_feature(link, dev, idx, 2, timeout_ms=timeout_ms)
    if payload is None or len(payload) < 3:
        return None, None
    dpi = (payload[1] << 8) | payload[2]
    default = None
    if len(payload) >= 5:
        d = (payload[3] << 8) | payload[4]
        default = d or None
    if dpi == 0:
        dpi = default                          # Solaar: 0 → 用默认值
    return dpi, default


def set_dpi(link, dev: int, new_dpi: int, timeout_ms: int = 600) -> DpiOutcome:
    """带安全网的 DPI 写入：读旧 → 写 → 读回验证 → 失败回滚。"""
    out = DpiOutcome(ok=False)
    if not (50 <= new_dpi <= 32000):
        out.log.append(f"拒绝: DPI {new_dpi} 超出安全范围 (50-32000)")
        return out

    idx = get_feature_index(link, dev, FEATURE_ADJUSTABLE_DPI, timeout_ms)
    if not idx:
        out.log.append("设备不支持 Adjustable DPI (0x2201)")
        return out

    old, default = read_dpi(link, dev, timeout_ms)
    out.old = old
    out.log.append(f"读旧值: {old} DPI" + (f"（默认 {default}）" if default else ""))

    params = bytes([0x00, (new_dpi >> 8) & 0xFF, new_dpi & 0xFF])
    payload = _call_feature(link, dev, idx, 3, params, timeout_ms)
    out.wrote = new_dpi
    out.log.append(f"写: SetSensorDpi(0, {new_dpi}) → {'ACK' if payload is not None else '无应答'}")

    readback, _ = read_dpi(link, dev, timeout_ms)
    out.readback = readback
    out.log.append(f"读回: {readback} DPI")
    if readback == new_dpi:
        out.ok = True
        out.log.append("验证: 通过")
        return out

    out.log.append(f"验证: 失败（期望 {new_dpi}）")
    if old is not None:
        rollback = bytes([0x00, (old >> 8) & 0xFF, old & 0xFF])
        _call_feature(link, dev, idx, 3, rollback, timeout_ms)
        confirm, _ = read_dpi(link, dev, timeout_ms)
        out.rolled_back = confirm == old
        out.log.append(f"回滚{'' if out.rolled_back else '失败'}: 读回 {confirm}")
    return out

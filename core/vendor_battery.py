"""国产外设电量协议（M5b，移植自 EZ2.4G 项目，来源已标注）。

三套协议，均为"写查询命令 → 听应答"模式（只读查询，不修改设备设置）：

1. ATK（前行者 / attack shark 鼠标，VID 0x373B / 0x3554）
   - ATK2: 64 字节接口，请求 [0x08, 7D 72 02 00 01 07 01, 0 填充]
     应答 rep[1]==0x72 且 rep[5]==0x07，电量 = rep[7]
   - ATK1: 17 字节接口，请求 [0x08, 04, 0x00×14, 校验]
     校验 = (0x55 - sum(report_id + cmd[:15])) & 0xFF
     应答 rep[0]==0x08 且 rep[1]==0x04 且 sum(rep[:16])+rep[16] ≡ 0x55，电量 = rep[6]

2. MCHOSE（迈从 C-Media 方案 dongle，VID 0x3837 / 0x1837 / 0x3654；
   同品牌 291D 设备也值得尝试）
   - 请求 [55 65 01, 0 填充至 64]
   - 应答 rep[0]==0x55 且 rep[1]==0x65，电量 = rep[2]（0-100）

3. 杰理 JL_RCSP（芯片方案，任意 VID 的 64B 厂商接口通用兜底）
   - 帧: [rid] FE seq 10 00 02 00 00 lenLo lenHi payload crcHi crcLo
   - CRC16-CCITT (0xFFFF 初值, 多项式 0x1021)
   - 电量查询 payload = 01 00；应答解析 TLV tag 0x01 首字节 = 电量
"""
from __future__ import annotations

import time
from dataclasses import dataclass

from core.report_io import ReportIO

ATK_VIDS = frozenset({"373B", "3554"})
MCHOSE_VIDS = frozenset({"291D", "3837", "1837", "3654"})


@dataclass
class VendorBattery:
    percent: int
    source: str          # "atk1" / "atk2" / "mchose" / "jl"
    detail: str


def _clamp(v) -> int:
    return max(0, min(100, int(v)))


# ---------------------------------------------------------------------------
# ATK（前行者）
# ---------------------------------------------------------------------------

_ATK2_PAYLOAD = bytes([0x7D, 0x72, 0x02, 0x00, 0x01, 0x07, 0x01])


def _atk1_checksum(report_id: int, cmd: bytes) -> int:
    total = report_id + sum(cmd[:15])
    return (0x55 - (total & 0xFF)) & 0xFF


def _atk1_valid(rep: bytes) -> bool:
    if len(rep) < 17 or rep[0] != 0x08 or rep[1] != 0x04:
        return False
    return (sum(rep[:16]) + rep[16]) & 0xFF == 0x55


def read_atk(io: ReportIO, timeout: float = 0.8, attempts: int = 3) -> VendorBattery | None:
    """ATK 协议电量：先试 ATK2（64B），再试 ATK1（17B）。都失败返回 None。"""
    req2 = bytes([0x08]) + _ATK2_PAYLOAD + bytes(64 - 8)
    for _ in range(attempts):
        try:
            io.write_output(req2)
        except Exception:
            break                                    # 接口不接受 64B 输出 → 跳过
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            rep = io.read_input(250)
            if rep and len(rep) > 7 and rep[1] == 0x72 and rep[5] == 0x07:
                return VendorBattery(_clamp(rep[7]), "atk2",
                                     f"ATK2 应答 {rep[:12].hex(' ')}")
    cmd = bytearray([0x04] + [0x00] * 15)
    cmd[15] = _atk1_checksum(0x08, bytes(cmd))
    req1 = bytes([0x08]) + bytes(cmd)
    for _ in range(attempts):
        try:
            io.write_output(req1)
        except Exception:
            return None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            rep = io.read_input(250)
            if rep and _atk1_valid(rep):
                return VendorBattery(_clamp(rep[6]), "atk1",
                                     f"ATK1 电量字节=0x{rep[6]:02X}")
    return None


# ---------------------------------------------------------------------------
# MCHOSE（迈从）
# ---------------------------------------------------------------------------

def read_mchose(io: ReportIO, timeout: float = 1.0) -> VendorBattery | None:
    req = bytes([0x55, 0x65, 0x01]) + bytes(61)          # 凑 64 字节
    try:
        io.write_output(req)
    except Exception:
        return None
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        rep = io.read_input(250)
        if rep and len(rep) >= 3 and rep[0] == 0x55 and rep[1] == 0x65 and rep[2] <= 100:
            return VendorBattery(_clamp(rep[2]), "mchose",
                                 f"应答 {rep[:8].hex(' ')}")
    return None


# ---------------------------------------------------------------------------
# 杰理 JL_RCSP（通用兜底）
# ---------------------------------------------------------------------------

def _crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def _build_jl_frame(seq: int, payload: bytes, report_id: int, total: int = 64,
                    include_zero: bool = True) -> bytes:
    f = bytearray(total)
    f[0] = report_id
    base = 1
    if include_zero:
        f[base] = 0x00
        base += 1
    f[base:base + 9] = bytes([0xFE, seq & 0xFF, 0x10, 0x00, 0x02, 0x00, 0x00,
                              len(payload) & 0xFF, (len(payload) >> 8) & 0xFF])
    f[base + 9:base + 9 + len(payload)] = payload
    crc = _crc16_ccitt(bytes(f[base:base + 9 + len(payload)]))
    f[base + 9 + len(payload)] = (crc >> 8) & 0xFF
    f[base + 10 + len(payload)] = crc & 0xFF
    return bytes(f)


def _parse_jl(report: bytes):
    """在报文前 4 字节内找 0xFE 帧头，返回 CRC 通过的 payload 或 None。"""
    for start in range(0, 4):
        if start + 12 > len(report) or report[start] != 0xFE:
            continue
        ln = report[start + 7] | (report[start + 8] << 8)
        if ln > 60 or start + 9 + ln + 2 > len(report):
            continue
        payload = report[start + 9:start + 9 + ln]
        crc = (report[start + 9 + ln] << 8) | report[start + 10 + ln]
        if crc == _crc16_ccitt(report[start:start + 9 + ln]):
            return payload
    return None


def read_jl(io: ReportIO, seconds: float = 1.2) -> VendorBattery | None:
    """杰理 TLV 电量：Report ID 0x55（无 0x00 头）/ 0x00（带 0x00 头）两种帧各试。"""
    payload = bytes([0x01, 0x00])
    for rid, include_zero in ((0x55, False), (0x00, True)):
        try:
            io.write_output(_build_jl_frame(0, payload, rid,
                                            include_zero=include_zero))
        except Exception:
            continue
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            rep = io.read_input(200)
            if not rep:
                continue
            parsed = _parse_jl(rep)
            if not parsed:
                continue
            i = 0
            while i + 2 <= len(parsed):
                tag, ln = parsed[i], parsed[i + 1]
                if tag == 0x01 and ln >= 1:
                    return VendorBattery(_clamp(parsed[i + 2]), "jl",
                                         f"杰理 TLV 0x01, ReportID=0x{rid:02X}")
                i += 2 + ln
    return None


# ---------------------------------------------------------------------------
# 编排
# ---------------------------------------------------------------------------

def try_vendor_battery(vid_hex: str, io: ReportIO) -> VendorBattery | None:
    """按 VID 路由厂商协议；杰理作为通用兜底最后试。"""
    if vid_hex in ATK_VIDS:
        result = read_atk(io)
        if result:
            return result
    if vid_hex in MCHOSE_VIDS:
        result = read_mchose(io)
        if result:
            return result
    return read_jl(io)

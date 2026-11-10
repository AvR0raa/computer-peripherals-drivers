"""M5b 单元测试：core/vendor_battery.py（EZ2.4G 移植协议，脚本化应答）。"""
from core.report_io import ReportIO
from core.vendor_battery import (
    VendorBattery,
    _atk1_checksum,
    _atk1_valid,
    _build_jl_frame,
    _parse_jl,
    read_atk,
    read_jl,
    read_mchose,
    try_vendor_battery,
)
from tests.test_report_io import FakeDev


class ReplyDev(FakeDev):
    """按最近一次写请求动态应答的假设备（前缀规则表，不消耗）。"""

    def __init__(self, rules):
        super().__init__()
        self.rules = rules            # list[(prefix_bytes, reply_bytes)]

    def read(self, length, timeout_ms):
        last = self.written[-1] if self.written else None
        if last is not None:
            for prefix, reply in self.rules:
                if last.startswith(prefix):
                    return list(reply[:length])
        return []


# ---------------------------------------------------------------------------
# ATK
# ---------------------------------------------------------------------------

def test_atk1_checksum_matches_ez24g():
    cmd = bytes([0x04] + [0x00] * 15)
    ck = _atk1_checksum(0x08, cmd)
    full = bytes([0x08]) + cmd[:15] + bytes([ck]) if False else bytes([0x08]) + bytes(cmd[:15]) + bytes([ck])
    # 整条 17B 满足 sum+ck ≡ 0x55
    assert (sum(full[:16]) + full[16]) & 0xFF == 0x55


def test_read_atk2_reply():
    dev = ReplyDev([(bytes([0x08, 0x7D]), bytes([0x08, 0x72, 0, 0, 0, 0x07, 0, 66]))])
    io = ReportIO(path=b"x")
    io._dev = dev
    result = read_atk(io, timeout=0.3, attempts=1)
    assert result == VendorBattery(66, "atk2", result.detail)
    assert "ATK2" in result.detail


def test_read_atk1_reply():
    # 构造一条合法 ATK1 应答: rep[0]=08, rep[1]=04, 电量@[6], 校验@[16]
    body = bytearray(17)
    body[0], body[1], body[6] = 0x08, 0x04, 0x55
    body[16] = (0x55 - sum(body[:16])) & 0xFF
    assert _atk1_valid(bytes(body))
    dev = ReplyDev([(bytes([0x08, 0x04]), bytes(body))])
    io = ReportIO(path=b"x")
    io._dev = dev
    result = read_atk(io, timeout=0.3, attempts=1)
    assert result.percent == 85 and result.source == "atk1"      # 电量字节 0x55=85


def test_read_atk_none_when_no_reply():
    io = ReportIO(path=b"x")
    io._dev = FakeDev()
    assert read_atk(io, timeout=0.2, attempts=1) is None


# ---------------------------------------------------------------------------
# MCHOSE
# ---------------------------------------------------------------------------

def test_read_mchose_reply():
    dev = ReplyDev([(bytes([0x55, 0x65]), bytes([0x55, 0x65, 78, 0x01]))])
    io = ReportIO(path=b"x")
    io._dev = dev
    result = read_mchose(io, timeout=0.3)
    assert result.percent == 78 and result.source == "mchose"
    assert dev.written[0][:3] == bytes([0x55, 0x65, 0x01])       # 请求帧正确
    assert len(dev.written[0]) == 64                             # 64 字节填充


def test_read_mchose_invalid_pct_ignored():
    # 应答电量字节 >100 → 视为无效继续等，超时返回 None
    dev = ReplyDev([(bytes([0x55, 0x65]), bytes([0x55, 0x65, 250, 0x00]))])
    io = ReportIO(path=b"x")
    io._dev = dev
    assert read_mchose(io, timeout=0.2) is None


# ---------------------------------------------------------------------------
# 杰理 JL
# ---------------------------------------------------------------------------

def test_jl_roundtrip():
    frame = _build_jl_frame(0, bytes([0x01, 0x00]), report_id=0x55, include_zero=False)
    # 帧布局(无 0x00 头): [0x55] FE seq 10 00 02 00 00 lenLo lenHi
    assert frame[0] == 0x55 and frame[1] == 0xFE and frame[3] == 0x10 and frame[5] == 0x02
    # 应答: 合法 0xFE 帧 + TLV [0x01, 0x01, 64]
    inner = bytes([0x01, 0x01, 64])
    reply = bytearray(_build_jl_frame(0, inner, report_id=0x00, include_zero=True))
    dev = ReplyDev([(bytes([0x55, 0xFE]), bytes(reply))])
    io = ReportIO(path=b"x")
    io._dev = dev
    result = read_jl(io, seconds=0.4)
    assert result.percent == 64 and result.source == "jl"


# ---------------------------------------------------------------------------
# 编排路由
# ---------------------------------------------------------------------------

def test_try_vendor_routes_by_vid():
    io = ReportIO(path=b"x")
    io._dev = ReplyDev([(bytes([0x08, 0x7D]), bytes([0x08, 0x72, 0, 0, 0, 0x07, 0, 80]))])
    result = try_vendor_battery("373B", io)          # ATK VID → ATK2 命中
    assert result.percent == 80


def test_try_vendor_jl_fallback_for_unknown_vid():
    inner = bytes([0x01, 0x01, 40])
    reply = bytes(_build_jl_frame(0, inner, report_id=0x00, include_zero=True))
    io = ReportIO(path=b"x")
    io._dev = ReplyDev([(bytes([0x00, 0x00, 0xFE]), reply)])     # rid=0 + 0x00 头 + FE
    result = try_vendor_battery("ABCD", io)          # 未知 VID → JL 兜底
    assert result.percent == 40 and result.source == "jl"

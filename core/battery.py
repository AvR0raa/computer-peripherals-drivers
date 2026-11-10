"""全设备电量编排（M2.5a）。

三条路径，按优先级：
1. 标准 HID Battery System 页（usage page 0x85）——零写入，纯读
2. HID++ 2.0（罗技：接收器逐槽位 ping / 直连设备 dev=0xFF）
3. 无源 → 标记"需学习模式"

来源：Solaar (HID++ 电量) + USB HID Usage Tables Battery System page 0x85。
"""
from __future__ import annotations

from dataclasses import dataclass

from core.descriptor import ReportField, parse_report_descriptor
from core.hidpp import HidppBattery, HidppLink, ping, read_battery
from core.report_io import ReportIO
from core.vendor_battery import try_vendor_battery

BATTERY_USAGE_PAGE = 0x85   # HID Battery System


@dataclass
class BatteryResult:
    vidpid: str
    product: str = ""
    percent: int | None = None
    voltage_mv: int | None = None
    source: str = "none"        # hid-standard / hidpp-unified / hidpp-status / hidpp-voltage / none
    detail: str = ""

    @property
    def display(self) -> str:
        if self.percent is not None:
            return f"{self.percent}%"
        if self.voltage_mv is not None:
            return f"{self.voltage_mv}mV"
        return "?"


# EZ2.4G 实测常用电量 usage 对（标准 HID 之外的厂商习惯用法）+ Battery System 页
_BATTERY_USAGE_PAIRS = {(0x0006, 0x0020), (0x000D, 0x003B), (0x0085, 0x0065)}


def find_battery_fields(descriptor: bytes) -> list[ReportField]:
    """描述符里可读电量字段：Battery System 页(0x85) + EZ2.4G 实测 usage 对。"""
    info = parse_report_descriptor(descriptor)
    out = []
    for f in info.fields:
        if f.is_constant or f.usage is None:
            continue
        pair = (f.usage >> 16, f.usage & 0xFFFF)
        if (f.usage >> 16) == BATTERY_USAGE_PAGE or pair in _BATTERY_USAGE_PAIRS:
            out.append(f)
    return out


def _scale_percent(field: ReportField, raw: int) -> int | None:
    """按字段逻辑范围缩放到 0-100；无范围或已近似百分比则原样钳制。"""
    lo, hi = field.logical_min, field.logical_max
    if lo is not None and hi is not None and hi > lo and not (lo == 0 and hi == 100):
        scaled = (raw - lo) * 100 / (hi - lo)
        return max(0, min(100, round(scaled)))
    return max(0, min(100, raw)) if 0 <= raw <= 100 else raw


def read_standard_battery(io: ReportIO, fields: list[ReportField]) -> tuple[int | None, str]:
    """读标准 HID 电量字段：feature 字段直接读，input 字段等 200ms 推送。

    返回 (百分比 | 原始值, 说明)。读不到返回 (None, 原因)。
    """
    feature_field = next((f for f in fields if f.report_type == "feature"), None)
    if feature_field is not None:
        data = io.get_feature(feature_field.report_id, 16)
        if data and len(data) > 1:
            offset_byte = feature_field.offset // 8
            body = data[1:] if feature_field.report_id else data
            if offset_byte < len(body):
                raw = body[offset_byte]
                pct = _scale_percent(feature_field, raw)
                detail = (f"标准HID电量 usage=0x{feature_field.usage & 0xFFFF:04X} "
                          f"raw={raw}" + (f" → {pct}%" if pct != raw else ""))
                return pct, detail
        return None, "标准HID电量字段读取失败（可能为写触发型）"

    input_field = next((f for f in fields if f.report_type == "input"), None)
    if input_field is not None:
        data = io.read_input(200)
        if data:
            value = data[input_field.offset // 8] if (input_field.offset // 8) < len(data) else None
            if value is not None:
                pct = _scale_percent(input_field, value)
                return pct, f"标准HID电量页 input 推送 raw={value}"
        return None, "标准HID电量字段存在但 200ms 内无推送"
    return None, "无可读电量字段"


def _hidpp_battery_for(io: ReportIO, dev: int) -> BatteryResult | None:
    """对已打开的 HID++ 接口读电量；设备不在线返回 None。"""
    state, version = ping(io, dev)
    if state == "offline":
        return None
    if state in ("empty-slot",):
        return None
    battery = read_battery(io, dev)
    if battery is None:
        return BatteryResult(vidpid="", product="", source="none",
                             detail=f"HID++ {version} 设备在线但无电量 feature")
    return _from_hidpp(battery)


def _from_hidpp(b: HidppBattery) -> BatteryResult:
    return BatteryResult(
        vidpid="", product="",
        percent=b.percent, voltage_mv=b.voltage_mv,
        source=b.source, detail=b.detail,
    )


def scan_all(
    grouped: dict[str, list],
    read_descriptor,
    hidpp_vids: frozenset[str] = frozenset({"046D"}),
) -> list[BatteryResult]:
    """对全部设备做电量扫描。grouped: {vidpid: [DeviceInfo]}。

    罗技设备走 HID++（接收器逐槽位 + 直连 0xFF）；其余先扫标准电量页。
    """
    results: list[BatteryResult] = []
    for vidpid in sorted(grouped):
        ifaces = grouped[vidpid]
        first = ifaces[0]
        vid_hex = first.vid_hex
        result = BatteryResult(vidpid=vidpid, product=first.product or "(未命名)")

        # 1) 罗技 → HID++ 路线
        if vid_hex in hidpp_vids:
            found = _scan_hidpp(vidpid, ifaces)
            if found is not None:
                found.vidpid = vidpid
                found.product = result.product
                results.append(found)
                continue
            result.detail = "罗技设备：HID++ 未取得电量（无配对设备/无 vendor 接口）"

        # 2) 厂商协议（EZ2.4G 移植: ATK前行者/MCHOSE/杰理）——只读查询命令
        vendor = _scan_vendor(first.vid_hex, ifaces)
        if vendor is not None:
            vendor.vidpid = vidpid
            vendor.product = result.product
            results.append(vendor)
            continue

        # 3) 标准电量页
        standard = _scan_standard(vidpid, ifaces, read_descriptor)
        if standard is not None:
            standard.vidpid = vidpid
            standard.product = result.product
            results.append(standard)
            continue

        # 4) 无源
        if not result.detail:
            result.detail = "未发现电量来源（vendor 协议未逆向，可用学习模式抓官方软件）"
        results.append(result)
    return results


def _scan_vendor(vid_hex: str, ifaces: list) -> BatteryResult | None:
    """对 vendor 自定义接口跑厂商电量协议（写查询命令 + 听应答）。"""
    for iface in ifaces:
        if iface.usage_page < 0xFF00:
            continue
        io = ReportIO(iface.path)
        try:
            with io:
                vb = try_vendor_battery(vid_hex, io)
        except Exception:
            continue
        if vb is not None:
            return BatteryResult(
                vidpid=vidpid, percent=vb.percent, source=f"vendor-{vb.source}",
                detail=f"厂商协议 {vb.source}: {vb.detail}")
    return None


def _has_data(r: BatteryResult | None) -> bool:
    """扫描结果是否带真实电量数据（没数据的不能短路后续扫描）。"""
    return r is not None and (r.percent is not None or r.voltage_mv is not None)


def _scan_hidpp(vidpid: str, ifaces: list) -> BatteryResult | None:
    """罗技：短/长双通道（FF00:0001=短，FF00:0002=长），接收器槽位 ping + 直连 0xFF。

    注意顺序：0xFF（接收器自身/直连设备）无电量时必须继续扫槽位 1-7 ——
    接收器自己应答 HID++ 1.0 但没有电量 feature，不能短路。
    """
    short = next((d for d in ifaces
                  if f"{d.usage_page:04X}" == "FF00" and f"{d.usage:04X}" == "0001"), None)
    long_if = next((d for d in ifaces
                    if f"{d.usage_page:04X}" == "FF00" and f"{d.usage:04X}" == "0002"), None)
    if short is None and long_if is None:
        return None
    link = HidppLink(short.path if short else None, long_if.path if long_if else None)
    with link:
        # 1) 直连设备（0xFF）
        result = _hidpp_battery_for(link, 0xFF)
        if _has_data(result):
            return result
        # 2) 接收器槽位 1-7（长报文优先，新款设备如 PRO X2 只应答长报文）
        fallback: BatteryResult | None = None
        for slot in range(1, 8):
            result = _hidpp_battery_for(link, slot)
            if _has_data(result):
                result.detail = f"槽位 {slot}: {result.detail}"
                return result
            if result is not None and fallback is None:
                fallback = result          # 在线但无电量：记下来兜底
        if fallback is not None:
            fallback.detail = f"槽位设备在线但无电量 feature — {fallback.detail}"
            return fallback
    return None


def _scan_standard(vidpid: str, ifaces: list, read_descriptor) -> BatteryResult | None:
    for iface in ifaces:
        raw = read_descriptor(iface.path)
        if not raw:
            continue
        fields = find_battery_fields(raw)
        if not fields:
            continue
        io = ReportIO(iface.path)
        with io:
            value, detail = read_standard_battery(io, fields)
        if value is not None:
            return BatteryResult(vidpid=vidpid, source="hid-standard",
                                 percent=value if value <= 100 else None,
                                 detail=detail)
        return BatteryResult(vidpid=vidpid, source="none", detail=detail)
    return None

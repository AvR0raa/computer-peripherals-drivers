"""HID 报告描述符解析器（纯逻辑，无 I/O）。

解析 USB HID Report Descriptor 的条目流（main/global/local items），
产出字段列表与集合树，供 CLI 展示"哪个接口是标准功能、哪个是厂商自定义"。

参考规范：USB HID 1.11 §6.2.2 (Report Descriptor)。
"""
from __future__ import annotations

from dataclasses import dataclass, field


class DescriptorError(ValueError):
    """描述符字节流不符合 HID 条目语法。"""


# ---------------------------------------------------------------------------
# 数据模型（公开接口见 ARCHITECTURE.md，新增字段仅允许追加，不得改名/删除）
# ---------------------------------------------------------------------------

@dataclass
class ReportField:
    """一个 Input/Output/Feature 字段（一次主条目声明）。"""

    report_type: str        # "input" | "output" | "feature"
    report_id: int          # 0 = 描述符未使用 Report ID
    offset: int             # 报文体（不含 ID 字节）内 bit 偏移，按 (type, id) 独立累计
    size: int               # 每个 item 的 bit 数（Report Size）
    count: int              # item 重复次数（Report Count）
    usage: int | None       # 第一个 32 位 usage（page<<16|index）；无数值用法
                            # 但有 Usage Min/Max（数组/范围字段）时等于 usage_min
    usages: tuple[int, ...] = ()        # 全部已解析 usage（Usage 列表可能多于一个）
    usage_min: int | None = None        # 数组型字段范围（已按当前 Usage Page 解析）
    usage_max: int | None = None
    flags: int = 0          # 主条目原始 flags（bit0 Data/Const, bit1 Array/Var, bit2 Abs/Rel）
    is_constant: bool = False


@dataclass
class CollectionNode:
    """一个 Collection，构成描述符的嵌套树。"""

    kind: str               # "Application"/"Physical"/... 或未知时的 "0x07"
    usage: int | None       # 32 位 usage（取该集合声明时的第一个 usage）
    children: list = field(default_factory=list)   # CollectionNode | ReportField


@dataclass
class DescriptorInfo:
    usage_pages: set[int]           # 出现过的 Usage Page
    application_usages: list[int]   # 顶层集合 usage 列表（顺序保留）
    collections: list[CollectionNode]
    fields: list[ReportField]       # 全部字段（含常量填充位）
    has_vendor_page: bool           # 是否含厂商自定义 Usage Page（0xFF00+）
    report_bits: dict = field(default_factory=dict)  # (type, id) -> 报文体总 bit 数


# ---------------------------------------------------------------------------
# 名称表（仅用于 summary 显示，缺名回退十六进制）
# ---------------------------------------------------------------------------

COLLECTION_KINDS = {
    0x00: "Physical",
    0x01: "Application",
    0x02: "Logical",
    0x03: "Report",
    0x04: "Named Array",
    0x05: "Usage Switch",
    0x06: "Usage Modifier",
}

USAGE_PAGE_NAMES = {
    0x01: "Generic Desktop",
    0x02: "Simulation",
    0x05: "Game Pad",
    0x06: "Keyboard/Keypad",
    0x07: "LED",
    0x08: "Button",
    0x09: "Tablet",
    0x0C: "Consumer",
}

ROOT_USAGE_NAMES = {
    0x00010001: "Pointer",
    0x00010002: "Mouse",
    0x00010004: "Joystick",
    0x00010005: "Game Pad",
    0x00010006: "Keyboard",
    0x00010080: "System Control",
    0x000C0001: "Consumer Control",
}


def format_usage(usage: int) -> str:
    """32 位 usage → "0x00010002 (Mouse)" 风格的人读字符串。"""
    page, index = usage >> 16, usage & 0xFFFF
    if page >= 0xFF00:
        return f"0x{usage:08X} (Vendor-defined page 0x{page:04X})"
    page_name = USAGE_PAGE_NAMES.get(page, f"page 0x{page:04X}")
    root_name = ROOT_USAGE_NAMES.get(usage)
    if root_name:
        return f"0x{usage:08X} ({root_name})"
    return f"0x{usage:08X} ({page_name})"


# ---------------------------------------------------------------------------
# 条目常量
# ---------------------------------------------------------------------------

# 主条目 tag -> 报文类型
_MAIN_REPORT_TYPES = {0x8: "input", 0x9: "output", 0xB: "feature"}
# 集合 kind 名称
_ITEM_SIZE = (0, 1, 2, 4)   # 前缀 bit0-1 -> 数据字节数
_LONG_ITEM_PREFIX = 0xFE


def _signed(raw: bytes) -> int:
    """按字节宽度做小端有符号解析（HID 全局 min/max 为有符号值）。"""
    value = int.from_bytes(raw, "little")
    bits = len(raw) * 8
    if value >= 1 << (bits - 1):
        value -= 1 << bits
    return value


def _unsigned(raw: bytes) -> int:
    return int.from_bytes(raw, "little")


def _resolve_usage(value: int, page: int) -> int:
    """短 usage（<=0xFFFF）与当前 Usage Page 拼成 32 位；完整 32 位值原样保留。"""
    if value <= 0xFFFF:
        return (page << 16) | value
    return value


# ---------------------------------------------------------------------------
# 解析器
# ---------------------------------------------------------------------------

def parse_report_descriptor(data: bytes) -> DescriptorInfo:
    """解析原始报告描述符字节流。

    支持：短条目（0/1/2/4 字节数据）、长条目（0xFE 前缀，跳过不解析）、
    Report ID、Push/Pop 全局状态栈。
    """
    info = DescriptorInfo(
        usage_pages=set(),
        application_usages=[],
        collections=[],
        fields=[],
        has_vendor_page=False,
        report_bits={},
    )

    # --- 全局条目状态 ---
    g_page = 0
    g_report_size = 0
    g_report_id = 0
    g_report_count = 0
    g_stack: list[tuple] = []

    # --- 局部条目状态（每个主条目后清零） ---
    l_usages: list[int] = []
    l_usage_min: int | None = None
    l_usage_max: int | None = None

    # --- 集合树 ---
    stack: list[CollectionNode] = []

    i, n = 0, len(data)
    while i < n:
        prefix = data[i]
        i += 1

        if prefix == _LONG_ITEM_PREFIX:
            # 长条目：FE <size> <tag> <data...>，仅跳过
            if i + 2 > n:
                raise DescriptorError(f"偏移 {i - 1}: 长条目头被截断")
            size = data[i]
            i += 2 + size
            if i > n:
                raise DescriptorError(f"偏移 {i - 2}: 长条目数据被截断")
            continue

        size = _ITEM_SIZE[prefix & 0x3]
        item_type = (prefix >> 2) & 0x3
        tag = (prefix >> 4) & 0xF

        if i + size > n:
            raise DescriptorError(f"偏移 {i - 1}: 条目数据被截断（需 {size} 字节）")
        payload = data[i:i + size]
        i += size

        if item_type == 0:                      # ---- Main ----
            if tag in _MAIN_REPORT_TYPES:
                report_type = _MAIN_REPORT_TYPES[tag]
                key = (report_type, g_report_id)
                offset = info.report_bits.get(key, 0)
                resolved = tuple(_resolve_usage(u, g_page) for u in l_usages)
                resolved_min = (
                    _resolve_usage(l_usage_min, g_page)
                    if l_usage_min is not None else None
                )
                resolved_max = (
                    _resolve_usage(l_usage_max, g_page)
                    if l_usage_max is not None else None
                )
                field_obj = ReportField(
                    report_type=report_type,
                    report_id=g_report_id,
                    offset=offset,
                    size=g_report_size,
                    count=g_report_count,
                    usage=resolved[0] if resolved else resolved_min,
                    usages=resolved,
                    usage_min=resolved_min,
                    usage_max=resolved_max,
                    flags=_unsigned(payload),
                    is_constant=bool(_unsigned(payload) & 0x1),
                )
                info.fields.append(field_obj)
                if stack:
                    stack[-1].children.append(field_obj)
                info.report_bits[key] = offset + g_report_size * g_report_count
            elif tag == 0xA:                    # Collection
                if not size and payload == b"":
                    value = 0
                else:
                    value = _unsigned(payload)
                kind = COLLECTION_KINDS.get(value, f"0x{value:02X}")
                node = CollectionNode(
                    kind=kind,
                    usage=l_usages[0] if l_usages else None,
                )
                if stack:
                    stack[-1].children.append(node)
                else:
                    info.collections.append(node)
                    if node.usage is not None:
                        info.application_usages.append(node.usage)
                stack.append(node)
            elif tag == 0xC:                    # End Collection
                if not stack:
                    raise DescriptorError(f"偏移 {i - 1}: End Collection 无匹配的 Collection")
                stack.pop()
            # 其余主条目 tag（0x7+ 保留位）忽略

            # 主条目处理完，清空局部状态（HID 规范 §6.2.2.8）
            l_usages.clear()
            l_usage_min = None
            l_usage_max = None

        elif item_type == 1:                    # ---- Global ----
            if tag == 0x0:                      # Usage Page
                g_page = _unsigned(payload)
                info.usage_pages.add(g_page)
                info.has_vendor_page = info.has_vendor_page or g_page >= 0xFF00
            elif tag == 0x7:                    # Report Size
                g_report_size = _unsigned(payload)
            elif tag == 0x8:                    # Report ID
                g_report_id = _unsigned(payload)
            elif tag == 0x9:                    # Report Count
                g_report_count = _unsigned(payload)
            elif tag == 0xA:                    # Push
                g_stack.append((g_page, g_report_size, g_report_id, g_report_count))
            elif tag == 0xB:                    # Pop
                if g_stack:
                    (g_page, g_report_size, g_report_id, g_report_count) = g_stack.pop()
            # 0x1-0x6（Logical/Physical Min/Max、Unit、Exponent）M1 显示层暂不需要，
            # 解析但仅保留在局部变量之外不输出；为简单起见直接跳过。
            # 保留位 tag 0xC-0xF 忽略。

        else:                                   # ---- Local ----
            value = _unsigned(payload)
            if tag == 0x0:                      # Usage
                l_usages.append(_resolve_usage(value, g_page))
            elif tag == 0x1:                    # Usage Minimum
                l_usage_min = value
            elif tag == 0x2:                    # Usage Maximum
                l_usage_max = value
            # 0x3+（Designator/String 实际量等）忽略

    if stack:
        raise DescriptorError("描述符结束仍有未闭合的 Collection")

    return info


def summary(info: DescriptorInfo) -> list[str]:
    """生成人读摘要行（CLI 直接逐行打印）。"""
    lines: list[str] = []
    for usage in info.application_usages:
        lines.append(f"顶层集合: {format_usage(usage)}")
    for (report_type, report_id), bits in sorted(info.report_bits.items()):
        rid = report_id if report_id else "-"
        lines.append(
            f"{report_type} report id={rid}: {bits} bits ({(bits + 7) // 8} bytes)"
        )
    if info.has_vendor_page:
        vendor_pages = sorted(p for p in info.usage_pages if p >= 0xFF00)
        pages_text = ", ".join(f"0x{p:04X}" for p in vendor_pages)
        lines.append(f"⚠ 厂商自定义 Usage Page: {pages_text}（可能含配置功能）")
    return lines

"""任务 1.2 单元测试：core/descriptor.py（纯逻辑，离线运行）。"""
import pytest

from core.descriptor import (
    DescriptorError,
    DescriptorInfo,
    parse_report_descriptor,
    summary,
    format_usage,
)

# 经典 3 键鼠标报告描述符（USB HID 规范附录 B  Example 1 变体，50 字节）
MOUSE_DESC = bytes.fromhex(
    "05 01"                 # Usage Page (Generic Desktop)
    "09 02"                 # Usage (Mouse)
    "A1 01"                 # Collection (Application)
    "09 01"                 #   Usage (Pointer)
    "A1 00"                 #   Collection (Physical)
    "05 09"                 #     Usage Page (Button)
    "19 01"                 #     Usage Minimum (Button 1)
    "29 03"                 #     Usage Maximum (Button 3)
    "15 00"                 #     Logical Minimum (0)
    "25 01"                 #     Logical Maximum (1)
    "95 03"                 #     Report Count (3)
    "75 01"                 #     Report Size (1)
    "81 02"                 #     Input (Data,Var,Abs) - 3 个按键
    "95 01"                 #     Report Count (1)
    "75 05"                 #     Report Size (5)
    "81 01"                 #     Input (Const) - 5 bit 填充
    "05 01"                 #     Usage Page (Generic Desktop)
    "09 30"                 #     Usage (X)
    "09 31"                 #     Usage (Y)
    "15 81"                 #     Logical Minimum (-127)
    "25 7F"                 #     Logical Maximum (127)
    "75 08"                 #     Report Size (8)
    "95 02"                 #     Report Count (2)
    "81 06"                 #     Input (Data,Var,Rel) - X/Y
    "C0"                    #   End Collection
    "C0"                    # End Collection
)

# 游戏键盘风格描述符：键盘 + Consumer + 厂商自定义页，含 Report ID 与 Feature
KBD_VENDOR_DESC = bytes.fromhex(
    "05 01 09 06 A1 01"                                 # Keyboard Application
    "85 01"                                             #   Report ID 1
    "05 07 19 E0 29 E7 15 00 25 01 75 01 95 08 81 02"   #   8 个修饰键
    "95 01 75 08 81 03"                                 #   8 bit 常量填充
    "05 08 19 01 29 05 15 00 25 01 75 01 95 05 91 02"   #   5 个 LED（output）
    "95 01 75 03 91 01"                                 #   3 bit 常量填充
    "C0"
    "05 0C 09 01 A1 01"                                 # Consumer Application
    "85 02"                                             #   Report ID 2
    "0A 23 02 15 00 25 01 75 01 95 01 81 02"            #   1 bit consumer 键
    "95 07 75 01 81 03"                                 #   7 bit 填充
    "C0"
    "06 00 FF 09 01 A1 01"                              # Vendor 0xFF00 Application
    "85 03"                                             #   Report ID 3
    "09 01 15 00 26 FF 00 75 08 95 08 B1 02"            #   8 字节 feature（厂商配置通道）
    "C0"
)


# ---------------------------------------------------------------------------
# 鼠标描述符
# ---------------------------------------------------------------------------

def test_mouse_structure():
    info = parse_report_descriptor(MOUSE_DESC)
    assert info.usage_pages == {0x01, 0x09}
    assert info.application_usages == [0x00010002]     # Mouse
    assert not info.has_vendor_page


def test_mouse_button_field():
    info = parse_report_descriptor(MOUSE_DESC)
    buttons = info.fields[0]
    assert (buttons.report_type, buttons.report_id) == ("input", 0)
    assert (buttons.offset, buttons.size, buttons.count) == (0, 1, 3)
    assert buttons.usage == 0x00090001
    assert (buttons.usage_min, buttons.usage_max) == (0x00090001, 0x00090003)
    assert not buttons.is_constant


def test_mouse_padding_and_xy():
    info = parse_report_descriptor(MOUSE_DESC)
    pad, xy = info.fields[1], info.fields[2]
    assert pad.is_constant and (pad.offset, pad.size, pad.count) == (3, 5, 1)
    assert pad.usage is None
    # X/Y 是一条 count=2 的字段，usage 列表按声明顺序保留
    assert (xy.usage, xy.offset, xy.size, xy.count, xy.flags) == (
        0x00010030, 8, 8, 2, 0x06)  # Data,Var,Rel
    assert xy.usages == (0x00010030, 0x00010031)
    assert info.report_bits[("input", 0)] == 24


# ---------------------------------------------------------------------------
# 键盘 + 厂商自定义页描述符
# ---------------------------------------------------------------------------

def test_kbd_vendor_structure():
    info = parse_report_descriptor(KBD_VENDOR_DESC)
    assert info.application_usages == [0x00010006, 0x000C0001, 0xFF000001]
    assert info.has_vendor_page
    assert 0xFF00 in info.usage_pages


def test_kbd_report_id_isolation():
    """Report ID 隔离：input/output 各自独立计 offset。"""
    info = parse_report_descriptor(KBD_VENDOR_DESC)
    assert info.report_bits == {
        ("input", 1): 16,      # 8 modifier + 8 const
        ("output", 1): 8,      # 5 LED + 3 const
        ("input", 2): 8,       # 1 consumer + 7 const
        ("feature", 3): 64,    # 厂商 8 字节配置通道
    }
    modifiers = info.fields[0]
    assert (modifiers.report_id, modifiers.offset) == (1, 0)
    assert modifiers.usage_min == 0x000700E0   # Keyboard page 修饰键
    assert modifiers.usage_max == 0x000700E7
    leds = info.fields[2]
    assert (leds.report_type, leds.report_id, leds.offset) == ("output", 1, 0)
    assert leds.usage == 0x00080001            # LED page
    led_pad = info.fields[3]                   # output 报文的 3 bit 填充
    assert led_pad.is_constant and (led_pad.offset, led_pad.size) == (5, 3)


def test_vendor_feature_field():
    info = parse_report_descriptor(KBD_VENDOR_DESC)
    feature = info.fields[-1]
    assert (feature.report_type, feature.report_id) == ("feature", 3)
    assert (feature.offset, feature.size, feature.count) == (0, 8, 8)
    assert feature.usage == 0xFF000001


# ---------------------------------------------------------------------------
# 语法边界
# ---------------------------------------------------------------------------

def test_push_pop_restores_global_state():
    desc = bytes.fromhex(
        "05 01 09 02 A1 01"
        "75 08 A4 00"           # Report Size 8, Push
        "75 05 95 01 81 02"     # Size 5 的 input
        "B4 00"                 # Pop -> Size 恢复 8
        "95 01 09 30 81 02"     # Size 8 的 input（X）
        "C0"
    )
    info = parse_report_descriptor(desc)
    f1, f2 = info.fields
    assert (f1.offset, f1.size) == (0, 5)
    # bit 偏移按位连续累计（不按字节对齐），Pop 后 Size 恢复 8
    assert (f2.offset, f2.size) == (5, 8)
    assert info.report_bits[("input", 0)] == 13


def test_long_item_is_skipped():
    desc = bytes.fromhex(
        "05 01 09 02 A1 01"
        "FE 03 9F 11 22 33"     # 长条目：FE <size=3> <tag=9F> <3 字节数据>
        "75 08 95 01 09 30 81 02"
        "C0"
    )
    info = parse_report_descriptor(desc)
    assert len(info.fields) == 1
    assert info.fields[0].usage == 0x00010030


def test_truncated_item_raises():
    with pytest.raises(DescriptorError):
        parse_report_descriptor(b"\x05")            # 有前缀无数据


def test_truncated_long_item_raises():
    with pytest.raises(DescriptorError):
        parse_report_descriptor(b"\xFE\x05\x9F\x01")  # 声称 5 字节只有 1 字节


def test_unclosed_collection_raises():
    with pytest.raises(DescriptorError):
        parse_report_descriptor(bytes.fromhex("05 01 09 02 A1 01 81 02"))


def test_stray_end_collection_raises():
    with pytest.raises(DescriptorError):
        parse_report_descriptor(bytes.fromhex("05 01 C0"))


def test_empty_descriptor_is_valid():
    info = parse_report_descriptor(b"")
    assert isinstance(info, DescriptorInfo)
    assert info.fields == [] and info.collections == []


# ---------------------------------------------------------------------------
# summary / format_usage
# ---------------------------------------------------------------------------

def test_summary_lines():
    lines = summary(parse_report_descriptor(KBD_VENDOR_DESC))
    joined = "\n".join(lines)
    assert "Keyboard" in joined
    assert "Consumer Control" in joined
    assert "Vendor-defined" in joined
    assert "⚠" in joined


def test_format_usage():
    assert format_usage(0x00010002) == "0x00010002 (Mouse)"
    assert format_usage(0x000C00B8) == "0x000C00B8 (Consumer)"
    assert "Vendor-defined" in format_usage(0xFF000001)

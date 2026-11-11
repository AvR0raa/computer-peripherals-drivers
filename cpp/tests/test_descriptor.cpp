// test_descriptor.cpp — 任务 1.2 单元测试（tests/test_descriptor.py 的平移，纯逻辑离线运行）。
#include <cstdint>
#include <cstdlib>
#include <string>

#include "doctest.h"
#include "core/descriptor.hpp"

using namespace pt;
using Bytes = std::vector<std::uint8_t>;

// "05 01 09 02 ..." → 字节流
static Bytes bhex(const char* s) {
    Bytes out;
    for (const char* p = s; *p;) {
        if (*p == ' ') { ++p; continue; }
        char tmp[3] = {p[0], p[1], '\0'};
        out.push_back(static_cast<std::uint8_t>(std::strtoul(tmp, nullptr, 16)));
        p += 2;
    }
    return out;
}

// 经典 3 键鼠标报告描述符（USB HID 规范附录 B Example 1 变体，50 字节）
static const Bytes MOUSE_DESC = bhex(
    "05 01"                 // Usage Page (Generic Desktop)
    "09 02"                 // Usage (Mouse)
    "A1 01"                 // Collection (Application)
    "09 01"                 //   Usage (Pointer)
    "A1 00"                 //   Collection (Physical)
    "05 09"                 //     Usage Page (Button)
    "19 01"                 //     Usage Minimum (Button 1)
    "29 03"                 //     Usage Maximum (Button 3)
    "15 00"                 //     Logical Minimum (0)
    "25 01"                 //     Logical Maximum (1)
    "95 03"                 //     Report Count (3)
    "75 01"                 //     Report Size (1)
    "81 02"                 //     Input (Data,Var,Abs) - 3 个按键
    "95 01"                 //     Report Count (1)
    "75 05"                 //     Report Size (5)
    "81 01"                 //     Input (Const) - 5 bit 填充
    "05 01"                 //     Usage Page (Generic Desktop)
    "09 30"                 //     Usage (X)
    "09 31"                 //     Usage (Y)
    "15 81"                 //     Logical Minimum (-127)
    "25 7F"                 //     Logical Maximum (127)
    "75 08"                 //     Report Size (8)
    "95 02"                 //     Report Count (2)
    "81 06"                 //     Input (Data,Var,Rel) - X/Y
    "C0"                    //   End Collection
    "C0"                    // End Collection
);

// 游戏键盘风格描述符：键盘 + Consumer + 厂商自定义页，含 Report ID 与 Feature
static const Bytes KBD_VENDOR_DESC = bhex(
    "05 01 09 06 A1 01"                                 // Keyboard Application
    "85 01"                                             //   Report ID 1
    "05 07 19 E0 29 E7 15 00 25 01 75 01 95 08 81 02"   //   8 个修饰键
    "95 01 75 08 81 03"                                 //   8 bit 常量填充
    "05 08 19 01 29 05 15 00 25 01 75 01 95 05 91 02"   //   5 个 LED（output）
    "95 01 75 03 91 01"                                 //   3 bit 常量填充
    "C0"
    "05 0C 09 01 A1 01"                                 // Consumer Application
    "85 02"                                             //   Report ID 2
    "0A 23 02 15 00 25 01 75 01 95 01 81 02"            //   1 bit consumer 键
    "95 07 75 01 81 03"                                 //   7 bit 填充
    "C0"
    "06 00 FF 09 01 A1 01"                              // Vendor 0xFF00 Application
    "85 03"                                             //   Report ID 3
    "09 01 15 00 26 FF 00 75 08 95 08 B1 02"            //   8 字节 feature（厂商配置通道）
    "C0"
);

// ---------------------------------------------------------------------------
// 鼠标描述符
// ---------------------------------------------------------------------------

TEST_CASE("鼠标描述符结构") {
    const auto info = parse_report_descriptor(MOUSE_DESC);
    CHECK(info.usage_pages == std::set<std::uint32_t>{0x01, 0x09});
    REQUIRE(info.application_usages.size() == 1);
    CHECK(info.application_usages[0] == 0x00010002);  // Mouse
    CHECK_FALSE(info.has_vendor_page);
}

TEST_CASE("鼠标按键字段") {
    const auto info = parse_report_descriptor(MOUSE_DESC);
    const auto& buttons = info.fields[0];
    CHECK(buttons.report_type == "input");
    CHECK(buttons.report_id == 0);
    CHECK(buttons.offset == 0);
    CHECK(buttons.size == 1);
    CHECK(buttons.count == 3);
    REQUIRE(buttons.usage.has_value());
    CHECK(*buttons.usage == 0x00090001);
    REQUIRE(buttons.usage_min.has_value());
    REQUIRE(buttons.usage_max.has_value());
    CHECK(*buttons.usage_min == 0x00090001);
    CHECK(*buttons.usage_max == 0x00090003);
    CHECK_FALSE(buttons.is_constant);
}

TEST_CASE("鼠标填充位与 X/Y") {
    const auto info = parse_report_descriptor(MOUSE_DESC);
    const auto& pad = info.fields[1];
    const auto& xy = info.fields[2];
    CHECK(pad.is_constant);
    CHECK(pad.offset == 3);
    CHECK(pad.size == 5);
    CHECK(pad.count == 1);
    CHECK_FALSE(pad.usage.has_value());
    // X/Y 是一条 count=2 的字段，usage 列表按声明顺序保留
    CHECK(*xy.usage == 0x00010030);
    CHECK(xy.offset == 8);
    CHECK(xy.size == 8);
    CHECK(xy.count == 2);
    CHECK(xy.flags == 0x06);  // Data,Var,Rel
    REQUIRE(xy.usages.size() == 2);
    CHECK(xy.usages[0] == 0x00010030);
    CHECK(xy.usages[1] == 0x00010031);
    CHECK(info.report_bits.at({"input", 0}) == 24);
}

// ---------------------------------------------------------------------------
// 键盘 + 厂商自定义页描述符
// ---------------------------------------------------------------------------

TEST_CASE("键盘厂商描述符结构") {
    const auto info = parse_report_descriptor(KBD_VENDOR_DESC);
    REQUIRE(info.application_usages.size() == 3);
    CHECK(info.application_usages[0] == 0x00010006);
    CHECK(info.application_usages[1] == 0x000C0001);
    CHECK(info.application_usages[2] == 0xFF000001);
    CHECK(info.has_vendor_page);
    CHECK(info.usage_pages.count(0xFF00) == 1);
}

TEST_CASE("Report ID 隔离") {
    const auto info = parse_report_descriptor(KBD_VENDOR_DESC);
    CHECK(info.report_bits.at({"input", 1}) == 16);    // 8 modifier + 8 const
    CHECK(info.report_bits.at({"output", 1}) == 8);    // 5 LED + 3 const
    CHECK(info.report_bits.at({"input", 2}) == 8);     // 1 consumer + 7 const
    CHECK(info.report_bits.at({"feature", 3}) == 64);  // 厂商 8 字节配置通道

    const auto& modifiers = info.fields[0];
    CHECK(modifiers.report_id == 1);
    CHECK(modifiers.offset == 0);
    REQUIRE(modifiers.usage_min.has_value());
    CHECK(*modifiers.usage_min == 0x000700E0);         // Keyboard page 修饰键
    REQUIRE(modifiers.usage_max.has_value());
    CHECK(*modifiers.usage_max == 0x000700E7);

    const auto& leds = info.fields[2];
    CHECK(leds.report_type == "output");
    CHECK(leds.report_id == 1);
    CHECK(leds.offset == 0);
    REQUIRE(leds.usage.has_value());
    CHECK(*leds.usage == 0x00080001);                  // LED page

    const auto& led_pad = info.fields[3];              // output 报文的 3 bit 填充
    CHECK(led_pad.is_constant);
    CHECK(led_pad.offset == 5);
    CHECK(led_pad.size == 3);
}

TEST_CASE("厂商 feature 字段") {
    const auto info = parse_report_descriptor(KBD_VENDOR_DESC);
    const auto& feature = info.fields.back();
    CHECK(feature.report_type == "feature");
    CHECK(feature.report_id == 3);
    CHECK(feature.offset == 0);
    CHECK(feature.size == 8);
    CHECK(feature.count == 8);
    REQUIRE(feature.usage.has_value());
    CHECK(*feature.usage == 0xFF000001);
}

// ---------------------------------------------------------------------------
// 语法边界
// ---------------------------------------------------------------------------

TEST_CASE("Push/Pop 恢复全局状态") {
    const Bytes desc = bhex(
        "05 01 09 02 A1 01"
        "75 08 A4 00"           // Report Size 8, Push
        "75 05 95 01 81 02"     // Size 5 的 input
        "B4 00"                 // Pop -> Size 恢复 8
        "95 01 09 30 81 02"     // Size 8 的 input（X）
        "C0"
    );
    const auto info = parse_report_descriptor(desc);
    REQUIRE(info.fields.size() == 2);
    CHECK(info.fields[0].offset == 0);
    CHECK(info.fields[0].size == 5);
    // bit 偏移按位连续累计（不按字节对齐），Pop 后 Size 恢复 8
    CHECK(info.fields[1].offset == 5);
    CHECK(info.fields[1].size == 8);
    CHECK(info.report_bits.at({"input", 0}) == 13);
}

TEST_CASE("长条目被跳过") {
    const Bytes desc = bhex(
        "05 01 09 02 A1 01"
        "FE 03 9F 11 22 33"     // 长条目：FE <size=3> <tag=9F> <3 字节数据>
        "75 08 95 01 09 30 81 02"
        "C0"
    );
    const auto info = parse_report_descriptor(desc);
    REQUIRE(info.fields.size() == 1);
    REQUIRE(info.fields[0].usage.has_value());
    CHECK(*info.fields[0].usage == 0x00010030);
}

TEST_CASE("截断条目抛异常") {
    CHECK_THROWS_AS(parse_report_descriptor(Bytes{0x05}), const DescriptorError&);
}

TEST_CASE("截断长条目抛异常") {
    CHECK_THROWS_AS(parse_report_descriptor(Bytes{0xFE, 0x05, 0x9F, 0x01}), const DescriptorError&);
}

TEST_CASE("未闭合 Collection 抛异常") {
    CHECK_THROWS_AS(parse_report_descriptor(bhex("05 01 09 02 A1 01 81 02")), const DescriptorError&);
}

TEST_CASE("孤立 End Collection 抛异常") {
    CHECK_THROWS_AS(parse_report_descriptor(bhex("05 01 C0")), const DescriptorError&);
}

TEST_CASE("空描述符合法") {
    const auto info = parse_report_descriptor(Bytes{});
    CHECK(info.fields.empty());
    CHECK(info.collections.empty());
}

// ---------------------------------------------------------------------------
// summary / format_usage
// ---------------------------------------------------------------------------

TEST_CASE("摘要行") {
    const auto lines = summary(parse_report_descriptor(KBD_VENDOR_DESC));
    std::string joined;
    for (const auto& l : lines) joined += l + "\n";
    CHECK(joined.find("Keyboard") != std::string::npos);
    CHECK(joined.find("Consumer Control") != std::string::npos);
    CHECK(joined.find("Vendor-defined") != std::string::npos);
    CHECK(joined.find("\xe2\x9a\xa0") != std::string::npos);  // ⚠ (UTF-8)
}

TEST_CASE("format_usage") {
    CHECK(format_usage(0x00010002) == "0x00010002 (Mouse)");
    CHECK(format_usage(0x000C00B8) == "0x000C00B8 (Consumer)");
    CHECK(format_usage(0xFF000001).find("Vendor-defined") != std::string::npos);
}

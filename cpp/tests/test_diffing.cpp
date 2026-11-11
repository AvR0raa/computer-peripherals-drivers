// test_diffing.cpp — 任务 4.2 单元测试（tests/test_diffing.py 平移，纯函数离线）。
#include "doctest.h"
#include "core/diffing.hpp"

using namespace pt;

static CaptureEntry entry(double ts = 1.0, const std::string& source = "FF21",
                          const std::string& kind = "input", int report_id = 237,
                          const std::string& data = "01 02 03 04") {
    return CaptureEntry{ts, source, kind, report_id, data};
}

static bool has_change(const GroupDiff& d, int offset, std::optional<int> before,
                       std::optional<int> after) {
    for (const auto& c : d.changes)
        if (c.offset == offset && c.before == before && c.after == after) return true;
    return false;
}

// ---------------------------------------------------------------------------
// 核心差分
// ---------------------------------------------------------------------------

TEST_CASE("定位变化字节（验收场景：改DPI前后）") {
    const auto diffs = diff_sessions({entry(1.0, "FF21", "input", 237, "0e ed 20 00 00")},
                                     {entry(2.0, "FF21", "input", 237, "0e ed 40 00 00")});
    REQUIRE(diffs.size() == 1);
    const auto& d = diffs[0];
    CHECK_FALSE(d.is_new);
    CHECK_FALSE(d.is_volatile);
    CHECK(d.changes.size() == 1);
    CHECK(has_change(d, 2, 0x20, 0x40));
}

TEST_CASE("同组多处变化") {
    auto diffs = diff_sessions({entry(1.0, "FF21", "input", 237, "aa 11 22 33")},
                               {entry(2.0, "FF21", "input", 237, "aa 11 99 33")});
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].changes.size() == 1);
    CHECK(diffs[0].changes[0].offset == 2);
    // 两个字段一起变
    diffs = diff_sessions({entry(1.0, "FF21", "input", 237, "aa 11 22 33")},
                          {entry(2.0, "FF21", "input", 237, "bb 11 99 33")});
    REQUIRE(diffs[0].changes.size() == 2);
    CHECK(diffs[0].changes[0].offset == 0);
    CHECK(diffs[0].changes[0].before == 0xAA);
    CHECK(diffs[0].changes[0].after == 0xBB);
    CHECK(diffs[0].changes[1].offset == 2);
    CHECK(diffs[0].changes[1].before == 0x22);
    CHECK(diffs[0].changes[1].after == 0x99);
}

TEST_CASE("未变化组跳过") {
    const std::vector<CaptureEntry> before = {entry(1.0, "FF21", "input", 237, "01"),
                                              entry(2.0, "rawhid", "input", 0, "02")};
    const std::vector<CaptureEntry> after = {entry(1.0, "FF21", "input", 237, "01"),
                                             entry(2.0, "rawhid", "input", 0, "02")};
    CHECK(diff_sessions(before, after).empty());
}

TEST_CASE("after 新组") {
    const auto diffs = diff_sessions({}, {entry(1.0, "FF21", "feature_read", 170, "aa bb")});
    REQUIRE(diffs.size() == 1);
    const auto& d = diffs[0];
    CHECK(d.is_new);
    CHECK(d.before_len == 0);
    CHECK(d.after_len == 2);
    REQUIRE(d.changes.size() == 2);
    CHECK(d.changes[0].offset == 0);
    CHECK_FALSE(d.changes[0].before.has_value());
    CHECK(d.changes[0].after == 0xAA);
    CHECK(d.changes[1].offset == 1);
    CHECK(d.changes[1].after == 0xBB);
}

TEST_CASE("长度变化报告") {
    const auto diffs = diff_sessions({entry(1.0, "FF21", "input", 237, "01 02 03")},
                                     {entry(2.0, "FF21", "input", 237, "01 02 03 04")});
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].changes.size() == 1);
    CHECK(diffs[0].changes[0].offset == 3);
    CHECK_FALSE(diffs[0].changes[0].before.has_value());
    CHECK(diffs[0].changes[0].after == 0x04);
}

TEST_CASE("同组取最后一条（feature_read 手动快照语义）") {
    const auto diffs = diff_sessions(
        {entry(1.0, "FF21", "feature_read", 237, "01 02"), entry(2.0, "FF21", "feature_read", 237, "01 09")},
        {entry(3.0, "FF21", "feature_read", 237, "01 02"), entry(4.0, "FF21", "feature_read", 237, "01 05")});
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].changes.size() == 1);
    CHECK(diffs[0].changes[0].offset == 1);
    CHECK(diffs[0].changes[0].before == 0x09);
    CHECK(diffs[0].changes[0].after == 0x05);
}

TEST_CASE("error 条目忽略") {
    const auto diffs = diff_sessions({entry(1.0, "FF21", "input", 237, "01"),
                                      entry(2.0, "FF21", "error", 0, "boom")},
                                     {entry(3.0, "FF21", "input", 237, "02")});
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].changes.size() == 1);
    CHECK(diffs[0].changes[0].before == 0x01);
    CHECK(diffs[0].changes[0].after == 0x02);
}

TEST_CASE("空数据按空字节串处理") {
    const auto diffs = diff_sessions({entry(1.0, "FF21", "input", 237, "")},
                                     {entry(2.0, "FF21", "input", 237, "01")});
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].changes.size() == 1);
    CHECK_FALSE(diffs[0].changes[0].before.has_value());
    CHECK(diffs[0].changes[0].after == 0x01);
}

// ---------------------------------------------------------------------------
// 噪声识别
// ---------------------------------------------------------------------------

TEST_CASE("volatile 组识别") {
    std::vector<CaptureEntry> stream;
    for (int i = 0; i < 10; ++i)
        stream.push_back(entry(static_cast<double>(i), "FF21", "input", 237,
                               (i < 10 ? "0" : "") + std::to_string(i)));  // 10 种内容
    const auto groups = volatile_groups(stream);
    REQUIRE(groups.size() == 1);
    CHECK(*groups.begin() == GroupKey("FF21", "input", 237));
}

TEST_CASE("稳定组不算 volatile") {
    const std::vector<CaptureEntry> stable = {entry(1.0, "FF21", "input", 237, "aa"),
                                              entry(2.0, "FF21", "input", 237, "aa")};
    CHECK(volatile_groups(stable).empty());
}

TEST_CASE("volatile 组仍给 diff 但带标记") {
    std::vector<CaptureEntry> noise;
    for (int i = 0; i < 10; ++i)
        noise.push_back(entry(static_cast<double>(i), "FF21", "input", 237,
                              (i < 10 ? "0" : "") + std::to_string(i)));
    auto after = noise;
    after.push_back(entry(99.0, "FF21", "input", 237, "ff"));
    const auto diffs = diff_sessions(noise, after);
    REQUIRE(diffs.size() == 1);
    CHECK(diffs[0].is_volatile);  // 内容 >= 8 种，仍给出 diff 但带标记
    const auto lines = format_diff(diffs[0]);
    CHECK(lines[0].find("疑似噪声流") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 人读格式
// ---------------------------------------------------------------------------

TEST_CASE("format_diff 常规") {
    const auto diffs = diff_sessions({entry(1.0, "FF21", "input", 237, "0e ed 20")},
                                     {entry(2.0, "FF21", "input", 237, "0e ed 40")});
    const auto lines = format_diff(diffs[0]);
    CHECK(lines[0].rfind("FF21 input id=237", 0) == 0);
    CHECK(lines[0].find("1 字节变化") != std::string::npos);
    CHECK(lines[1].find("[  2] 0x20 → 0x40") != std::string::npos);
}

TEST_CASE("format_diff 新组") {
    const auto diffs = diff_sessions({}, {entry(1.0, "FF21", "feature_read", 170, "aa bb")});
    const auto lines = format_diff(diffs[0]);
    CHECK(lines[0].find("新报文组") != std::string::npos);
    CHECK(lines[0].find("2 字节") != std::string::npos);
    CHECK(lines[1].find("AA BB") != std::string::npos);  // hex 大写展示
}

TEST_CASE("format_diff 超 16 变化截断") {
    std::string big_before, big_after;
    for (int i = 0; i < 20; ++i) {
        if (i) { big_before += " "; big_after += " "; }
        big_before += "00";
        big_after += "ff";
    }
    const auto diffs = diff_sessions({entry(1.0, "FF21", "input", 237, big_before)},
                                     {entry(2.0, "FF21", "input", 237, big_after)});
    const auto lines = format_diff(diffs[0]);
    CHECK(lines.size() == 18);  // 头 + 16 变化 + 省略行
    CHECK(lines.back().find("还有 4 个变化字节") != std::string::npos);
}

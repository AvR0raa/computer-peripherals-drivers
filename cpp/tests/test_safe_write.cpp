// test_safe_write.cpp — 任务 2.4 单元测试（tests/test_safe_write.py 平移，脚本化收发离线）。
#include "doctest.h"
#include "core/safe_write.hpp"
#include "test_report_io.hpp"

using namespace pt;
using pt_test::b;
using pt_test::Bytes;
using pt_test::install_scripted;
using pt_test::Json;
using pt_test::Params;
using pt_test::parse;
using pt_test::ReportIO;
using pt_test::ScriptedDev;

// 安装 ScriptedDev；返回裸指针供断言（open 后所有权归 ReportIO，close 前访问）
static ScriptedDev* install(ReportIO& io, std::vector<Bytes> responses = {},
                            std::vector<Bytes> feature_reads = {}) {
    return install_scripted(io, std::move(responses), std::move(feature_reads));
}

static bool log_contains(const WriteOutcome& out, const std::string& fragment) {
    for (const auto& line : out.log)
        if (line.find(fragment) != std::string::npos) return true;
    return false;
}

// ---- schema 片段 ----

static const Json FEATURE_WRITE = parse(R"({
    "id": "set_dpi", "kind": "write", "report": "feature",
    "template": [7, "{reg}", "{value}", 0, 0],
    "params": {
        "reg": {"type": "int", "default": 16, "min": 0, "max": 255},
        "value": {"type": "int", "default": 0, "min": 0, "max": 255}
    }
})");
static const Bytes READ_OLD{0x07, 0x10, 0x20, 0x00, 0x00};
static const Bytes READ_NEW_OK{0x07, 0x10, 0x40, 0x00, 0x00};

static const Json OUTPUT_WRITE = parse(R"({
    "id": "set_dpi", "kind": "write", "report": "output",
    "template": [16, "{device_index}", 0, 33, "{value}", 0, 66],
    "params": {
        "device_index": {"type": "int", "default": 255, "min": 0, "max": 255},
        "value": {"type": "int", "default": 0, "min": 0, "max": 255}
    }
})");
static const Json OUTPUT_READ = parse(R"({
    "id": "read_dpi", "kind": "read", "report": "output",
    "template": [16, "{device_index}", 0, 32, 0, 0, 66],
    "params": {"device_index": {"type": "int", "default": 255, "min": 0, "max": 255}},
    "response": {
        "check": [16, "{device_index}", 0, 32, null, null, 66],
        "extract": {"value": 4}
    }
})");
static const Bytes OLD_RSP{16, 255, 0, 32, 0x20, 0, 66};
static const Bytes NEW_RSP{16, 255, 0, 32, 0x40, 0, 66};

// ---- 拒绝条件 ----

TEST_CASE("拒绝非 write 功能") {
    ReportIO io("fake");
    ScriptedDev* dev = install(io);
    const Json feature = parse(
        R"({"id": "x", "kind": "command", "report": "feature", "template": [1]})");
    const Params empty{};
    const auto result = safe_write(io, feature, nullptr, &empty);
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.wrote.has_value());
    REQUIRE_FALSE(result.log.empty());
    CHECK(result.log[0].find("不是 write") != std::string::npos);
    CHECK(dev->features_sent.empty());
}

TEST_CASE("拒绝 output 写入缺 read 配对") {
    ReportIO io("fake");
    install(io);
    const Params new_params{{"value", 1}};
    const auto result = safe_write(io, OUTPUT_WRITE, nullptr, &new_params);
    CHECK_FALSE(result.ok);
    REQUIRE_FALSE(result.log.empty());
    CHECK(result.log[0].find("必须配对 read") != std::string::npos);
}

// ---- feature report 路径 ----

TEST_CASE("feature 写入验证通过") {
    ReportIO io("fake");
    ScriptedDev* dev = install(io, {}, {READ_OLD, READ_NEW_OK});
    const Params new_params{{"value", 0x40}};
    const ByteExpect expect{{2, 0x40}};
    const auto result = safe_write(io, FEATURE_WRITE, nullptr, &new_params, &expect);
    CHECK(result.ok);
    CHECK(result.verified);
    CHECK_FALSE(result.rolled_back);
    REQUIRE(result.old_value.has_value());
    REQUIRE(result.readback.has_value());
    CHECK(*result.old_value == READ_OLD);
    CHECK(*result.readback == READ_NEW_OK);
    REQUIRE(dev->features_sent.size() == 1);  // 只写一次
    CHECK(dev->features_sent[0] == Bytes{0x07, 0x10, 0x40, 0x00, 0x00});
    CHECK(log_contains(result, "读旧值"));
    CHECK(log_contains(result, "验证: 通过"));
}

TEST_CASE("feature 写入失败后回滚") {
    // 读回仍是旧值 → 验证失败 → 回滚旧值 → 读回确认
    ReportIO io("fake");
    ScriptedDev* dev = install(io, {}, {READ_OLD, READ_OLD, READ_OLD});
    const Params new_params{{"value", 0x40}};
    const ByteExpect expect{{2, 0x40}};
    const auto result = safe_write(io, FEATURE_WRITE, nullptr, &new_params, &expect);
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.verified);
    CHECK(result.rolled_back);
    REQUIRE(dev->features_sent.size() == 2);
    CHECK(dev->features_sent[0] == Bytes{0x07, 0x10, 0x40, 0x00, 0x00});
    CHECK(dev->features_sent[1] == READ_OLD);
    CHECK(log_contains(result, "回滚成功"));
}

TEST_CASE("feature 写入无 expect = 已写入未验证但 ok") {
    ReportIO io("fake");
    ScriptedDev* dev = install(io, {}, {READ_OLD, READ_NEW_OK});
    const Params new_params{{"value", 0x40}};
    const auto result = safe_write(io, FEATURE_WRITE, nullptr, &new_params);
    CHECK(result.ok);
    CHECK_FALSE(result.verified);  // 已写入未验证
    CHECK(log_contains(result, "无法自动验证"));
    REQUIRE(dev->features_sent.size() == 1);  // 无 expect 不触发回滚
    CHECK(dev->features_sent[0] == Bytes{0x07, 0x10, 0x40, 0x00, 0x00});
}

TEST_CASE("feature 模板渲染错误中止") {
    ReportIO io("fake");
    ScriptedDev* dev = install(io);
    const Params new_params{{"value", 999}};  // 越界
    const ByteExpect expect{{2, 1}};
    const auto result = safe_write(io, FEATURE_WRITE, nullptr, &new_params, &expect);
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.wrote.has_value());
    CHECK(dev->features_sent.empty());
}

// ---- output + read 功能配对路径 ----

TEST_CASE("output 写入验证通过") {
    ReportIO io("fake");
    ScriptedDev* dev = install(io, {OLD_RSP, NEW_RSP});
    const Params new_params{{"value", 0x40}};
    const ByteExpect expect{{4, 0x40}};
    const auto result = safe_write(io, OUTPUT_WRITE, &OUTPUT_READ, &new_params, &expect);
    CHECK(result.ok);
    CHECK(result.verified);
    CHECK_FALSE(result.rolled_back);
    REQUIRE(result.old_value.has_value());
    REQUIRE(result.readback.has_value());
    CHECK(*result.old_value == OLD_RSP);
    CHECK(*result.readback == NEW_RSP);
    // 写入请求出现在两次读请求之间
    std::vector<Bytes> writes;
    for (const auto& w : dev->written)
        if (w.size() > 3 && w[3] == 33) writes.push_back(w);
    REQUIRE(writes.size() == 1);
    CHECK(writes[0] == Bytes{16, 255, 0, 33, 0x40, 0, 66});
}

TEST_CASE("output 失败回滚（带 rollback_params）") {
    ReportIO io("fake");
    ScriptedDev* dev = install(io, {OLD_RSP, OLD_RSP, OLD_RSP});
    const Params new_params{{"value", 0x40}};
    const ByteExpect expect{{4, 0x40}};
    const Params rollback{{"value", 0x20}};
    const auto result = safe_write(io, OUTPUT_WRITE, &OUTPUT_READ, &new_params, &expect, &rollback);
    CHECK_FALSE(result.verified);
    CHECK(result.rolled_back);
    int rollback_writes = 0;
    for (const auto& w : dev->written)
        if (w.size() > 4 && w[3] == 33 && w[4] == 0x20) ++rollback_writes;
    CHECK(rollback_writes == 1);
    CHECK(log_contains(result, "回滚成功"));
}

TEST_CASE("output 失败且无 rollback_params 不回滚") {
    ReportIO io("fake");
    install(io, {OLD_RSP, OLD_RSP});
    const Params new_params{{"value", 0x40}};
    const ByteExpect expect{{4, 0x40}};
    const auto result = safe_write(io, OUTPUT_WRITE, &OUTPUT_READ, &new_params, &expect);
    CHECK_FALSE(result.verified);
    CHECK_FALSE(result.rolled_back);
    CHECK(log_contains(result, "未提供 rollback_params"));
}

TEST_CASE("output 无旧值无法回滚") {
    // 第一次读就无响应 → 无旧值 → 不回滚
    ReportIO io("fake");
    install(io, {Bytes{}, Bytes{}, Bytes{}});
    const Params new_params{{"value", 0x40}};
    const ByteExpect expect{{4, 0x40}};
    const auto result = safe_write(io, OUTPUT_WRITE, &OUTPUT_READ, &new_params, &expect);
    CHECK_FALSE(result.old_value.has_value());
    CHECK_FALSE(result.rolled_back);
    CHECK(log_contains(result, "无旧值可回滚"));
}

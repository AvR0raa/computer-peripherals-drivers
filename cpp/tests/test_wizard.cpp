// test_wizard.cpp — 任务 4.3 单元测试（tests/test_wizard.py 平移）。
#include "doctest.h"
#include "core/fingerprint.hpp"
#include "core/wizard.hpp"

using namespace pt;

static CaptureEntry entry(double ts = 1.0, const std::string& source = "FF21",
                          const std::string& kind = "input", int report_id = 237,
                          const std::string& data = "01 02") {
    return CaptureEntry{ts, source, kind, report_id, data};
}

static const Json LABEL_MATCH = Json::parse(R"({"FF21": {"usage_page": "FF21", "usage": "0001"}})");

TEST_CASE("input 差分 → 监听型草稿") {
    const auto diffs = diff_sessions({}, {entry(1.0, "FF21", "input", 237, "0e ed 40 00")});
    const auto drafts = build_drafts(diffs, "DPI 800→1600", LABEL_MATCH);
    REQUIRE(drafts.size() == 1);
    const auto& d = drafts[0];
    CHECK(d["id"].get<std::string>().rfind(LEARNED_PREFIX, 0) == 0);
    CHECK(d["kind"] == "read");
    CHECK(d["report"] == "input");
    CHECK(d["template"] == Json::array());  // 监听型零写入
    CHECK(d["match"] == Json::parse(R"({"usage_page": "FF21", "usage": "0001"})"));
    CHECK(d["response"]["check"] == Json::parse("[null, null, null, null]"));  // 通配
    CHECK(d["learned"]["changes"] == Json::parse(
              "[[0, null, 14], [1, null, 237], [2, null, 64], [3, null, 0]]"));
    CHECK(d["learned"]["is_new"] == true);
    CHECK(d["label"] == "DPI 800→1600");
}

TEST_CASE("feature_read 差分 → feature 草稿") {
    const auto diffs = diff_sessions(
        {entry(1.0, "FF21", "feature_read", 237, "ed 01 20")},
        {entry(2.0, "FF21", "feature_read", 237, "ed 01 40")});
    const auto drafts = build_drafts(diffs, "改DPI");
    REQUIRE(drafts.size() == 1);
    CHECK(drafts[0]["report"] == "feature");
    CHECK(drafts[0]["template"] == Json::parse("[237]"));
    CHECK(drafts[0]["learned"]["changes"] == Json::parse("[[2, 32, 64]]"));
    CHECK(drafts[0]["learned"]["is_new"] == false);
}

TEST_CASE("volatile 差分跳过") {
    std::vector<CaptureEntry> noise;
    for (int i = 0; i < 10; ++i)
        noise.push_back(entry(static_cast<double>(i), "FF21", "input", 237,
                              "0" + std::to_string(i)));
    const auto diffs = diff_sessions(noise, noise);
    CHECK(build_drafts(diffs).empty());
}

TEST_CASE("草稿 id 清洗") {
    const auto diffs = diff_sessions({}, {entry(1.0, "41E4:2116 UP:0001", "input", 237, "aa")});
    const auto drafts = build_drafts(diffs);
    REQUIRE(drafts.size() == 1);
    CHECK(drafts[0]["id"] == "learned_41e4_2116_up_0001_237");
}

TEST_CASE("protocol 块取传入 match") {
    const auto drafts = build_drafts(diff_sessions({}, {entry(1.0, "FF21", "input", 237, "aa")}),
                                     "", LABEL_MATCH);
    const auto block = build_protocol_block(LABEL_MATCH["FF21"], drafts);
    CHECK(block["name"] == "learned");
    CHECK(block["transport"]["match"] == Json::parse(R"({"usage_page": "FF21", "usage": "0001"})"));
    CHECK(block["features"] == drafts);
}

TEST_CASE("protocol 块占位 match") {
    const auto drafts = build_drafts(diff_sessions({}, {entry(1.0, "FF21", "input", 237, "aa")}));
    const auto block = build_protocol_block(Json(), drafts);
    CHECK(block["transport"]["match"] == Json::parse(R"({"usage_page": "0000", "usage": "0000"})"));
}

TEST_CASE("合并到新设备") {
    Json db = Json::parse(R"({"$schema": "protocol-db/v2", "brands": {}, "devices": {}})");
    const auto drafts = build_drafts(diff_sessions({}, {entry(1.0, "FF21", "input", 237, "aa")}),
                                     "", LABEL_MATCH, 64);
    const auto block = build_protocol_block(LABEL_MATCH["FF21"], drafts);
    const auto& merged = merge_into_db(db, "291D:385D", block, "DPI 改动");
    CHECK(merged["source"] == "learning-mode");
    CHECK(merged["verified"] == false);
    CHECK(merged["note"] == "学习模式: DPI 改动");
    validate_db(db);  // 合并结果必须过校验
}

TEST_CASE("合并到已有 protocol 按 id 合并") {
    Json db = Json::parse(R"({
        "$schema": "protocol-db/v2", "brands": {}, "devices": {
            "046D:C54D": {
                "brand": "Logitech", "source": "hidapi-observed", "verified": true,
                "protocol": {"name": "hidpp", "transport": {
                    "match": {"usage_page": "FF00", "usage": "0001"}},
                    "features": [{"id": "ping", "label": "Ping", "kind": "command",
                                  "report": "output", "template": [16]}]}
            }}})");
    const auto drafts = build_drafts(
        diff_sessions({}, {entry(1.0, "FF00", "input", 237, "aa")}),
        "", Json::parse(R"({"FF00": {"usage_page": "FF00", "usage": "0001"}})"));
    const auto block = build_protocol_block(Json::parse(R"({"usage_page": "FF00", "usage": "0001"})"),
                                            drafts);
    const auto& merged = merge_into_db(db, "046D:C54D", block);
    CHECK(merged["protocol"]["name"] == "hidpp");   // 不动已有协议名
    CHECK(merged["verified"] == true);              // 不降级
    bool has_ping = false, has_learned = false;
    for (const auto& f : merged["protocol"]["features"]) {
        if (f["id"] == "ping") has_ping = true;
        if (f["id"].get<std::string>().rfind(LEARNED_PREFIX, 0) == 0) has_learned = true;
    }
    CHECK(has_ping);
    CHECK(has_learned);
    validate_db(db);
}

TEST_CASE("重学覆盖同 id") {
    Json db = Json::parse(R"({"$schema": "protocol-db/v2", "brands": {}, "devices": {}})");
    const auto drafts1 = build_drafts(diff_sessions({}, {entry(1.0, "FF21", "input", 237, "aa")}),
                                      "第一次");
    merge_into_db(db, "291D:385D", build_protocol_block(Json(), drafts1));
    const std::string old_id =
        db["devices"]["291D:385D"]["protocol"]["features"][0]["id"].get<std::string>();
    const auto drafts2 = build_drafts(diff_sessions({}, {entry(2.0, "FF21", "input", 237, "bb")}),
                                      "第二次");
    merge_into_db(db, "291D:385D", build_protocol_block(Json(), drafts2));
    const auto& features = db["devices"]["291D:385D"]["protocol"]["features"];
    CHECK(features.size() == 1);  // 同 id 覆盖不重复
    CHECK(features[0]["label"] == "第二次");
    CHECK(features[0]["id"] == old_id);
}

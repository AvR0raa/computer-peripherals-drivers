// test_report_io.cpp — 任务 2.2 单元测试（tests/test_report_io.py 平移，假设备离线运行）。
//
// 所有权约定：假设备由工厂移交 ReportIO（open 时 unique_ptr 接管）。
// 测试在 close() 之前访问设备状态（close 会销毁设备对象），与真实 hidapi 语义一致。
#include <cstring>

#include "doctest.h"
#include "core/report_io.hpp"
#include "test_report_io.hpp"

using namespace pt;
using namespace pt_test;

// ---------------------------------------------------------------------------
// render_template
// ---------------------------------------------------------------------------

TEST_CASE("render_template 纯整数") {
    const auto out = render_template(parse("[16, 255, 0]"), {});
    CHECK(out == b({0x10, 0xFF, 0x00}));
}

TEST_CASE("render_template 参数占位") {
    const auto out =
        render_template(parse(R"([16, "{device_index}", 0, "{request_id}", 0, 0, "{mark}"])"),
                        Params{{"device_index", 255}, {"request_id", 16}, {"mark", 66}});
    CHECK(out == b({0x10, 0xFF, 0x00, 0x10, 0x00, 0x00, 0x42}));
}

TEST_CASE("render_template 缺参数抛错") {
    CHECK_THROWS_AS(render_template(parse(R"(["{nope}"])"), {}), const RenderError&);
}

TEST_CASE("render_template 越界抛错") {
    CHECK_THROWS_AS(render_template(parse("[256]"), {}), const RenderError&);
    CHECK_THROWS_AS(render_template(parse(R"(["{x}"])"), Params{{"x", -1}}), const RenderError&);
    CHECK_THROWS_AS(render_template(parse("[1.5]"), {}), const RenderError&);
}

TEST_CASE("render_template 非法占位符抛错") {
    CHECK_THROWS_AS(render_template(parse(R"(["{1bad}"])"), {}), const RenderError&);
}

// ---------------------------------------------------------------------------
// match_response / extract_fields
// ---------------------------------------------------------------------------

TEST_CASE("match_response 精确与通配") {
    const Json check = parse("[16, 255, null, 16]");
    const Params params;
    CHECK(match_response(b({0x10, 0xFF, 0xAB, 0x10}), check, &params));
    CHECK_FALSE(match_response(b({0x11, 0xFF, 0xAB, 0x10}), check, &params));
    CHECK_FALSE(match_response(b({0x10, 0xFF, 0xAB}), check));  // 长度不足
}

TEST_CASE("match_response 参数引用与任一") {
    const Params params{{"device_index", 255}, {"mark", 66}};
    const Json check =
        parse(R"([16, "{device_index}", [0, 143], null, null, null, "{mark}"])");
    CHECK(match_response(b({0x10, 0xFF, 0x00, 0x10, 0x01, 0x02, 0x42}), check, &params));
    CHECK(match_response(b({0x10, 0xFF, 0x8F, 0x10, 0x01, 0x02, 0x42}), check, &params));
    CHECK_FALSE(match_response(b({0x10, 0xFF, 0x00, 0x10, 0x01, 0x02, 0x99}), check, &params));
}

TEST_CASE("extract_fields") {
    const auto out = extract_fields(b({0x10, 0xFF, 0x00, 0x10, 0x02, 0x50, 0x42}),
                                    parse(R"({"major": 4, "minor": 5})"));
    REQUIRE(out.count("major") == 1);
    REQUIRE(out.count("minor") == 1);
    CHECK(*out.at("major") == 2);
    CHECK(*out.at("minor") == 0x50);

    const auto out2 = extract_fields(b({0x00}), parse(R"({"x": 9})"));
    REQUIRE(out2.count("x") == 1);
    CHECK_FALSE(out2.at("x").has_value());  // 越界给 None
}

// ---------------------------------------------------------------------------
// ReportIO（假设备）
// ---------------------------------------------------------------------------

TEST_CASE("call 返回匹配的响应") {
    ReportIO io("fake");
    install_fake(io, {b({0xA1, 0x01, 0x20, 0x30, 0x00, 0x00, 0x00}),    // 鼠标移动推送，应被忽略
                      b({0x10, 0xFF, 0x00, 0x10, 0x02, 0x50, 0x42})});  // ping 响应
    const Params params{{"device_index", 255}, {"request_id", 16}, {"mark", 66}};
    const Json check =
        parse(R"([16, "{device_index}", 0, "{request_id}", null, null, "{mark}"])");
    io.open();
    const auto result =
        io.call(b({0x10, 0xFF, 0x00, 0x10, 0x00, 0x00, 0x42}), &check, &params, 300);
    CHECK(result.has_value());
    REQUIRE(result.has_value());
    CHECK(*result == b({0x10, 0xFF, 0x00, 0x10, 0x02, 0x50, 0x42}));
    CHECK(io.history.front().direction == "tx");
    CHECK(io.history.back().direction == "rx");
}

TEST_CASE("call 超时返回空") {
    ReportIO io("fake");
    install_fake(io);
    io.open();
    const Json check = parse("[16]");
    const auto result = io.call(b({0x10, 0xFF, 0x00, 0x10, 0x00, 0x00, 0x42}), &check, nullptr, 80);
    CHECK_FALSE(result.has_value());
}

TEST_CASE("call 无 check 返回第一个响应") {
    ReportIO io("fake");
    install_fake(io, {b({1, 2, 3})});
    io.open();
    const auto result = io.call(b({0x00}), nullptr, nullptr, 200);
    REQUIRE(result.has_value());
    CHECK(*result == b({1, 2, 3}));
}

TEST_CASE("history 订阅") {
    FakeDev* dev = new FakeDev(std::vector<Bytes>{b({9, 9})});
    std::vector<HistoryEntry> seen;
    ReportIO io("fake",
                [dev](const std::string&) { return std::unique_ptr<HidDevice>(dev); },
                [&](const HistoryEntry& e) { seen.push_back(e); });
    io.open();
    io.call(b({0x00}), nullptr, nullptr, 200);
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].direction == "tx");
    CHECK(seen[1].direction == "rx");
    CHECK(seen[0].data == b({0x00}));
}

TEST_CASE("feature 读写往返") {
    ReportIO io("fake");
    FakeDev* dev = install_fake(io);
    io.open();
    io.send_feature(b({0x07, 0x01, 0x02}));
    const auto out = io.get_feature(7, 4);
    REQUIRE(dev->features_sent.size() == 1);  // close 前访问设备状态
    CHECK(dev->features_sent[0] == b({0x07, 0x01, 0x02}));
    REQUIRE(out.has_value());
    CHECK(*out == b({7, 0xAA, 0xAA, 0xAA}));
}

TEST_CASE("打开失败抛 ReportError") {
    ReportIO io("ghost");
    install_boom(io);
    CHECK_THROWS_AS(io.open(), const ReportError&);
}

TEST_CASE("write 失败抛 ReportError") {
    ReportIO io("fake");
    struct BrokenDev : FakeDev {
        using FakeDev::FakeDev;
        int write(const Bytes&) override { return -1; }
    };
    BrokenDev* dev = new BrokenDev();
    io = ReportIO("fake", [dev](const std::string&) {
        return std::unique_ptr<HidDevice>(dev);
    });
    io.open();
    CHECK_THROWS_AS(io.write_output(b({0x00})), const ReportError&);
}

TEST_CASE("close 幂等") {
    ReportIO io("fake");
    install_fake(io);
    io.open();
    CHECK(io.is_open());
    io.close();
    CHECK_FALSE(io.is_open());
    io.close();  // 二次关闭无操作不崩溃
    CHECK_FALSE(io.is_open());
}

// ---------------------------------------------------------------------------
// call_multi / execute_feature（M2.3 引擎分支路由）
// ---------------------------------------------------------------------------

static const Json PING_FEATURE = parse(R"({
    "id": "ping", "kind": "command", "report": "output",
    "template": [16, "{device_index}", 0, "{request_id}", 0, 0, "{mark}"],
    "params": {
        "device_index": {"type": "int", "default": 255, "min": 0, "max": 255},
        "request_id": {"type": "int", "default": 16, "min": 16, "max": 23},
        "mark": {"type": "int", "default": 66, "min": 0, "max": 255}
    },
    "response": {
        "check": [16, "{device_index}", 0, "{request_id}", null, null, "{mark}"],
        "extract": {"major": 4, "minor": 5},
        "alt": {
            "label": "错误应答",
            "check": [16, "{device_index}", 143, null, null, null, null],
            "extract": {"error_code": 5}
        }
    }
})");

static const Params P{{"device_index", 255}, {"request_id", 16}, {"mark", 66}};

TEST_CASE("execute_feature 主分支") {
    ReportIO io("fake");
    install_fake(io, {b({0x10, 0xFF, 0x00, 0x10, 0x02, 0x50, 0x42})});
    const auto result = execute_feature(io, PING_FEATURE, &P, 300);
    CHECK(result.ok);
    CHECK_FALSE(result.branch.has_value());
    REQUIRE(result.decoded.count("major") == 1);
    REQUIRE(result.decoded.count("minor") == 1);
    CHECK(*result.decoded.at("major") == 2);
    CHECK(*result.decoded.at("minor") == 0x50);
}

TEST_CASE("execute_feature 错误分支") {
    ReportIO io("fake");
    install_fake(io, {b({0x10, 0xFF, 0x8F, 0x00, 0x10, 0x01, 0x00})});
    const auto result = execute_feature(io, PING_FEATURE, &P, 300);
    CHECK_FALSE(result.ok);
    REQUIRE(result.branch.has_value());
    CHECK(*result.branch == "错误应答");
    REQUIRE(result.decoded.count("error_code") == 1);
    CHECK(*result.decoded.at("error_code") == 1);
}

TEST_CASE("execute_feature 超时") {
    ReportIO io("fake");
    install_fake(io);
    const auto result = execute_feature(io, PING_FEATURE, &P, 80);
    CHECK_FALSE(result.response.has_value());
    CHECK_FALSE(result.ok);
}

TEST_CASE("execute_feature 参数默认回填") {
    ReportIO io("fake");
    FakeDev* dev = install_fake(io, {b({0x10, 0xFF, 0x00, 0x10, 0x00, 0x00, 0x42})});
    const auto result = execute_feature(io, PING_FEATURE, nullptr, 300);
    CHECK(result.ok);
    REQUIRE(dev->written.size() == 1);
    CHECK(dev->written[0] == b({0x10, 0xFF, 0x00, 0x10, 0x00, 0x00, 0x42}));  // default 回填
}

TEST_CASE("call_multi 分支顺序") {
    ReportIO io("fake");
    install_fake(io, {b({0x10, 0xFF, 0x00, 0x10, 0x00, 0x00, 0x42})});
    ReportIO::Checks checks;
    checks.emplace_back("a", parse("[16, 255, 0]"));
    checks.emplace_back("b", parse("[16, 255, null]"));
    io.open();
    const auto result = io.call_multi(b({0x00}), checks, nullptr, 200);
    REQUIRE(result.has_value());
    CHECK(result->first == b({0x10, 0xFF, 0x00, 0x10, 0x00, 0x00, 0x42}));
    CHECK(result->second == "a");  // 同时匹配多分支时，按 checks 顺序取第一个
}

// ---------------------------------------------------------------------------
// read_until / 监听型 execute_feature（M4.3 学习模式产出）
// ---------------------------------------------------------------------------

TEST_CASE("read_until 命中且零写入") {
    ReportIO io("fake");
    FakeDev* dev = install_fake(io, {b({0xED, 1, 0x40})});
    ReportIO::Checks checks;
    checks.emplace_back("hit", parse("[237]"));  // 0xED
    io.open();
    const auto result = io.read_until(checks, nullptr, 300);
    REQUIRE(result.has_value());
    CHECK(result->first == b({0xED, 1, 0x40}));
    CHECK(result->second == "hit");
    CHECK(dev->written.empty());  // 零写入
}

TEST_CASE("read_until 超时") {
    ReportIO io("fake");
    install_fake(io);
    ReportIO::Checks checks;
    checks.emplace_back("x", parse("[255]"));  // 0xFF
    io.open();
    const auto result = io.read_until(checks, nullptr, 80);
    CHECK_FALSE(result.has_value());
}

TEST_CASE("execute_feature 监听型零写入") {
    ReportIO io("fake");
    FakeDev* dev = install_fake(io, {b({0xED, 1, 0x40, 0x00})});
    const Json feature = parse(R"({
        "id": "learned_dpi", "kind": "read", "report": "input",
        "template": [],
        "response": {"min_len": 4, "check": [237], "extract": {"value": 2}}
    })");
    const auto result = execute_feature(io, feature, nullptr, 300);
    CHECK(result.ok);
    CHECK_FALSE(result.branch.has_value());
    REQUIRE(result.decoded.count("value") == 1);
    CHECK(*result.decoded.at("value") == 0x40);
    CHECK(dev->written.empty());  // 全程零写入
}

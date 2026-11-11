// test_dpi.cpp — M2.5b 单元测试（tests/test_dpi.py 平移，脚本化 HID++ 应答，sw_id 对齐）。
#include <algorithm>

#include "doctest.h"
#include "core/dpi.hpp"
#include "hidpp_scripted.hpp"
#include "test_report_io.hpp"

using namespace pt;
using pt_test::b;
using pt_test::Bytes;
using pt_test::FakeDev;
using pt_test::HidppScript;
using pt_test::install_fake;
using pt_test::Json;

static constexpr int DEV = 1;
static constexpr int IDX = 0x08;  // 假设 0x2201 被分配到 index 8

// GetSensorDpiList 应答 payload: [X] + BE16 序列 + 0x0000 结尾（截断/填充到 16 字节）
static Bytes dpi_list_payload(const std::vector<int>& values, int page = 0) {
    Bytes body{static_cast<std::uint8_t>(page)};
    for (int v : values) {
        body.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        body.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
    body.push_back(0x00);
    body.push_back(0x00);
    body.resize(std::min<std::size_t>(16, body.size()));
    body.resize(16, 0x00);
    return body;
}

TEST_CASE("get_dpi_list 纯值列表") {
    HidppScript script;
    ReportIO link("x");
    FakeDev* dev = install_fake(link, {
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 1, dpi_list_payload({100, 200, 400}), true),
    });
    const auto result = get_dpi_list(link, DEV);
    REQUIRE(result.size() == 3);
    CHECK(result[0] == 100);
    CHECK(result[1] == 200);
    CHECK(result[2] == 400);
    REQUIRE(dev->written.size() >= 2);
    CHECK(dev->written[1][0] == 0x11);              // 长报文请求
    CHECK((dev->written[1][3] >> 4) == 1);          // funct 1
}

TEST_CASE("get_dpi_list 步进编码展开") {
    // 档位表: [400] + 步进编码(step=50, last=800) → 展开 400,450,...,800
    const int step_val = 0xE000 | 50;
    Bytes payload{0x00, 0x01, 0x90,
                  static_cast<std::uint8_t>((step_val >> 8) & 0xFF),
                  static_cast<std::uint8_t>(step_val & 0xFF),
                  0x03, 0x20, 0x00, 0x00};
    payload.resize(16, 0x00);
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 1, payload, true),
    });
    const auto result = get_dpi_list(link, DEV);
    REQUIRE_FALSE(result.empty());
    CHECK(result.front() == 400);
    CHECK(result.back() == 800);
    std::vector<int> expected;
    for (int v = 400; v <= 800; v += 50) expected.push_back(v);
    CHECK(result == expected);
}

TEST_CASE("get_dpi_list 不支持返回空") {
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {script.gf(DEV, 0)});
    CHECK(get_dpi_list(link, DEV).empty());
}

TEST_CASE("read_dpi 短应答") {
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, Bytes{0x00, 0x03, 0x20}),  // [X, 03, 20] = 800
    });
    const auto r = read_dpi(link, DEV);
    REQUIRE(r.x.has_value());
    CHECK(*r.x == 800);
    CHECK_FALSE(r.y.has_value());
    CHECK_FALSE(r.lod.has_value());
    CHECK_FALSE(r.default_dpi.has_value());
}

TEST_CASE("read_dpi 长应答带默认值") {
    // 短请求、长应答: payload [X, hi, lo, defHi, defLo, ...]
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, Bytes{0x00, 0x03, 0x20, 0x01, 0x2C}, true),
    });
    const auto r = read_dpi(link, DEV);
    REQUIRE(r.x.has_value());
    CHECK(*r.x == 800);
    REQUIRE(r.default_dpi.has_value());
    CHECK(*r.default_dpi == 300);
}

TEST_CASE("read_dpi 为 0 时回退默认值") {
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, Bytes{0x00, 0x00, 0x00, 0x01, 0x2C}, true),
    });
    const auto r = read_dpi(link, DEV);
    REQUIRE(r.x.has_value());
    CHECK(*r.x == 300);  // 0 → 默认值
}

// ---------------------------------------------------------------------------
// 0x2202 Extended Adjustable DPI（PRO X2 等新款，Solaar #2207）
// ---------------------------------------------------------------------------

TEST_CASE("read_dpi extended feature") {
    // 0x2201 不支持 → 0x2202 分配 idx 9，能力 byte2 bit0=Y bit1=LOD
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {
        script.gf(DEV, 0),                                    // 0x2201 无
        script.gf(DEV, 0x09),                                 // 0x2202 → 9
        script.call(DEV, 0x09, 1, Bytes{0x00, 0x00, 0x03}),   // GetInfo: Y=1 LOD=1
        script.call(DEV, 0x09, 5,
                    Bytes{0x00, 0x0B, 0xB8, 0x00, 0x00, 0x0B, 0xB8, 0x00, 0x00, 0x02}, true),
    });
    const auto r = read_dpi(link, DEV);
    REQUIRE(r.x.has_value());
    CHECK(*r.x == 3000);
    REQUIRE(r.y.has_value());
    CHECK(*r.y == 3000);
    REQUIRE(r.lod.has_value());
    CHECK(*r.lod == 2);
}

TEST_CASE("get_dpi_list extended") {
    const int step_val = 0xE000 | 50;
    Bytes payload{0x00, 0x00, 0x00, 0x01, 0x90,
                  static_cast<std::uint8_t>((step_val >> 8) & 0xFF),
                  static_cast<std::uint8_t>(step_val & 0xFF),
                  0x03, 0x20, 0x00, 0x00};
    payload.resize(16, 0x00);
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {
        script.gf(DEV, 0),                                    // 0x2201 无
        script.gf(DEV, 0x09),                                 // 0x2202 → 9
        script.call(DEV, 0x09, 1, Bytes{0x00, 0x00, 0x00}),   // GetInfo
        script.call(DEV, 0x09, 2, payload, true),
    });
    std::vector<int> expected;
    for (int v = 400; v <= 800; v += 50) expected.push_back(v);
    CHECK(get_dpi_list(link, DEV) == expected);
}

// ---------------------------------------------------------------------------
// set_dpi 安全写
// ---------------------------------------------------------------------------

TEST_CASE("set_dpi 成功") {
    // set_dpi: detect(GF) → read(detect GF + read) → set → read(detect GF + read)
    HidppScript script;
    ReportIO link("x");
    FakeDev* dev = install_fake(link, {
        script.gf(DEV, IDX),
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, Bytes{0x00, 0x03, 0x20}),   // 读旧 800
        script.call(DEV, IDX, 3, Bytes{0x00, 0x06, 0x40}),   // SetSensorDpi ACK
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, Bytes{0x00, 0x06, 0x40}),   // 读回 1600
    });
    const auto out = set_dpi(link, DEV, 1600);
    CHECK(out.ok);
    REQUIRE(out.wrote.has_value());
    CHECK(*out.wrote == 1600);
    REQUIRE(out.old.has_value());
    CHECK(*out.old == 800);
    REQUIRE(out.readback.has_value());
    CHECK(*out.readback == 1600);
    CHECK_FALSE(out.rolled_back);
    CHECK(out.feature == "0x2201");
    REQUIRE(dev->written.size() >= 4);
    const Bytes& set_wire = dev->written[3];
    CHECK(set_wire[0] == 0x10);
    CHECK(set_wire[2] == IDX);
    CHECK((set_wire[3] >> 4) == 3);                          // funct 3
    CHECK(Bytes(set_wire.begin() + 4, set_wire.begin() + 7) == Bytes{0x00, 0x06, 0x40});
}

TEST_CASE("set_dpi 验证失败回滚") {
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {
        script.gf(DEV, IDX),
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, Bytes{0x00, 0x03, 0x20}),   // 读旧 800
        script.call(DEV, IDX, 3, Bytes{0x00, 0x06, 0x40}),   // 写 1600
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, Bytes{0x00, 0x03, 0x20}),   // 读回=旧值 → 失败
        script.call(DEV, IDX, 3, Bytes{0x00, 0x03, 0x20}),   // 回滚写
        script.gf(DEV, IDX),
        script.call(DEV, IDX, 2, Bytes{0x00, 0x03, 0x20}),   // 读回 800
    });
    const auto out = set_dpi(link, DEV, 1600);
    CHECK_FALSE(out.ok);
    CHECK(out.rolled_back);
    bool has_rollback_log = false;
    for (const auto& line : out.log) has_rollback_log = has_rollback_log || line.find("回滚") != std::string::npos;
    CHECK(has_rollback_log);
}

TEST_CASE("set_dpi 超范围拒绝") {
    ReportIO link("x");
    install_fake(link);
    const auto out = set_dpi(link, DEV, 99999);
    CHECK_FALSE(out.ok);
    CHECK_FALSE(out.wrote.has_value());
    REQUIRE_FALSE(out.log.empty());
    CHECK(out.log[0].find("超出安全范围") != std::string::npos);
}

TEST_CASE("set_dpi 无 feature") {
    HidppScript script;
    ReportIO link("x");
    install_fake(link, {script.gf(DEV, 0), script.gf(DEV, 0)});
    const auto out = set_dpi(link, DEV, 1600);
    CHECK_FALSE(out.ok);
    REQUIRE_FALSE(out.log.empty());
    CHECK(out.log[0].find("0x2201/0x2202") != std::string::npos);
}

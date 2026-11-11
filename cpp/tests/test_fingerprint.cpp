// test_fingerprint.cpp — 任务 1.3 + M2.1 单元测试（tests/test_fingerprint.py 平移）。
// 逻辑测试用内存 db；数据集完整性测试直接读真实 protocol_db/db.json（两版共享）。
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>

#include "doctest.h"
#include "core/device_manager.hpp"
#include "core/fingerprint.hpp"

using namespace pt;
namespace fs = std::filesystem;

static DeviceInfo device(unsigned vid = 0x046D, unsigned pid = 0xC54D) {
    DeviceInfo d;
    d.path = "\\\\?\\fake";
    d.vendor_id = vid;
    d.product_id = pid;
    d.usage_page = 1;
    d.usage = 2;
    d.bus_type = 1;
    return d;
}

// 临时目录（对应 pytest tmp_path）
class TempDir {
public:
    TempDir() {
        static int counter = 0;
        path_ = fs::temp_directory_path()
                / ("pt_test_" + std::to_string(++counter) + "_"
                   + std::to_string(std::time(nullptr)));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    fs::path file(const std::string& name) const { return path_ / name; }
    void write(const std::string& name, const std::string& text) const {
        std::ofstream f(file(name), std::ios::binary);
        f << text;
    }
private:
    fs::path path_;
};

// 内存测试库（对应 fixture db）
static Json make_db() {
    return Json::parse(R"({
        "$schema": "protocol-db/v1",
        "brands": {
            "046D": {"name": "Logitech", "name_zh": "罗技", "sdk": ["GHSDK"]},
            "25A7": {"name": "Dareu", "name_zh": "达尔优"}
        },
        "devices": {
            "046D:C54D": {
                "brand": "Logitech", "product": "USB Receiver",
                "source": "hidapi-observed", "verified": false
            }
        }
    })");
}

// ---------------------------------------------------------------------------
// 逻辑测试（内存 db）
// ---------------------------------------------------------------------------

TEST_CASE("精确 PID 命中 = KNOWN") {
    const auto result = fingerprint(device(0x046D, 0xC54D), make_db());
    CHECK(result.status == DeviceStatus::Known);
    REQUIRE(result.brand.has_value());
    REQUIRE(result.brand_zh.has_value());
    CHECK(*result.brand == "Logitech");
    CHECK(*result.brand_zh == "罗技");
    CHECK_FALSE(result.entry.is_null());
    CHECK(result.entry["product"] == "USB Receiver");
}

TEST_CASE("品牌有 SDK = SDK 状态") {
    const auto result = fingerprint(device(0x046D, 0x9999), make_db());
    CHECK(result.status == DeviceStatus::Sdk);
    REQUIRE(result.sdk.size() == 1);
    CHECK(result.sdk[0] == "GHSDK");
    CHECK(result.entry.is_null());
}

TEST_CASE("品牌无 SDK = UNKNOWN 但带品牌名") {
    const auto result = fingerprint(device(0x25A7, 0x0001), make_db());
    CHECK(result.status == DeviceStatus::Unknown);
    REQUIRE(result.brand.has_value());
    CHECK(*result.brand == "Dareu");
    REQUIRE(result.brand_zh.has_value());
    CHECK(*result.brand_zh == "达尔优");
    CHECK(result.sdk.empty());
}

TEST_CASE("完全未知 VID") {
    const auto result = fingerprint(device(0xDEAD, 0xBEEF), make_db());
    CHECK(result.status == DeviceStatus::Unknown);
    CHECK_FALSE(result.brand.has_value());
    CHECK(result.detail.find("学习模式") != std::string::npos);
}

// ---------------------------------------------------------------------------
// load_db 校验
// ---------------------------------------------------------------------------

TEST_CASE("load_db 校验 schema") {
    TempDir tmp;
    tmp.write("db.json", R"({"brands": {}, "devices": {}})");
    CHECK_THROWS_AS(load_db(tmp.file("db.json")), const ProtocolDBError&);
}

TEST_CASE("load_db 拒绝缺失文件") {
    TempDir tmp;
    CHECK_THROWS_AS(load_db(tmp.file("nope.json")), const ProtocolDBError&);
}

TEST_CASE("load_db 拒绝非法设备键") {
    TempDir tmp;
    tmp.write("db.json",
              R"({"$schema": "protocol-db/v1", "brands": {}, "devices": {"NOTAKEY": {}}})");
    CHECK_THROWS_AS(load_db(tmp.file("db.json")), const ProtocolDBError&);
}

// ---------------------------------------------------------------------------
// schema v2 protocol 块校验
// ---------------------------------------------------------------------------

static const char* VALID_PROTOCOL_JSON = R"({
    "name": "hidpp",
    "transport": {"match": {"usage_page": "FF00", "usage": "0001"}, "short_len": 7},
    "features": [{
        "id": "ping", "label": "Ping", "kind": "command", "report": "output",
        "template": [16, "{device_index}", 0, "{request_id}", 0, 0, "{mark}"],
        "params": {"device_index": {"type": "int", "default": 255, "min": 0, "max": 255}},
        "response": {"min_len": 7,
                     "check": [16, "{device_index}", 0, "{request_id}", null, null, "{mark}"],
                     "extract": {"major": 4, "minor": 5}}
    }]
})";

// 写入以 devices 为内容的 v2 库文件
static fs::path write_db_with(TempDir& tmp, const Json& devices) {
    Json db = {{"$schema", "protocol-db/v2"}, {"brands", Json::object()}, {"devices", devices}};
    const fs::path path = tmp.file("db.json");
    std::ofstream f(path, std::ios::binary);
    f << db.dump();
    return path;
}

// 断言 load_db 抛 ProtocolDBError 且消息含 fragment
static void check_load_error(const fs::path& path, const std::string& fragment) {
    try {
        load_db(path);
        CHECK_MESSAGE(false, "应当抛出 ProtocolDBError");
    } catch (const ProtocolDBError& exc) {
        CHECK(std::string(exc.what()).find(fragment) != std::string::npos);
    }
}

TEST_CASE("v2 合法 protocol 可加载") {
    TempDir tmp;
    Json devices = Json::object();
    devices["046D:C54D"] = Json{{"brand", "Logitech"},
                                {"protocol", Json::parse(VALID_PROTOCOL_JSON)}};
    const auto path = write_db_with(tmp, devices);
    const auto db = load_db(path);
    CHECK(db["devices"]["046D:C54D"]["protocol"]["name"] == "hidpp");
}

TEST_CASE("v2 非法 feature 被拒") {
    TempDir tmp;
    Json devices = Json::object();

    Json protocol = Json::parse(VALID_PROTOCOL_JSON);
    protocol["features"][0]["template"] = Json::array({999});
    devices["046D:C54D"] = Json{{"protocol", protocol}};
    check_load_error(write_db_with(tmp, devices), "template");

    protocol = Json::parse(VALID_PROTOCOL_JSON);
    protocol["features"][0]["template"] = Json::array({"{1bad}"});
    devices["046D:C54D"] = Json{{"protocol", protocol}};
    check_load_error(write_db_with(tmp, devices), "template");

    protocol = Json::parse(VALID_PROTOCOL_JSON);
    protocol["features"][0]["kind"] = "write";
    devices["046D:C54D"] = Json{{"protocol", protocol}};
    check_load_error(write_db_with(tmp, devices), "read_from");

    protocol = Json::parse(VALID_PROTOCOL_JSON);
    protocol["features"][0]["report"] = "serial";
    devices["046D:C54D"] = Json{{"protocol", protocol}};
    check_load_error(write_db_with(tmp, devices), "report");

    protocol = Json::parse(VALID_PROTOCOL_JSON);
    protocol["features"][0]["params"]["x"] = Json{{"type", "string"}};
    devices["046D:C54D"] = Json{{"protocol", protocol}};
    check_load_error(write_db_with(tmp, devices), "params");
}

TEST_CASE("v2 非法 transport 被拒") {
    TempDir tmp;
    Json protocol = Json::parse(VALID_PROTOCOL_JSON);
    protocol["transport"]["match"]["usage_page"] = "XYZ";
    Json devices = Json::object();
    devices["046D:C54D"] = Json{{"protocol", protocol}};
    check_load_error(write_db_with(tmp, devices), "usage_page");
}

TEST_CASE("v2 非法 response.check 被拒") {
    TempDir tmp;
    Json protocol = Json::parse(VALID_PROTOCOL_JSON);
    protocol["features"][0]["response"]["check"][4] = "nope";
    Json devices = Json::object();
    devices["046D:C54D"] = Json{{"protocol", protocol}};
    check_load_error(write_db_with(tmp, devices), "check");
}

// ---------------------------------------------------------------------------
// 数据集完整性（M5b 扩容纪律：只录有据条目）
// ---------------------------------------------------------------------------

TEST_CASE("真实 db 完整性") {
    const auto db = load_db(default_db_path());
    CHECK(db["brands"].size() >= 20);
    CHECK(db["devices"].size() >= 40);
    for (auto it = db["brands"].begin(); it != db["brands"].end(); ++it) {
        const std::string key = it.key();
        for (char c : key) {
            const bool upper_hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
            const std::string msg = "品牌 VID 须大写4位hex: " + key;
            REQUIRE_MESSAGE(upper_hex, msg.c_str());
        }
        const bool has_name = it.value().contains("name")
                              && !it.value()["name"].get<std::string>().empty();
        const std::string msg = "品牌缺 name: " + key;
        CHECK_MESSAGE(has_name, msg.c_str());
    }
    for (auto it = db["devices"].begin(); it != db["devices"].end(); ++it) {
        const std::string& key = it.key();
        const bool key_ok = key.size() == 9 && key[4] == ':'
                            && is_hex4(key.substr(0, 4)) && is_hex4(key.substr(5, 4));
        const std::string msg1 = "设备键格式: " + key;
        CHECK_MESSAGE(key_ok, msg1.c_str());
        const bool has_bp = it.value().contains("brand") && it.value().contains("product")
                            && !it.value()["brand"].get<std::string>().empty()
                            && !it.value()["product"].get<std::string>().empty();
        const std::string msg2 = "设备缺 brand/product: " + key;
        CHECK_MESSAGE(has_bp, msg2.c_str());
        const bool has_source = it.value().contains("source")
                                && !it.value()["source"].get<std::string>().empty();
        const std::string msg3 = "设备缺 source（纪律：只录有据条目）: " + key;
        CHECK_MESSAGE(has_source, msg3.c_str());
        const std::string msg4 = "设备缺 type(鼠标/键盘/耳机/接收器): " + key;
        CHECK_MESSAGE(it.value().contains("type"), msg4.c_str());
    }
}

TEST_CASE("真实 db 新品牌命中") {
    const auto db = load_db(default_db_path());
    // 雷蛇 DeathAdder V2 → 精确条目命中（KNOWN 优先于品牌 SDK 路线）
    auto result = fingerprint(device(0x1532, 0x0084), db);
    CHECK(result.status == DeviceStatus::Known);
    REQUIRE(result.brand_zh.has_value());
    CHECK(*result.brand_zh == "雷蛇");
    CHECK(result.entry["product"] == "DeathAdder V2");
    bool has_chroma = false;
    for (const auto& s : result.sdk) has_chroma = has_chroma || s == "ChromaSDK";
    CHECK(has_chroma);  // 品牌级 SDK 信息仍在

    // HyperX 金士顿线 → 精确条目命中（品牌无 SDK）
    result = fingerprint(device(0x0951, 0x16A4), db);
    CHECK(result.status == DeviceStatus::Known);
    REQUIRE(result.brand.has_value());
    CHECK(*result.brand == "HyperX");
    CHECK(result.sdk.empty());

    // Redragon（Microdia 线）→ 品牌识别
    result = fingerprint(device(0x0C45, 0xFC30), db);
    REQUIRE(result.brand_zh.has_value());
    CHECK(*result.brand_zh == "红龙");
}

TEST_CASE("真实 db v2 可加载") {
    const auto db = load_db(default_db_path());
    CHECK(db["devices"]["046D:C54D"]["protocol"]["features"][0]["id"] == "ping");
}

// ---------------------------------------------------------------------------
// save_db / input 监听型 / feature 级 match（M4.3）
// ---------------------------------------------------------------------------

TEST_CASE("save_db 往返一致且无残留") {
    TempDir tmp;
    const auto path = tmp.file("db.json");
    const Json db = Json::parse(
        R"({"$schema": "protocol-db/v2", "brands": {"046D": {"name": "Logitech"}}, "devices": {}})");
    save_db(db, path);
    const auto loaded = load_db(path);
    CHECK(loaded["brands"]["046D"]["name"] == "Logitech");
    CHECK_FALSE(fs::exists(tmp.file("db.json.tmp")));  // 原子替换后无残留
}

TEST_CASE("save_db 拒绝非法且不落盘") {
    TempDir tmp;
    const auto path = tmp.file("db.json");
    const Json bad = Json::parse(R"({"$schema": "bad", "brands": {}, "devices": {}})");
    CHECK_THROWS_AS(save_db(bad, path), const ProtocolDBError&);
    CHECK_FALSE(fs::exists(path));  // 校验失败不落盘
}

TEST_CASE("input 监听型合法") {
    TempDir tmp;
    const auto path = tmp.file("db.json");
    const Json db = Json::parse(R"({
        "$schema": "protocol-db/v2", "brands": {}, "devices": {
            "291D:385D": {"protocol": {
                "name": "learned",
                "transport": {"match": {"usage_page": "FF21", "usage": "0001"}},
                "features": [{
                    "id": "learned_x", "label": "监听", "kind": "read",
                    "report": "input", "template": [],
                    "response": {"min_len": 4, "check": [null, null, null, null]}
                }]
            }}}})");
    save_db(db, path);  // 不抛即通过
    CHECK(load_db(path)["devices"]["291D:385D"]["protocol"]["features"][0]["report"] == "input");
}

TEST_CASE("feature 级 match 校验") {
    TempDir tmp;
    Json protocol = Json::parse(VALID_PROTOCOL_JSON);
    protocol["features"][0]["match"] = Json{{"usage_page", "XYZ"}, {"usage", "0001"}};
    Json devices = Json::object();
    devices["046D:C54D"] = Json{{"protocol", protocol}};
    check_load_error(write_db_with(tmp, devices), "match");
}

TEST_CASE("feature 级 match 合法") {
    TempDir tmp;
    Json protocol = Json::parse(VALID_PROTOCOL_JSON);
    protocol["features"][0]["match"] = Json{{"usage_page", "FF21"}, {"usage", "0001"}};
    Json devices = Json::object();
    devices["046D:C54D"] = Json{{"protocol", protocol}};
    const auto db = load_db(write_db_with(tmp, devices));
    CHECK(db["devices"]["046D:C54D"]["protocol"]["features"][0]["match"]["usage_page"] == "FF21");
}

// ---------------------------------------------------------------------------
// 真实 db.json 完整性 + 真机观察到的设备命中
// ---------------------------------------------------------------------------

TEST_CASE("真实 db 命中本机设备") {
    const auto db = load_db(default_db_path());
    // 罗技 Unifying 接收器（本机实测存在）
    auto result = fingerprint(device(0x046D, 0xC54D), db);
    CHECK(result.status == DeviceStatus::Known);

    // MCHOSE V9 PRO（本机实测存在的游戏耳机，用户确认）
    result = fingerprint(device(0x291D, 0x385D), db);
    CHECK(result.status == DeviceStatus::Known);
    CHECK(result.entry["note"].get<std::string>().rfind("游戏耳机", 0) == 0);
}

TEST_CASE("精确条目优先于品牌 SDK") {
    const auto db = load_db(default_db_path());
    const auto exact = fingerprint(device(0x046D, 0xC54D), db);
    CHECK(exact.status == DeviceStatus::Known);
    CHECK_FALSE(exact.entry.is_null());
}

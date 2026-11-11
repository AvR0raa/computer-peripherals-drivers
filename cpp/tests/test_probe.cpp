// test_probe.cpp — M5 多设备测试支持：core/probe.py 单元测试（tests/test_probe.py 平移）。
#include "doctest.h"
#include "core/probe.hpp"
#include "test_report_io.hpp"

using namespace pt;
using pt_test::bhex;
using pt_test::Bytes;
using pt_test::FakeDev;
using pt_test::HidDevice;

// KBD_VENDOR_DESC 同 test_descriptor：含 feature report id=3 的厂商接口
static const Bytes KBD_VENDOR_DESC = bhex(
    "05 01 09 06 A1 01"
    "85 01"
    "05 07 19 E0 29 E7 15 00 25 01 75 01 95 08 81 02"
    "95 01 75 08 81 03"
    "05 08 19 01 29 05 15 00 25 01 75 01 95 05 91 02"
    "95 01 75 03 91 01"
    "C0"
    "05 0C 09 01 A1 01"
    "85 02"
    "0A 23 02 15 00 25 01 75 01 95 01 81 02"
    "95 07 75 01 81 03"
    "C0"
    "06 00 FF 09 01 A1 01"
    "85 03"
    "09 01 15 00 26 FF 00 75 08 95 08 B1 02"
    "C0"
);

static DeviceInfo mk(const std::string& path, int usage_page, int usage) {
    DeviceInfo d;
    d.path = path;
    d.usage_page = static_cast<unsigned>(usage_page);
    d.usage = static_cast<unsigned>(usage);
    d.bus_type = 1;
    return d;
}

// 安装一个全局假设备工厂（对应 Python monkeypatch _open_device）
static DevFactory fake_factory() {
    return [](const std::string&) -> std::unique_ptr<HidDevice> { return std::make_unique<FakeDev>(); };
}

TEST_CASE("feature_report_ids") {
    const auto info = parse_report_descriptor(KBD_VENDOR_DESC);
    const auto ids = feature_report_ids(info);
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == 3);
}

TEST_CASE("feature_report_ids 无 feature 回退 [0]") {
    const Bytes mouse = bhex(
        "05 01 09 02 A1 01 09 01 A1 00 05 09 19 01 29 03 15 00 25 01"
        "95 03 75 01 81 02 95 01 75 05 81 01 05 01 09 30 09 31 15 81 25 7F"
        "75 08 95 02 81 06 C0 C0");
    const auto ids = feature_report_ids(parse_report_descriptor(mouse));
    REQUIRE(ids.size() == 1);
    CHECK(ids[0] == 0);
}

TEST_CASE("probe_interface 读 feature 原始内容") {
    // 特制假设备：get_feature 返回 id=3 的 64 字节（0x03, 0x10..0x1F, 0 填充）
    class FeatureDev : public FakeDev {
    public:
        Bytes get_feature(int report_id, int size) override {
            if (report_id != 3) return FakeDev::get_feature(report_id, size);
            Bytes out{0x03};
            for (int i = 0x10; i < 0x20; ++i) out.push_back(static_cast<std::uint8_t>(i));
            out.resize(64, 0x00);
            return out;
        }
    };
    FeatureDev* dev = new FeatureDev();
    const DevFactory factory = [dev](const std::string&) {
        return std::unique_ptr<HidDevice>(dev);
    };
    const auto probe = probe_interface("\\fake", 0xFF00, 0x0001,
                                       [](const std::string&) { return KBD_VENDOR_DESC; },
                                       factory);
    REQUIRE(probe.descriptor_len.has_value());
    CHECK(*probe.descriptor_len == static_cast<int>(KBD_VENDOR_DESC.size()));
    REQUIRE(probe.feature_ids.size() == 1);
    CHECK(probe.feature_ids[0] == 3);
    REQUIRE(probe.reads.count("3") == 1);
    REQUIRE(probe.reads.at("3").has_value());
    CHECK((*probe.reads.at("3"))[0] == 0x03);
    CHECK_FALSE(probe.error.has_value());
}

TEST_CASE("probe_interface 描述符读取失败") {
    const auto probe = probe_interface("\\fake", 0xFF21, 0x0001,
                                       [](const std::string&) { return Bytes{}; },
                                       fake_factory());
    CHECK(probe.error.has_value());
    CHECK(probe.feature_ids.empty());
}

TEST_CASE("probe_vendor_interfaces 只挑 vendor 页") {
    int called = 0;
    std::string last_path;
    const ReadDescriptorFn read = [&](const std::string& path) {
        ++called;
        last_path = path;
        return KBD_VENDOR_DESC;
    };
    const std::vector<DeviceInfo> ifaces = {
        mk("a", 0x0001, 0x0006),   // 标准键盘接口 → 跳过
        mk("b", 0xFF21, 0x0001),   // vendor → 探测
    };
    const auto results = probe_vendor_interfaces(ifaces, read, fake_factory());
    REQUIRE(results.size() == 1);
    CHECK(results[0].usage_page == 0xFF21);
    REQUIRE(called == 1);
    CHECK(last_path == "b");
}

// rawhid 风格描述符：usage 0 + 64 字节 input + 64 字节 output（Ace68-II IF1 同款）
static const Bytes RAWHID_DESC = bhex(
    "05 01 09 00 A1 01"
    "09 01 15 00 26 FF 00 75 08 95 40 81 02"
    "09 01 15 00 26 FF 00 75 08 95 40 91 02"
    "C0"
);

TEST_CASE("suspect_rawhid_interfaces 识别") {
    const std::vector<DeviceInfo> ifaces = {
        mk("kbd", 0x0001, 0x0006),   // 标准键盘 → 跳过
        mk("raw", 0x0001, 0x0000),   // rawhid → 命中
        mk("ven", 0xFF21, 0x0001),   // vendor 页 → 非 rawhid
    };
    const auto results = suspect_rawhid_interfaces(ifaces,
                                                   [](const std::string&) { return RAWHID_DESC; },
                                                   fake_factory());
    REQUIRE(results.size() == 1);
    CHECK(results[0].usage == 0x0000);
    REQUIRE(results[0].note.has_value());
    CHECK(results[0].note->find("rawhid") != std::string::npos);
    CHECK(results[0].reads.count("input_passive") == 1);
}

TEST_CASE("suspect_rawhid 拒绝非 64 字节形态") {
    // usage 0 但描述符是 8 字节小报文 → 不算 rawhid
    const Bytes small = bhex("05 01 09 00 A1 01 09 01 15 00 26 FF 00 75 08 95 02 81 02 C0");
    const std::vector<DeviceInfo> ifaces = {mk("raw", 0x0001, 0x0000)};
    const auto results = suspect_rawhid_interfaces(ifaces,
                                                   [&](const std::string&) { return small; },
                                                   fake_factory());
    CHECK(results.empty());
}

TEST_CASE("ProbeResult 默认值") {
    ProbeResult r;
    r.usage_page = 0xFF00;
    r.usage = 1;
    CHECK(r.reads.empty());
    CHECK_FALSE(r.error.has_value());
}

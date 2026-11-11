// test_vendor_battery.cpp — M5b 单元测试（tests/test_vendor_battery.py 平移，脚本化应答）。
#include <algorithm>

#include "doctest.h"
#include "core/vendor_battery.hpp"
#include "test_report_io.hpp"

using namespace pt;
using pt_test::Bytes;
using pt_test::FakeDev;
using pt_test::HidDevice;
using pt_test::ReportIO;

// 按最近一次写请求动态应答的假设备（前缀规则表，不消耗）。
class ReplyDev : public FakeDev {
public:
    ReplyDev(std::vector<std::pair<Bytes, Bytes>> rules) : rules_(std::move(rules)) {}

    Bytes read(int length, int) override {
        if (!written.empty()) {
            const Bytes& last = written.back();
            for (const auto& [prefix, reply] : rules_) {
                if (last.size() >= prefix.size()
                    && std::equal(prefix.begin(), prefix.end(), last.begin()))
                    return Bytes(reply.begin(),
                                 reply.begin() + std::min<std::size_t>(length, reply.size()));
            }
        }
        return {};
    }

private:
    std::vector<std::pair<Bytes, Bytes>> rules_;
};

// 安装 ReplyDev（所有权移交 ReportIO）
static ReplyDev* install_reply(ReportIO& io, std::vector<std::pair<Bytes, Bytes>> rules) {
    ReplyDev* dev = new ReplyDev(std::move(rules));
    io = ReportIO("x", [dev](const std::string&) {
        return std::unique_ptr<HidDevice>(dev);
    });
    return dev;
}

// ---------------------------------------------------------------------------
// ATK
// ---------------------------------------------------------------------------

TEST_CASE("atk1 校验和满足 0x55 约束") {
    Bytes cmd(16, 0x00);
    cmd[0] = 0x04;
    const int ck = atk1_checksum(0x08, cmd);
    Bytes full{0x08};
    full.insert(full.end(), cmd.begin(), cmd.begin() + 15);
    full.push_back(static_cast<std::uint8_t>(ck));
    int sum = full[16];
    for (std::size_t i = 0; i < 16; ++i) sum += full[i];
    CHECK((sum & 0xFF) == 0x55);  // 整条 17B 满足 sum+ck ≡ 0x55
}

TEST_CASE("read_atk ATK2 应答") {
    ReportIO io("x");
    install_reply(io, {{Bytes{0x08, 0x7D}, Bytes{0x08, 0x72, 0, 0, 0, 0x07, 0, 66}}});
    const auto result = read_atk(io, 0.3, 1);
    REQUIRE(result.has_value());
    CHECK(result->percent == 66);
    CHECK(result->source == "atk2");
    CHECK(result->detail.find("ATK2") != std::string::npos);
}

TEST_CASE("read_atk ATK1 应答") {
    // 构造一条合法 ATK1 应答: rep[0]=08, rep[1]=04, 电量@[6], 校验@[16]
    Bytes body(17, 0);
    body[0] = 0x08;
    body[1] = 0x04;
    body[6] = 0x55;
    int sum = body[16];
    for (std::size_t i = 0; i < 16; ++i) sum += body[i];
    body[16] = static_cast<std::uint8_t>((0x55 - (sum & 0xFF)) & 0xFF);
    CHECK(atk1_valid(body));
    ReportIO io("x");
    install_reply(io, {{Bytes{0x08, 0x04}, body}});
    const auto result = read_atk(io, 0.3, 1);
    REQUIRE(result.has_value());
    CHECK(result->percent == 85);  // 电量字节 0x55=85
    CHECK(result->source == "atk1");
}

TEST_CASE("read_atk 无应答返回空") {
    ReportIO io("x", [](const std::string&) {
        return std::unique_ptr<HidDevice>(new FakeDev());
    });
    CHECK_FALSE(read_atk(io, 0.2, 1).has_value());
}

// ---------------------------------------------------------------------------
// MCHOSE
// ---------------------------------------------------------------------------

TEST_CASE("read_mchose 应答") {
    ReportIO io("x");
    FakeDev* dev = install_reply(io, {{Bytes{0x55, 0x65}, Bytes{0x55, 0x65, 78, 0x01}}});
    const auto result = read_mchose(io, 0.3);
    REQUIRE(result.has_value());
    CHECK(result->percent == 78);
    CHECK(result->source == "mchose");
    REQUIRE_FALSE(dev->written.empty());
    CHECK(Bytes(dev->written[0].begin(), dev->written[0].begin() + 3)
          == Bytes{0x55, 0x65, 0x01});  // 请求帧正确
    CHECK(dev->written[0].size() == 64);  // 64 字节填充
}

TEST_CASE("read_mchose 电量字节超 100 视为无效") {
    // 应答电量字节 >100 → 视为无效继续等，超时返回 None
    ReportIO io("x");
    install_reply(io, {{Bytes{0x55, 0x65}, Bytes{0x55, 0x65, 250, 0x00}}});
    CHECK_FALSE(read_mchose(io, 0.2).has_value());
}

// ---------------------------------------------------------------------------
// 杰理 JL
// ---------------------------------------------------------------------------

TEST_CASE("jl 帧往返") {
    const Bytes frame = build_jl_frame(0, Bytes{0x01, 0x00}, 0x55, 64, false);
    // 帧布局(无 0x00 头): [0x55] FE seq 10 00 02 00 00 lenLo lenHi
    CHECK(frame[0] == 0x55);
    CHECK(frame[1] == 0xFE);
    CHECK(frame[3] == 0x10);
    CHECK(frame[5] == 0x02);
    // 应答: 合法 0xFE 帧 + TLV [0x01, 0x01, 64]
    const Bytes inner = {0x01, 0x01, 64};
    const Bytes reply = build_jl_frame(0, inner, 0x00, 64, true);
    ReportIO io("x");
    install_reply(io, {{Bytes{0x55, 0xFE}, reply}});
    const auto result = read_jl(io, 0.4);
    REQUIRE(result.has_value());
    CHECK(result->percent == 64);
    CHECK(result->source == "jl");
}

// ---------------------------------------------------------------------------
// 编排路由
// ---------------------------------------------------------------------------

TEST_CASE("try_vendor 按 VID 路由") {
    ReportIO io("x");
    install_reply(io, {{Bytes{0x08, 0x7D}, Bytes{0x08, 0x72, 0, 0, 0, 0x07, 0, 80}}});
    const auto result = try_vendor_battery("373B", io);  // ATK VID → ATK2 命中
    REQUIRE(result.has_value());
    CHECK(result->percent == 80);
}

TEST_CASE("try_vendor 未知 VID 走杰理兜底") {
    const Bytes inner = {0x01, 0x01, 40};
    const Bytes reply = build_jl_frame(0, inner, 0x00, 64, true);
    ReportIO io("x");
    install_reply(io, {{Bytes{0x00, 0x00, 0xFE}, reply}});  // rid=0 + 0x00 头 + FE
    const auto result = try_vendor_battery("ABCD", io);     // 未知 VID → JL 兜底
    REQUIRE(result.has_value());
    CHECK(result->percent == 40);
    CHECK(result->source == "jl");
}

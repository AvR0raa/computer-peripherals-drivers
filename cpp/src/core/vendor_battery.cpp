// vendor_battery.cpp — 厂商电量协议实现（core/vendor_battery.py 平移）。
#include "vendor_battery.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace pt {

namespace {

int clamp_pct(int v) { return std::clamp(v, 0, 100); }

// 轮询辅助：deadline 前反复 read_input(250ms)；假设备立即返回，加 5ms 睡眠防空转。
template <typename Fn>
void poll_until(double timeout_seconds, Fn&& on_reply) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(timeout_seconds));
    while (std::chrono::steady_clock::now() < deadline) {
        if (on_reply()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// ATK（前行者）
// ---------------------------------------------------------------------------

int atk1_checksum(int report_id, const Bytes& cmd) {
    int total = report_id;
    for (std::size_t i = 0; i < 15 && i < cmd.size(); ++i) total += cmd[i];
    return (0x55 - (total & 0xFF)) & 0xFF;
}

bool atk1_valid(const Bytes& rep) {
    if (rep.size() < 17 || rep[0] != 0x08 || rep[1] != 0x04) return false;
    int sum = rep[16];
    for (std::size_t i = 0; i < 16; ++i) sum += rep[i];
    return (sum & 0xFF) == 0x55;
}

std::optional<VendorBattery> read_atk(ReportIO& io, double timeout, int attempts) {
    // ATK2: 64 字节接口，请求 [0x08, 7D 72 02 00 01 07 01, 0 填充]
    static const Bytes ATK2_PAYLOAD = {0x7D, 0x72, 0x02, 0x00, 0x01, 0x07, 0x01};
    Bytes req2{0x08};
    req2.insert(req2.end(), ATK2_PAYLOAD.begin(), ATK2_PAYLOAD.end());
    req2.resize(64, 0x00);
    for (int attempt = 0; attempt < attempts; ++attempt) {
        try {
            io.write_output(req2);
        } catch (...) {
            break;  // 接口不接受 64B 输出 → 跳过 ATK2
        }
        std::optional<Bytes> hit;
        poll_until(timeout, [&]() -> bool {
            const auto rep = io.read_input(250);
            if (rep && rep->size() > 7 && (*rep)[1] == 0x72 && (*rep)[5] == 0x07) {
                hit = *rep;
                return true;
            }
            return false;
        });
        if (hit) {
            VendorBattery vb;
            vb.percent = clamp_pct((*hit)[7]);
            vb.source = "atk2";
            const std::size_t n = std::min<std::size_t>(12, hit->size());
            vb.detail = "ATK2 应答 " + to_hex(Bytes(hit->begin(), hit->begin() + n));
            return vb;
        }
    }
    // ATK1: 17 字节接口，请求 [0x08, 04, 0x00×14, 校验]
    Bytes cmd(16, 0x00);
    cmd[0] = 0x04;
    cmd[15] = static_cast<std::uint8_t>(atk1_checksum(0x08, cmd));
    Bytes req1{0x08};
    req1.insert(req1.end(), cmd.begin(), cmd.end());
    for (int attempt = 0; attempt < attempts; ++attempt) {
        try {
            io.write_output(req1);
        } catch (...) {
            return std::nullopt;
        }
        std::optional<Bytes> hit;
        poll_until(timeout, [&]() -> bool {
            const auto rep = io.read_input(250);
            if (rep && atk1_valid(*rep)) {
                hit = *rep;
                return true;
            }
            return false;
        });
        if (hit) {
            VendorBattery vb;
            vb.percent = clamp_pct((*hit)[6]);
            vb.source = "atk1";
            char hexbuf[4];
            std::snprintf(hexbuf, sizeof hexbuf, "%02X", (*hit)[6]);
            vb.detail = std::string("ATK1 电量字节=0x") + hexbuf;
            return vb;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// MCHOSE（迈从）
// ---------------------------------------------------------------------------

std::optional<VendorBattery> read_mchose(ReportIO& io, double timeout) {
    Bytes req{0x55, 0x65, 0x01};
    req.resize(64, 0x00);  // 凑 64 字节
    try {
        io.write_output(req);
    } catch (...) {
        return std::nullopt;
    }
    std::optional<VendorBattery> result;
    poll_until(timeout, [&]() -> bool {
        const auto rep = io.read_input(250);
        if (rep && rep->size() >= 3 && (*rep)[0] == 0x55 && (*rep)[1] == 0x65 && (*rep)[2] <= 100) {
            VendorBattery vb;
            vb.percent = clamp_pct((*rep)[2]);
            vb.source = "mchose";
            const std::size_t n = std::min<std::size_t>(8, rep->size());
            vb.detail = "应答 " + to_hex(Bytes(rep->begin(), rep->begin() + n));
            result = vb;
            return true;
        }
        return false;
    });
    return result;
}

// ---------------------------------------------------------------------------
// 杰理 JL_RCSP（通用兜底）
// ---------------------------------------------------------------------------

int crc16_ccitt(const Bytes& data) {
    int crc = 0xFFFF;
    for (std::uint8_t byte : data) {
        crc ^= byte << 8;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xFFFF : (crc << 1) & 0xFFFF;
    }
    return crc;
}

Bytes build_jl_frame(int seq, const Bytes& payload, int report_id, int total, bool include_zero) {
    Bytes f(static_cast<std::size_t>(total), 0);
    int base = 1;
    f[0] = static_cast<std::uint8_t>(report_id);
    if (include_zero) {
        f[static_cast<std::size_t>(base)] = 0x00;
        base += 1;
    }
    const int len = static_cast<int>(payload.size());
    const std::uint8_t header[9] = {0xFE, static_cast<std::uint8_t>(seq & 0xFF), 0x10, 0x00,
                                    0x02, 0x00, 0x00,
                                    static_cast<std::uint8_t>(len & 0xFF),
                                    static_cast<std::uint8_t>((len >> 8) & 0xFF)};
    for (int i = 0; i < 9; ++i)
        f[static_cast<std::size_t>(base + i)] = header[i];
    for (std::size_t i = 0; i < payload.size(); ++i)
        f[static_cast<std::size_t>(base + 9 + i)] = payload[i];
    const Bytes crc_src(f.begin() + base, f.begin() + base + 9 + len);
    const int crc = crc16_ccitt(crc_src);
    f[static_cast<std::size_t>(base + 9 + len)] = static_cast<std::uint8_t>((crc >> 8) & 0xFF);
    f[static_cast<std::size_t>(base + 10 + len)] = static_cast<std::uint8_t>(crc & 0xFF);
    return f;
}

std::optional<Bytes> parse_jl(const Bytes& report) {
    // 在报文前 4 字节内找 0xFE 帧头，返回 CRC 通过的 payload
    for (int start = 0; start < 4; ++start) {
        if (start + 12 > static_cast<int>(report.size()) || report[static_cast<std::size_t>(start)] != 0xFE)
            continue;
        const int ln = report[static_cast<std::size_t>(start + 7)]
                       | (report[static_cast<std::size_t>(start + 8)] << 8);
        if (ln > 60 || start + 9 + ln + 2 > static_cast<int>(report.size())) continue;
        Bytes payload(report.begin() + start + 9, report.begin() + start + 9 + ln);
        const int crc = (report[static_cast<std::size_t>(start + 9 + ln)] << 8)
                        | report[static_cast<std::size_t>(start + 10 + ln)];
        const Bytes crc_src(report.begin() + start, report.begin() + start + 9 + ln);
        if (crc == crc16_ccitt(crc_src))
            return payload;
    }
    return std::nullopt;
}

std::optional<VendorBattery> read_jl(ReportIO& io, double seconds) {
    // 杰理 TLV 电量：Report ID 0x55（无 0x00 头）/ 0x00（带 0x00 头）两种帧各试
    const Bytes payload = {0x01, 0x00};
    for (auto [rid, include_zero] : {std::pair<int, bool>{0x55, false}, {0x00, true}}) {
        try {
            io.write_output(build_jl_frame(0, payload, rid, 64, include_zero));
        } catch (...) {
            continue;
        }
        std::optional<VendorBattery> result;
        poll_until(seconds, [&]() -> bool {
            const auto rep = io.read_input(200);
            if (!rep) return false;
            const auto parsed = parse_jl(*rep);
            if (!parsed) return false;
            std::size_t i = 0;
            while (i + 2 <= parsed->size()) {
                const int tag = (*parsed)[i];
                const int ln = (*parsed)[i + 1];
                if (tag == 0x01 && ln >= 1 && i + 2 < parsed->size()) {
                    char ridbuf[4];
                    std::snprintf(ridbuf, sizeof ridbuf, "%02X", rid);
                    VendorBattery vb;
                    vb.percent = clamp_pct((*parsed)[i + 2]);
                    vb.source = "jl";
                    vb.detail = std::string("杰理 TLV 0x01, ReportID=0x") + ridbuf;
                    result = vb;
                    return true;
                }
                i += static_cast<std::size_t>(2 + ln);
            }
            return false;
        });
        if (result) return result;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 编排
// ---------------------------------------------------------------------------

std::optional<VendorBattery> try_vendor_battery(const std::string& vid_hex, ReportIO& io) {
    if (ATK_VIDS.count(vid_hex)) {
        if (auto result = read_atk(io)) return result;
    }
    if (MCHOSE_VIDS.count(vid_hex)) {
        if (auto result = read_mchose(io)) return result;
    }
    return read_jl(io);
}

}  // namespace pt

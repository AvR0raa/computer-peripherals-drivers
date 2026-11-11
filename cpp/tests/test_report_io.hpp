// test_report_io.hpp — 测试共享：假设备与安装辅助（test_report_io / test_safe_write 共用）。
// 所有权约定：设备对象在 io.open() 时移交 ReportIO；close 后不得解引用裸指针。
#pragma once
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/json_util.hpp"
#include "core/report_io.hpp"

namespace pt_test {

using pt::Bytes;
using pt::HidDevice;
using pt::Json;
using pt::Params;
using pt::ReportIO;

class FakeDev : public HidDevice {
public:
    explicit FakeDev(std::vector<Bytes> responses = {})
        : responses_(std::make_shared<std::vector<Bytes>>(std::move(responses))) {}

    // 共享脚本队列构造：双通道场景下每条 ReportIO 各持一个 FakeDev、读同一个队列
    //（对应 Python monkeypatch 单假设备全局共享的语义）。
    explicit FakeDev(std::shared_ptr<std::vector<Bytes>> responses)
        : responses_(std::move(responses)) {}

    int write(const Bytes& data) override {
        written.push_back(data);
        return static_cast<int>(data.size());
    }
    Bytes read(int, int) override {
        if (responses_->empty()) return {};
        Bytes out = responses_->front();
        responses_->erase(responses_->begin());
        return out;
    }
    int send_feature(const Bytes& data) override {
        features_sent.push_back(data);
        return static_cast<int>(data.size());
    }
    Bytes get_feature(int report_id, int size) override {
        Bytes out{static_cast<std::uint8_t>(report_id)};
        for (int i = 1; i < size; ++i) out.push_back(0xAA);
        return out;
    }
    void close() override { closed = true; }

    std::vector<Bytes> written;
    std::vector<Bytes> features_sent;
    bool closed = false;

private:
    std::shared_ptr<std::vector<Bytes>> responses_;
};

// 安装假设备：open 时所有权移交 ReportIO。
inline FakeDev* install_fake(ReportIO& io, std::vector<Bytes> responses = {}) {
    FakeDev* dev = new FakeDev(std::move(responses));
    io = ReportIO("fake", [dev](const std::string&) {
        return std::unique_ptr<HidDevice>(dev);
    });
    return dev;
}

// 工厂直接抛异常（打开失败场景）。
inline void install_boom(ReportIO& io) {
    io = ReportIO("ghost", [](const std::string&) -> std::unique_ptr<HidDevice> {
        throw std::runtime_error("no such device");
    });
}

// feature report 读走脚本队列（FakeDev 的 get_feature 是固定填充）。
class ScriptedDev : public FakeDev {
public:
    ScriptedDev(std::vector<Bytes> responses = {}, std::vector<Bytes> feature_reads = {})
        : FakeDev(std::move(responses)), feature_reads_(std::move(feature_reads)) {}

    Bytes get_feature(int report_id, int size) override {
        if (!feature_reads_.empty()) {
            Bytes out = feature_reads_.front();
            feature_reads_.erase(feature_reads_.begin());
            return out;
        }
        Bytes out{static_cast<std::uint8_t>(report_id)};
        out.insert(out.end(), static_cast<std::size_t>(size) - 1, 0x00);
        return out;
    }

private:
    std::vector<Bytes> feature_reads_;
};

inline ScriptedDev* install_scripted(ReportIO& io, std::vector<Bytes> responses = {},
                                     std::vector<Bytes> feature_reads = {}) {
    ScriptedDev* dev = new ScriptedDev(std::move(responses), std::move(feature_reads));
    io = ReportIO("fake", [dev](const std::string&) {
        return std::unique_ptr<HidDevice>(dev);
    });
    return dev;
}

inline Bytes b(std::initializer_list<std::uint8_t> list) { return Bytes(list); }

// "05 01 09 02 ..." → 字节流
inline Bytes bhex(const char* s) {
    Bytes out;
    for (const char* p = s; *p;) {
        if (*p == ' ') { ++p; continue; }
        char tmp[3] = {p[0], p[1], '\0'};
        out.push_back(static_cast<std::uint8_t>(std::strtoul(tmp, nullptr, 16)));
        p += 2;
    }
    return out;
}

inline Json parse(const char* s) { return Json::parse(s); }

}  // namespace pt_test

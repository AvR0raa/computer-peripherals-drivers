// test_recorder.cpp — 任务 4.1 单元测试（tests/test_recorder.py 平移，假设备 + 真线程）。
#include <chrono>
#include <fstream>
#include <memory>
#include <set>
#include <thread>

#include "doctest.h"
#include "core/recorder.hpp"
#include "test_report_io.hpp"

using namespace pt;
using pt_test::b;
using pt_test::Bytes;
using pt_test::FakeDev;
using pt_test::HidDevice;

// 轮询等待条件成立（对应 Python wait_until）
template <typename Pred>
static bool wait_until(Pred&& pred, double timeout = 3.0) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                std::chrono::duration<double>(timeout));
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// read 抛异常的假设备（模拟设备拔出/接口死亡）
class DyingDev : public FakeDev {
public:
    using FakeDev::FakeDev;
    Bytes read(int, int) override { throw std::runtime_error("device gone"); }
};

// get_feature 恒返回空的假设备（无响应快照 → data=""）
class EmptyFeatureDev : public FakeDev {
public:
    Bytes get_feature(int, int) override { return {}; }
};

TEST_CASE("recorder 捕获 input 记录") {
    auto queue = std::make_shared<std::vector<Bytes>>(
        std::vector<Bytes>{b({0xED, 1, 2}), b({0xED, 3, 4})});
    Recorder rec({RecorderTarget{"FF21", "\\\\?\\x", {}}}, 50, RECORDER_MAX_ENTRIES,
                 [queue](const std::string&) {
                     return std::unique_ptr<HidDevice>(new FakeDev(queue));
                 });
    rec.start();
    const bool ok = wait_until([&] { return rec.entries().size() >= 2; });
    rec.stop();
    REQUIRE(ok);
    std::vector<CaptureEntry> inputs;
    for (const auto& e : rec.entries())
        if (e.kind == "input") inputs.push_back(e);
    REQUIRE(inputs.size() == 2);
    CHECK(inputs[0].data == "ed 01 02");
    CHECK(inputs[1].data == "ed 03 04");
    CHECK(inputs[0].report_id == 0xED);
    CHECK(inputs[0].ts > 0);
    CHECK(inputs[1].ts >= inputs[0].ts);
}

TEST_CASE("起始快照 + 手动快照") {
    auto queue = std::make_shared<std::vector<Bytes>>();
    Recorder rec({RecorderTarget{"FF21", "\\\\?\\x", {237}}}, 50, RECORDER_MAX_ENTRIES,
                 [queue](const std::string&) {
                     return std::unique_ptr<HidDevice>(new FakeDev(queue));
                 });
    rec.start();
    wait_until([&] {
        for (const auto& e : rec.entries())
            if (e.kind == "feature_read") return true;
        return false;
    });
    const int before = rec.snapshot_features();
    rec.stop();
    std::vector<CaptureEntry> starts;
    for (const auto& e : rec.entries())
        if (e.kind == "feature_read") starts.push_back(e);
    CHECK(starts.size() >= 2);  // 起始 1 次 + 手动 1 次
    CHECK(before == 1);
    REQUIRE_FALSE(starts.empty());
    CHECK(starts[0].report_id == 237);
    CHECK(starts[0].data.rfind("ed aa aa", 0) == 0);
}

TEST_CASE("feature 无响应存空串") {
    // get_feature 返回空 → feature_read 记录 data=""
    Recorder rec({RecorderTarget{"FF22", "\\\\?\\x", {170}}}, 50, RECORDER_MAX_ENTRIES,
                 [](const std::string&) {
                     return std::unique_ptr<HidDevice>(new EmptyFeatureDev());
                 });
    const int count = rec.snapshot_features();
    CHECK(count == 1);
    REQUIRE_FALSE(rec.entries().empty());
    CHECK(rec.entries()[0].kind == "feature_read");
    CHECK(rec.entries()[0].data == "");
}

TEST_CASE("读取异常终止目标并记录 error") {
    Recorder rec({RecorderTarget{"raw", "\\\\?\\x", {}}}, 50, RECORDER_MAX_ENTRIES,
                 [](const std::string&) {
                     return std::unique_ptr<HidDevice>(new DyingDev());
                 });
    rec.start();
    const bool ok = wait_until([&] {
        for (const auto& e : rec.entries())
            if (e.kind == "error") return true;
        return false;
    });
    rec.stop();
    CHECK(ok);
    CHECK(rec.entries().back().data.find("input 读取异常") != std::string::npos);
}

TEST_CASE("打开失败记录 error") {
    Recorder rec({RecorderTarget{"ghost", "\\\\?\\none", {}}}, 50, RECORDER_MAX_ENTRIES,
                 [](const std::string&) -> std::unique_ptr<HidDevice> {
                     throw std::runtime_error("no such device");
                 });
    rec.start();
    const bool ok = wait_until([&] {
        for (const auto& e : rec.entries())
            if (e.kind == "error") return true;
        return false;
    });
    rec.stop();
    CHECK(ok);
    CHECK(rec.entries().at(0).data.find("打开失败") != std::string::npos);
}

TEST_CASE("start 两次被拒") {
    Recorder rec({RecorderTarget{"x", "\\\\?\\x", {}}}, 50, RECORDER_MAX_ENTRIES,
                 [](const std::string&) {
                     return std::unique_ptr<HidDevice>(new FakeDev());
                 });
    rec.start();
    CHECK_THROWS_AS(rec.start(), const std::runtime_error&);
    rec.stop();
}

TEST_CASE("stop 终止线程") {
    Recorder rec({RecorderTarget{"x", "\\\\?\\x", {}}}, 50, RECORDER_MAX_ENTRIES,
                 [](const std::string&) {
                     return std::unique_ptr<HidDevice>(new FakeDev());
                 });
    rec.start();
    CHECK(rec.is_recording());
    rec.stop();
    CHECK_FALSE(rec.is_recording());
}

TEST_CASE("jsonl 往返一致") {
    auto queue = std::make_shared<std::vector<Bytes>>(std::vector<Bytes>{b({0xED, 1, 2})});
    Recorder rec({RecorderTarget{"FF21", "\\\\?\\x", {}}}, 50, RECORDER_MAX_ENTRIES,
                 [queue](const std::string&) {
                     return std::unique_ptr<HidDevice>(new FakeDev(queue));
                 });
    rec.start();
    wait_until([&] { return !rec.entries().empty(); });
    rec.stop();

    const auto path = std::filesystem::temp_directory_path() / "pt_recorder_session.jsonl";
    const int written = rec.save_jsonl(path);
    const auto loaded = Recorder::load_jsonl(path);
    CHECK(written == static_cast<int>(rec.entries().size()));
    CHECK(static_cast<int>(loaded.size()) == written);
    REQUIRE_FALSE(loaded.empty());
    CHECK(loaded[0].data == rec.entries()[0].data);
    CHECK(loaded[0].kind == "input");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("jsonl 空行容忍") {
    const auto path = std::filesystem::temp_directory_path() / "pt_recorder_empty.jsonl";
    {
        std::ofstream f(path, std::ios::binary);
        f << "\n{\"ts\": 1.0, \"source\": \"a\", \"kind\": \"input\", \"report_id\": 5, \"data\": \"aa\"}\n\n";
    }
    const auto loaded = Recorder::load_jsonl(path);
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].report_id == 5);
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("超出 max_entries 溢出停录") {
    auto queue = std::make_shared<std::vector<Bytes>>(std::vector<Bytes>(50, Bytes{1, 2}));
    Recorder rec({RecorderTarget{"x", "\\\\?\\x", {}}}, 50, 3,
                 [queue](const std::string&) {
                     return std::unique_ptr<HidDevice>(new FakeDev(queue));
                 });
    rec.start();
    wait_until([&] { return rec.overflowed(); });
    rec.stop();
    CHECK(rec.overflowed());
    CHECK(rec.entries().size() == 3);
}

TEST_CASE("多目标隔离") {
    // 每个目标一个独立假设备（工厂按 path 分发），label 区分来源
    auto queue_a = std::make_shared<std::vector<Bytes>>(std::vector<Bytes>{b({0xED, 1})});
    auto queue_b = std::make_shared<std::vector<Bytes>>(std::vector<Bytes>{b({0xEE, 2})});
    Recorder rec({RecorderTarget{"FF21", "\\\\?\\a", {}}, RecorderTarget{"rawhid", "\\\\?\\b", {}}},
                 50, RECORDER_MAX_ENTRIES, [&](const std::string& path) {
                     return std::unique_ptr<HidDevice>(
                         new FakeDev(path.find('a') != std::string::npos ? queue_a : queue_b));
                 });
    rec.start();
    wait_until([&] {
        int inputs = 0;
        for (const auto& e : rec.entries())
            if (e.kind == "input") ++inputs;
        return inputs >= 2;
    });
    rec.stop();
    std::set<std::string> sources;
    for (const auto& e : rec.entries())
        if (e.kind == "input") sources.insert(e.source);
    CHECK(sources == std::set<std::string>{"FF21", "rawhid"});
}

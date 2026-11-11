// test_replay.cpp — 任务 4.5 单元测试（假设备 + 真线程）。
// 所有权约定：open 后假设备归 ReportIO，stop 后已析构 —— 断言用共享指针观测缓冲
//（RecordingDev），与 test_report_io.hpp 的共享脚本队列同一思路。
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include "doctest.h"
#include "core/replay.hpp"
#include "test_report_io.hpp"

using namespace pt;
using pt_test::b;
using pt_test::bhex;
using pt_test::Bytes;
using pt_test::FakeDev;

// 轮询等待条件成立（与 test_recorder 同款）
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

using LogPtr = std::shared_ptr<std::vector<Bytes>>;

// 写入/feature 落共享观测缓冲的假设备（设备析构后断言仍可读）
class RecordingDev : public FakeDev {
public:
    RecordingDev(LogPtr written_log, LogPtr features_log,
                 std::shared_ptr<std::vector<Bytes>> responses = {})
        : FakeDev(std::move(responses)), written_log_(std::move(written_log)),
          features_log_(std::move(features_log)) {}

    int write(const Bytes& data) override {
        written_log_->push_back(data);
        return static_cast<int>(data.size());
    }
    int send_feature(const Bytes& data) override {
        features_log_->push_back(data);
        return static_cast<int>(data.size());
    }

private:
    LogPtr written_log_;
    LogPtr features_log_;
};

// 第 2 次 write 起抛异常的假设备（模拟端点假死）
class DyingWriteDev : public FakeDev {
public:
    using FakeDev::FakeDev;
    int write(const Bytes& data) override {
        if (++writes_ >= 2) throw std::runtime_error("endpoint stalled");
        written.push_back(data);
        return static_cast<int>(data.size());
    }

private:
    int writes_ = 0;
};

// 带 RecordingDev 的回放器（log 缓冲由调用方持有）
static Replayer make_player(LogPtr written_log, LogPtr features_log,
                            std::shared_ptr<std::vector<Bytes>> responses = {}) {
    return Replayer("\\\\?\\x",
                    [=](const std::string&) -> std::unique_ptr<HidDevice> {
                        return std::unique_ptr<HidDevice>(
                            new RecordingDev(written_log, features_log, responses));
                    });
}

TEST_CASE("action_from_capture 回放映射") {
    SUBCASE("input → output") {
        const auto action =
            action_from_capture(CaptureEntry{1.0, "rawhid", "input", 0xED, "ed 01 02"});
        REQUIRE(action.has_value());
        CHECK(action->mode == "output");
        CHECK(action->report_id == 0xED);
        CHECK(action->data_hex == "ed 01 02");
        CHECK(action->origin.find("input") != std::string::npos);
    }
    SUBCASE("feature_read → feature") {
        const auto action =
            action_from_capture(CaptureEntry{1.0, "FF21", "feature_read", 170, "aa 01 02"});
        REQUIRE(action.has_value());
        CHECK(action->mode == "feature");
        CHECK(action->report_id == 170);
        CHECK(action->data_hex == "aa 01 02");
    }
    SUBCASE("error/空数据/未知 kind 不可回放") {
        CHECK_FALSE(action_from_capture(CaptureEntry{1.0, "x", "error", 0, "打开失败"}).has_value());
        CHECK_FALSE(action_from_capture(CaptureEntry{1.0, "x", "input", 5, ""}).has_value());
        CHECK_FALSE(action_from_capture(CaptureEntry{1.0, "x", "mystery", 5, "aa"}).has_value());
    }
    SUBCASE("自定义 origin") {
        const auto action =
            action_from_capture(CaptureEntry{1.0, "x", "input", 1, "01"}, "input#3");
        REQUIRE(action.has_value());
        CHECK(action->origin == "input#3");
    }
}

TEST_CASE("output 回放写报文并捕获响应") {
    auto written_log = std::make_shared<std::vector<Bytes>>();
    auto features_log = std::make_shared<std::vector<Bytes>>();
    auto queue = std::make_shared<std::vector<Bytes>>(
        std::vector<Bytes>{b({0x81, 0xAA}), b({0x82, 0xBB})});
    Replayer player = make_player(written_log, features_log, queue);
    player.set_actions(std::vector<ReplayAction>{
        {"output", 0x01, "01 02 03", "input#1"},
        {"output", 0x02, "02 04 06", "input#2"}});
    player.set_min_interval_ms(0);
    player.set_response_window_ms(50);
    player.start();
    const bool finished = wait_until([&] {
        for (const auto& e : player.events())
            if (e.kind == "done") return true;
        return false;
    });
    player.stop();
    REQUIRE(finished);
    REQUIRE(written_log->size() == 2);
    CHECK((*written_log)[0] == bhex("01 02 03"));
    CHECK((*written_log)[1] == bhex("02 04 06"));
    CHECK(features_log->empty());
    CHECK(player.sent_count() == 2);
    CHECK(player.aborted() == false);

    int writes = 0, responses = 0;
    for (const auto& e : player.events()) {
        if (e.kind == "write") {
            ++writes;
            CHECK(e.ts > 0);
        }
        if (e.kind == "response") ++responses;
    }
    CHECK(writes == 2);
    CHECK(responses == 2);  // 观察窗口捕获两条队列响应
    CHECK(player.events().back().kind == "done");
}

TEST_CASE("feature 回放走 send_feature") {
    auto written_log = std::make_shared<std::vector<Bytes>>();
    auto features_log = std::make_shared<std::vector<Bytes>>();
    Replayer player = make_player(written_log, features_log);
    player.set_actions(
        std::vector<ReplayAction>{{"feature", 0xAA, "aa 55 0f", "feature_read id=0xAA"}});
    player.set_min_interval_ms(0);
    player.set_response_window_ms(0);
    player.start();
    wait_until([&] { return player.sent_count() >= 1; });
    player.stop();
    REQUIRE(features_log->size() == 1);
    CHECK((*features_log)[0] == bhex("aa 55 0f"));
    CHECK(written_log->empty());  // output 通道零写入
}

TEST_CASE("限速：相邻写间隔不小于 min_interval") {
    auto written_log = std::make_shared<std::vector<Bytes>>();
    auto features_log = std::make_shared<std::vector<Bytes>>();
    Replayer player = make_player(written_log, features_log);
    player.set_actions(std::vector<ReplayAction>{
        {"output", 0, "01", "a"}, {"output", 0, "02", "b"}, {"output", 0, "03", "c"}});
    player.set_min_interval_ms(60);
    player.set_response_window_ms(0);
    const auto t0 = std::chrono::steady_clock::now();
    player.start();
    const bool finished = wait_until([&] {
        for (const auto& e : player.events())
            if (e.kind == "done") return true;
        return false;
    });
    player.stop();
    REQUIRE(finished);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(player.sent_count() == 3);
    CHECK(written_log->size() == 3);
    CHECK(elapsed >= 0.12);  // 2 个间隔 × 60ms（首写不限速）
    CHECK(elapsed < 2.0);    // 上界只防呆
}

TEST_CASE("全程可中止：stop 后不再写、标记 aborted") {
    auto written_log = std::make_shared<std::vector<Bytes>>();
    auto features_log = std::make_shared<std::vector<Bytes>>();
    Replayer player = make_player(written_log, features_log);
    std::vector<ReplayAction> actions;
    for (int i = 0; i < 20; ++i)
        actions.push_back(ReplayAction{"output", 0, "0f", "a"});
    player.set_actions(std::move(actions));
    player.set_min_interval_ms(50);
    player.set_response_window_ms(0);
    player.start();
    REQUIRE(wait_until([&] { return player.sent_count() >= 1; }));
    player.stop();
    CHECK(player.aborted());
    CHECK(player.sent_count() < 20);
    CHECK(written_log->size() == player.sent_count());
    REQUIRE_FALSE(player.events().empty());
    CHECK(player.events().back().kind == "aborted");
}

TEST_CASE("写异常终止回放") {
    DyingWriteDev* dev = nullptr;
    Replayer player("\\\\?\\x", [&](const std::string&) -> std::unique_ptr<HidDevice> {
        auto owned = std::make_unique<DyingWriteDev>();
        dev = owned.get();
        return owned;
    });
    player.set_actions(std::vector<ReplayAction>{
        {"output", 0, "01", "a"}, {"output", 0, "02", "b"}, {"output", 0, "03", "c"}});
    player.set_min_interval_ms(0);
    player.set_response_window_ms(0);
    player.start();
    const bool errored = wait_until([&] {
        for (const auto& e : player.events())
            if (e.kind == "error") return true;
        return false;
    });
    player.stop();
    REQUIRE(errored);
    CHECK(player.sent_count() == 1);
    bool has_done = false;
    for (const auto& e : player.events())
        if (e.kind == "done") has_done = true;
    CHECK_FALSE(has_done);  // 异常终止不产生 done
    for (const auto& e : player.events())
        if (e.kind == "error") CHECK(e.data.find("写入异常") != std::string::npos);
    (void)dev;  // 写入发生在 dev->written，仅事件断言（设备析构后不可读）
}

TEST_CASE("打开失败记录 error 且无 done") {
    Replayer player("\\\\?\\none", [](const std::string&) -> std::unique_ptr<HidDevice> {
        throw std::runtime_error("no such device");
    });
    player.set_actions(std::vector<ReplayAction>{{"output", 0, "01", "a"}});
    player.start();
    const bool errored = wait_until([&] {
        for (const auto& e : player.events())
            if (e.kind == "error") return true;
        return false;
    });
    player.stop();
    REQUIRE(errored);
    CHECK(player.events().at(0).data.find("打开失败") != std::string::npos);
}

TEST_CASE("空动作 start 被拒；start 两次被拒") {
    Replayer player("\\\\?\\x", [](const std::string&) {
        return std::unique_ptr<HidDevice>(new FakeDev());
    });
    player.set_actions({});
    CHECK_THROWS_AS(player.start(), const std::runtime_error&);

    player.set_actions(std::vector<ReplayAction>{{"output", 0, "01", "a"}});
    player.set_min_interval_ms(0);
    player.set_response_window_ms(0);
    player.start();
    CHECK_THROWS_AS(player.start(), const std::runtime_error&);
    player.stop();
    CHECK_FALSE(player.is_running());
}

TEST_CASE("录制条目端到端：input/feature 混合回放") {
    auto written_log = std::make_shared<std::vector<Bytes>>();
    auto features_log = std::make_shared<std::vector<Bytes>>();
    Replayer player = make_player(written_log, features_log);
    std::vector<CaptureEntry> session{
        CaptureEntry{1.0, "rawhid", "input", 0x00, "00 01 02"},
        CaptureEntry{2.0, "rawhid", "error", 0, "input 读取异常（目标终止）: x"},
        CaptureEntry{3.0, "rawhid", "feature_read", 0xAA, "aa 7f"},
    };
    std::vector<ReplayAction> actions;
    for (std::size_t i = 0; i < session.size(); ++i) {
        if (auto action = action_from_capture(session[i], "entry#" + std::to_string(i)))
            actions.push_back(std::move(*action));
    }
    REQUIRE(actions.size() == 2);  // error 条目被过滤
    player.set_actions(std::move(actions));
    player.set_min_interval_ms(0);
    player.set_response_window_ms(0);
    player.start();
    wait_until([&] { return player.sent_count() >= 2; });
    player.stop();
    REQUIRE(written_log->size() == 1);
    CHECK((*written_log)[0] == bhex("00 01 02"));
    REQUIRE(features_log->size() == 1);
    CHECK((*features_log)[0] == bhex("aa 7f"));
}

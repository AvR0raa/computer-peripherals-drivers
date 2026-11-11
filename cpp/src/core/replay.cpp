// replay.cpp — 学习模式写回放实现（任务 4.5）。
#include "replay.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace pt {

namespace {

Bytes hex_to_bytes(const std::string& data) {
    Bytes out;
    if (data.empty()) return out;
    std::string packed;
    packed.reserve(data.size());
    for (char c : data)
        if (c != ' ') packed.push_back(c);
    if (packed.size() % 2 != 0) return out;  // 非法 hex（录制数据不会出现）
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i + 1 < packed.size(); i += 2) {
        const int hi = nibble(packed[i]), lo = nibble(packed[i + 1]);
        if (hi < 0 || lo < 0) return Bytes(packed.size() / 2, 0);
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

double now_ts() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

std::optional<ReplayAction> action_from_capture(const CaptureEntry& entry,
                                                const std::string& origin) {
    if (entry.data.empty()) return std::nullopt;  // 无响应快照/空报文不可回放
    ReplayAction action;
    if (entry.kind == "input") {
        action.mode = "output";  // 写触发型协议："设备回声即命令"
        action.report_id = entry.report_id;
    } else if (entry.kind == "feature_read") {
        action.mode = "feature";  // 配置页快照回写
        action.report_id = entry.report_id;
    } else {
        return std::nullopt;  // error 条目（人话文本）不可回放
    }
    action.data_hex = entry.data;
    action.origin = origin.empty() ? entry.kind + " id=0x" + hex(entry.report_id, 2) : origin;
    return action;
}

Replayer::Replayer(std::string path, DevFactory factory)
    : path_(std::move(path)), factory_(std::move(factory)) {}

void Replayer::set_actions(std::vector<ReplayAction> actions) {
    std::lock_guard<std::mutex> lock(mutex_);
    actions_ = std::move(actions);
}

void Replayer::set_min_interval_ms(int ms) { min_interval_ms_ = ms < 0 ? 0 : ms; }
void Replayer::set_response_window_ms(int ms) { response_window_ms_ = ms < 0 ? 0 : ms; }

void Replayer::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) throw std::runtime_error("回放已在进行中");
    if (actions_.empty()) throw std::runtime_error("没有可回放的动作");
    stop_ = false;
    aborted_ = false;
    events_.clear();
    sent_ = 0;
    thread_ = std::thread([this] { run(); });
}

void Replayer::stop(int timeout_ms) {
    (void)timeout_ms;  // 回放循环以 5ms 步进检查 stop，join 至线程退出
    stop_ = true;
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker = std::move(thread_);  // 移出后再 join，不持锁等线程
    }
    if (worker.joinable()) worker.join();
}

bool Replayer::is_running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return thread_.joinable();
}

std::vector<ReplayEvent> Replayer::events() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
}

int Replayer::sent_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sent_;
}

void Replayer::add(const ReplayEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(event);
}

void Replayer::run() {
    const auto interval = std::chrono::milliseconds(min_interval_ms_);
    const auto window = std::chrono::milliseconds(response_window_ms_);
    try {
        ReportIO io(path_, factory_);
        io.open();
        auto last_write = std::chrono::steady_clock::now() - interval;  // 首写不限速
        for (const auto& action : actions_) {
            if (stop_) {
                aborted_ = true;
                break;
            }
            // 限速等待：分段睡眠，随时响应中止
            while (!stop_ && std::chrono::steady_clock::now() - last_write < interval)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (stop_) {
                aborted_ = true;
                break;
            }
            const Bytes data = hex_to_bytes(action.data_hex);
            if (data.empty()) {
                add({now_ts(), "error", action.report_id, "空报文，跳过: " + action.origin});
                continue;
            }
            try {
                if (action.mode == "feature") io.send_feature(data);
                else io.write_output(data);
            } catch (const std::exception& exc) {
                add({now_ts(), "error", action.report_id,
                     "写入异常（终止回放）: " + std::string(exc.what())});
                return;
            }
            last_write = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++sent_;
            }
            add({now_ts(), "write", action.report_id, to_hex(data)});
            // 观察窗口：持续读 input 直到窗口耗尽（短读步进，随时中止）
            const auto deadline = std::chrono::steady_clock::now() + window;
            while (!stop_) {
                const auto remain = deadline - std::chrono::steady_clock::now();
                if (remain <= std::chrono::milliseconds::zero()) break;
                const int timeout_ms = static_cast<int>(
                    std::min(std::chrono::duration_cast<std::chrono::milliseconds>(remain),
                             std::chrono::milliseconds(20))
                        .count());
                std::optional<Bytes> resp;
                try {
                    resp = io.read_input(std::max(1, timeout_ms));
                } catch (const std::exception& exc) {
                    add({now_ts(), "error", 0,
                         "读取异常（终止回放）: " + std::string(exc.what())});
                    return;
                }
                if (resp) add({now_ts(), "response", (*resp)[0], to_hex(*resp)});
            }
        }
    } catch (const std::exception& exc) {
        add({now_ts(), "error", 0, "打开失败: " + std::string(exc.what())});
        return;
    }
    add({now_ts(), stop_ ? "aborted" : "done", 0, stop_ ? "回放被中止" : "回放完成"});
}

}  // namespace pt

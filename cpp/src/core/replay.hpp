// replay.hpp — 学习模式写回放（任务 4.5）。
// 把录到的报文按可配限速回放到指定接口，并观察设备响应：
// - 回放映射（action_from_capture）：input 回声 → output 写（写触发型协议的
//   "设备回声即命令"假设）；feature_read → feature 写（配置页快照回写假设）
// - 限速：相邻写之间强制最小间隔 min_interval_ms（默认 100ms ≈ 10 条/秒，保守）
// - 观察响应：每次写后开一个 response_window_ms 窗口持续读 input（默认 50ms）
// - 全程可中止：stop() 随时安全终止（限速睡眠/写前后/读循环均检查中止标志）
// 安全边界：只回放用户显式选择的报文，绝不自动改字节；写/读异常即停（设备可能已死）。
#pragma once
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"
#include "recorder.hpp"
#include "report_io.hpp"

namespace pt {

// 单条回放动作。data_hex 含 report id 字节（沿用 CaptureEntry 约定，hidapi 透明处理：
// 无 report id 的 rawhid 通道首字节为 0）。
struct ReplayAction {
    std::string mode;      // "output" | "feature"
    int report_id = 0;     // feature 写用；output 报文 id 即首字节
    std::string data_hex;  // 完整报文 hex（空 = 跳过）
    std::string origin;    // 人读来源（如 "input#3" / "feature_read id=0xED"）
};

// 录制条目 → 回放动作。kind=input→output、kind=feature_read→feature；
// error 条目/空数据/未知 kind 返回 nullopt（不可回放）。
std::optional<ReplayAction> action_from_capture(const CaptureEntry& entry,
                                                const std::string& origin = "");

struct ReplayEvent {
    double ts = 0;
    std::string kind;      // "write" | "response" | "error" | "aborted" | "done"
    int report_id = 0;
    std::string data;      // hex；error/aborted 时为人话
};

// 单接口写回放器。start() 起线程逐条限速回放并捕获响应，stop() 随时可中止。
class Replayer {
public:
    explicit Replayer(std::string path, DevFactory factory = hidapi_factory());
    ~Replayer() { stop(); }
    Replayer(const Replayer&) = delete;
    Replayer& operator=(const Replayer&) = delete;

    void set_actions(std::vector<ReplayAction> actions);   // start 前设置
    void set_min_interval_ms(int ms);                      // 默认 100；<0 视为 0
    void set_response_window_ms(int ms);                   // 默认 50；<0 视为 0

    void start();                       // 无动作/已在进行中抛 std::runtime_error
    void stop(int timeout_ms = 3000);
    bool is_running() const;

    std::vector<ReplayEvent> events() const;   // 线程安全快照
    int total_actions() const { return static_cast<int>(actions_.size()); }
    int sent_count() const;                    // 已成功写的条数
    bool aborted() const { return aborted_; }  // 被 stop() 中途终止

private:
    void run();
    void add(const ReplayEvent& event);
    void wait_interval();               // 分段睡眠到本次写允许时刻，随时响应中止

    std::string path_;
    DevFactory factory_;
    std::vector<ReplayAction> actions_;
    int min_interval_ms_ = 100;
    int response_window_ms_ = 50;

    mutable std::mutex mutex_;
    std::vector<ReplayEvent> events_;
    int sent_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> aborted_{false};
};

}  // namespace pt

// recorder.hpp — 学习模式录制器（任务 4.1）。
// 对一个或多个 vendor/rawhid 接口做只读录制：
// - 连续 input 报文监听（Windows HID input 对所有打开句柄广播）
// - feature report 快照（录制开始时自动拍一次，改配置前后可手动再拍）
// 会话文件格式（jsonl，每行一个 JSON 对象）：
//   {"ts": 1699999999.123, "source": "FF21", "kind": "input",
//    "report_id": 237, "data": "0e ed ..."}
// 安全边界：只读（read_input / get_feature），绝不写入。
#pragma once
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"
#include "json_util.hpp"
#include "report_io.hpp"

namespace pt {

constexpr int RECORDER_MAX_ENTRIES = 200000;  // 防失控内存增长；超出自动停止并标记

struct RecorderTarget {
    std::string label;                  // 人读标签（如 "FF21" / "rawhid"）
    std::string path;                   // 设备路径（DeviceInfo.path）
    std::vector<int> feature_ids;       // 需要做快照的 feature report ID
};

struct CaptureEntry {
    double ts = 0;
    std::string source;   // target.label
    std::string kind;     // "input" | "feature_read" | "error"
    int report_id = 0;    // input 的最佳估计 = data[0]；无 ID 报文即首字节
    std::string data;     // hex（空格分隔）；error 时为人话文本；无响应快照为 ""
};

// 多接口只读录制器。start() 起线程，stop() 收尾，entries()/save_jsonl() 取数据。
class Recorder {
public:
    Recorder(std::vector<RecorderTarget> targets, int read_timeout_ms = 50,
             int max_entries = RECORDER_MAX_ENTRIES, DevFactory factory = hidapi_factory());
    ~Recorder() { stop(); }
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    void start();                       // 已在进行中抛 std::runtime_error
    void stop(int timeout_ms = 3000);
    bool is_recording() const;
    bool overflowed() const { return overflowed_; }

    std::vector<CaptureEntry> entries() const;   // 线程安全快照
    int snapshot_features();                     // 手动快照（改配置前后各拍）。返回记录条数

    int save_jsonl(const std::filesystem::path& path) const;  // 返回写入条数
    static std::vector<CaptureEntry> load_jsonl(const std::filesystem::path& path);

private:
    bool add(const CaptureEntry& entry);         // 溢出置位并请求停止；返回是否写入
    void run_target(const RecorderTarget& target);

    std::vector<RecorderTarget> targets_;
    int read_timeout_ms_;
    int max_entries_;
    DevFactory factory_;

    mutable std::mutex mutex_;
    std::vector<CaptureEntry> entries_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> overflowed_{false};
    std::vector<std::thread> threads_;
};

}  // namespace pt

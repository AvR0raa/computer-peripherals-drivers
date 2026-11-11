// recorder.cpp — 录制器实现（core/recorder.py 平移）。
#include "recorder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>

namespace pt {

namespace {
constexpr const char* HEX_DIGITS = "0123456789abcdef";

std::string hex_or_empty(const std::optional<Bytes>& data) {
    return data ? to_hex(*data) : "";
}

double now_ts() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
}  // namespace

Recorder::Recorder(std::vector<RecorderTarget> targets, int read_timeout_ms, int max_entries,
                   DevFactory factory)
    : targets_(std::move(targets)), read_timeout_ms_(read_timeout_ms),
      max_entries_(max_entries), factory_(std::move(factory)) {}

void Recorder::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!threads_.empty())
        throw std::runtime_error("录制已在进行中");
    stop_ = false;
    for (const auto& target : targets_) {
        threads_.emplace_back([this, target] { run_target(target); });
    }
}

void Recorder::stop(int timeout_ms) {
    (void)timeout_ms;  // 读取循环以 read_timeout_ms 粒度检查 stop，join 至线程退出
    stop_ = true;
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        threads.swap(threads_);
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();
}

bool Recorder::is_recording() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !threads_.empty() && !stop_;
}

std::vector<CaptureEntry> Recorder::entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

bool Recorder::add(const CaptureEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(entries_.size()) >= max_entries_) {
        overflowed_ = true;
        stop_ = true;
        return false;
    }
    entries_.push_back(entry);
    return true;
}

void Recorder::run_target(const RecorderTarget& target) {
    try {
        ReportIO io(target.path, factory_);
        io.open();
        for (int fid : target.feature_ids) {
            std::optional<Bytes> data;
            try {
                data = io.get_feature(fid, 64);
            } catch (const std::exception& exc) {
                if (!add(CaptureEntry{now_ts(), target.label, "error", fid,
                                      "feature 读取异常: " + std::string(exc.what())}))
                    return;
                continue;
            }
            if (!add(CaptureEntry{now_ts(), target.label, "feature_read", fid,
                                  hex_or_empty(data)}))
                return;
        }
        while (!stop_) {
            std::optional<Bytes> data;
            try {
                data = io.read_input(read_timeout_ms_);
            } catch (const std::exception& exc) {
                add(CaptureEntry{now_ts(), target.label, "error", 0,
                                 "input 读取异常（目标终止）: " + std::string(exc.what())});
                return;  // 读取异常视为该目标死亡，终止此线程
            }
            if (data) {
                if (!add(CaptureEntry{now_ts(), target.label, "input", (*data)[0],
                                      hex_or_empty(*data)}))
                    return;
            }
        }
    } catch (const std::exception& exc) {
        add(CaptureEntry{now_ts(), target.label, "error", 0, "打开失败: " + std::string(exc.what())});
    }
}

int Recorder::snapshot_features() {
    int count = 0;
    for (const auto& target : targets_) {
        if (target.feature_ids.empty()) continue;
        try {
            ReportIO io(target.path, factory_);
            io.open();
            for (int fid : target.feature_ids) {
                std::optional<Bytes> data;
                try {
                    data = io.get_feature(fid, 64);
                } catch (const std::exception& exc) {
                    if (add(CaptureEntry{now_ts(), target.label, "error", fid,
                                         "feature 读取异常: " + std::string(exc.what())}))
                        ++count;
                    continue;
                }
                if (add(CaptureEntry{now_ts(), target.label, "feature_read", fid,
                                     hex_or_empty(data)}))
                    ++count;
            }
        } catch (const std::exception& exc) {
            if (add(CaptureEntry{now_ts(), target.label, "error", 0,
                                 "打开失败: " + std::string(exc.what())}))
                ++count;
        }
    }
    return count;
}

int Recorder::save_jsonl(const std::filesystem::path& path) const {
    const auto snapshot = entries();
    std::ofstream fh(path, std::ios::binary);
    if (!fh) throw std::runtime_error("无法写入会话文件: " + path.string());
    for (const auto& e : snapshot) {
        Json line = Json::object();
        line["ts"] = e.ts;
        line["source"] = e.source;
        line["kind"] = e.kind;
        line["report_id"] = e.report_id;
        line["data"] = e.data;
        fh << line.dump(-1, ' ', false, Json::error_handler_t::replace) << "\n";
    }
    return static_cast<int>(snapshot.size());
}

std::vector<CaptureEntry> Recorder::load_jsonl(const std::filesystem::path& path) {
    std::vector<CaptureEntry> entries;
    std::ifstream fh(path, std::ios::binary);
    if (!fh) throw std::runtime_error("无法读取会话文件: " + path.string());
    std::string line;
    while (std::getline(fh, line)) {
        // 去掉 \r（Windows 行尾）与首尾空白
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        std::size_t begin = 0;
        while (begin < line.size() && (line[begin] == ' ' || line[begin] == '\t')) ++begin;
        if (begin >= line.size()) continue;
        const Json obj = Json::parse(line.substr(begin));
        entries.push_back(CaptureEntry{obj.at("ts").get<double>(),
                                       obj.at("source").get<std::string>(),
                                       obj.at("kind").get<std::string>(),
                                       obj.at("report_id").get<int>(),
                                       obj.at("data").get<std::string>()});
    }
    return entries;
}

}  // namespace pt

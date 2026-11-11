// report_io.hpp — HID report 模板渲染 + 读写器（任务 2.2）。
// 纯函数 render_template / match_response 可离线测试；
// ReportIO 通过 DevFactory 注入设备（真实实现包 hidapi，测试按脚本回放假设备）。
#pragma once
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"
#include "json_util.hpp"

namespace pt {

struct RenderError : std::runtime_error {
    using std::runtime_error::runtime_error;  // 模板或参数不合法
};

struct ReportError : std::runtime_error {
    using std::runtime_error::runtime_error;  // hidapi 打开/读写失败
};

// ---------------------------------------------------------------------------
// 设备抽象：真实实现包 hidapi；测试实现按脚本回放（等价 Python monkeypatch）。
// ---------------------------------------------------------------------------

struct HidDevice {
    virtual ~HidDevice() = default;
    virtual int write(const Bytes& data) = 0;
    virtual Bytes read(int timeout_ms, int size) = 0;       // 空 = 无数据/超时
    virtual int send_feature(const Bytes& data) = 0;
    virtual Bytes get_feature(int report_id, int size) = 0; // 空 = 失败
    virtual void close() {}
};

using DevFactory = std::function<std::unique_ptr<HidDevice>(const std::string& path)>;

// 描述符读取回调：path → 原始字节；空 = 读取失败（scan_all/probe 共用签名）。
using ReadDescriptorFn = std::function<Bytes(const std::string& path)>;

// 真实 hidapi 设备工厂（ReportIO 默认值）。
DevFactory hidapi_factory();

// ---------------------------------------------------------------------------
// 纯函数
// ---------------------------------------------------------------------------

// 模板 + 参数 → 字节流。元素: int(0-255) | "{param}" | "{#negsum}"（校验和，
// 覆盖其前所有渲染字节）。未知占位符/越界/缺参数抛 RenderError。
Bytes render_template(const Json& template_, const Params& params);

// 响应字节流是否满足 check：int 精确 | null 通配 | "{param}" | [v1,v2..] 任一。
bool match_response(const Bytes& response, const Json& check, const Params* params = nullptr);

// 按 extract 索引表从响应取字段: {"major": 4} -> {"major": response[4]}；越界为空。
std::map<std::string, std::optional<int>> extract_fields(const Bytes& response,
                                                         const Json& extract);

// ---------------------------------------------------------------------------
// ReportIO
// ---------------------------------------------------------------------------

struct HistoryEntry {
    double ts;
    std::string direction;  // "tx" | "rx"
    Bytes data;
};

using HistoryCb = std::function<void(const HistoryEntry&)>;

// "可执行 call_multi 的通道"接口：ReportIO（单通道）与 hidpp::HidppLink（双通道）
// 都实现它，hidpp/dpi 函数因此对两者通用（对应 Python 的 duck typing）。
struct HidppChannel {
    virtual ~HidppChannel() = default;
    virtual void open() {}
    virtual void close() {}
    virtual std::optional<std::pair<Bytes, std::string>> call_multi(
        const Bytes& data, const std::vector<std::pair<std::string, Json>>& checks,
        const Params* params, int timeout_ms) = 0;
    virtual int write_output(const Bytes& data) = 0;
    virtual std::optional<Bytes> read_input(int timeout_ms, int size) = 0;
};

// 一个 HID 接口的读写句柄。惰性打开，析构自动 close（RAII，杜绝句柄泄漏）。
class ReportIO : public HidppChannel {
public:
    ReportIO(std::string path, DevFactory factory = hidapi_factory(),
             HistoryCb on_history = nullptr);
    ~ReportIO();
    ReportIO(const ReportIO&) = delete;
    ReportIO& operator=(const ReportIO&) = delete;
    ReportIO(ReportIO&&) = default;              // 移动安全：句柄所有权转移
    ReportIO& operator=(ReportIO&&) = default;

    void open() override;    // 已打开则无操作；失败抛 ReportError
    void close() override;
    bool is_open() const { return dev_ != nullptr; }

    int write_output(const Bytes& data) override;             // 失败抛 ReportError
    std::optional<Bytes> read_input(int timeout_ms = 500, int size = 64) override;
    void send_feature(const Bytes& data);                     // 失败抛 ReportError
    std::optional<Bytes> get_feature(int report_id = 0, int size = 64);

    // 写 output report，然后轮询读 input 直到出现匹配 check 的响应。
    // 返回第一个匹配的响应；check 为空指针/null 时返回第一个收到的响应；超时返回空。
    std::optional<Bytes> call(const Bytes& data, const Json* check,
                              const Params* params = nullptr, int timeout_ms = 500);

    using Checks = std::vector<std::pair<std::string, Json>>;

    // 多组分支：写 output 后等匹配 (label, check) 的响应（"正常应答+错误应答"并存场景）。
    std::optional<std::pair<Bytes, std::string>> call_multi(const Bytes& data,
                                                            const Checks& checks,
                                                            const Params* params = nullptr,
                                                            int timeout_ms = 500) override;
    // 只读轮询 input，直到命中某组 (label, check) 或超时。不写任何报文。
    std::optional<std::pair<Bytes, std::string>> read_until(const Checks& checks,
                                                            const Params* params = nullptr,
                                                            int timeout_ms = 500);

    std::vector<HistoryEntry> history;  // 每次收发追加

    // 实时订阅（GUI 用；对应 Python 的公开属性 on_history）。线程任意，回调由调用方保证。
    void set_history_callback(HistoryCb cb) { on_history_ = std::move(cb); }

private:
    std::string path_;
    DevFactory factory_;
    std::unique_ptr<HidDevice> dev_;
    HistoryCb on_history_;
    void log(const char* direction, const Bytes& data);
};

// ---------------------------------------------------------------------------
// 功能执行器（schema feature -> 实际收发）
// ---------------------------------------------------------------------------

struct FeatureResult {
    bool ok = false;                            // 命中主 check（业务意义上的成功）
    std::optional<Bytes> response;
    std::optional<std::string> branch;          // 空=主分支；否则为 alt 的 label
    std::map<std::string, std::optional<int>> decoded;  // extract 出来的字段
};

// 按协议库 feature 定义执行一次收发（command/read 通用）。
// params 未提供的项自动回填 params 定义里的 default。
// report=output: 写 output 并轮询等 input 响应（HID++ 等）。
// report=feature: send_feature 后立即 get_feature 同 ID 读回（无超时轮询）。
// report=input: 不写任何报文，只读轮询 input 直到匹配（学习模式"监听型"功能）。
FeatureResult execute_feature(ReportIO& io, const Json& feature,
                              const Params* params = nullptr, int timeout_ms = 600);

}  // namespace pt

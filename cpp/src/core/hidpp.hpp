// hidpp.hpp — HID++ 2.0 最小客户端（电量/在线探测/报告率）。
// 报文布局移植自 Solaar (lib/logitech_receiver/base.py + hidpp20*.py)，来源已标注：
// - 短报文: [0x10, dev, reqHi, reqLo, p0, p1, p2]，reqHi=featIdx、reqLo=(funct<<4)|sw_id
// - feature 查询: Root(0x0000) funct 0，params=[featIdHi, featIdLo]，应答首字节=index(0=无)
// - 电量 feature: 0x1000 BatteryStatus / 0x1001 BatteryVoltage / 0x1004 UnifiedBattery
#pragma once
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"
#include "json_util.hpp"
#include "report_io.hpp"

namespace pt {

constexpr int SW_ID = 5;  // 兼容常量保留；实际 sw_id 每请求轮换（见 hidpp_next_sw_id）

constexpr int FEATURE_ROOT = 0x0000;
constexpr int FEATURE_BATTERY_STATUS = 0x1000;
constexpr int FEATURE_BATTERY_VOLTAGE = 0x1001;
constexpr int FEATURE_UNIFIED_BATTERY = 0x1004;
constexpr int FEATURE_ADJUSTABLE_DPI = 0x2201;
constexpr int FEATURE_EXTENDED_ADJUSTABLE_DPI = 0x2202;
constexpr int FEATURE_REPORT_RATE = 0x8060;
constexpr int FEATURE_EXTENDED_REPORT_RATE = 0x8061;

// 0x8061 Extended Report Rate: 索引式档位（Solaar ExtendedReportRate choices_universe）
extern const int POLL_RATE_BY_INDEX[7];
std::optional<int> poll_index_by_hz(int hz);

// BatteryStatus 语义（Solaar hidpp20_constants.BatteryStatus 子集）；未知回退 "0xXX"
std::string battery_status_name(int status);

// 测试辅助：重置 sw_id 轮换计数（对齐 hidpp_scripted 的预测序列）。
void hidpp_reset_sw_id();

struct HidppBattery {
    std::optional<int> percent;
    std::optional<int> voltage_mv;
    bool charging = false;
    int status_raw = 0;
    std::string source;   // "hidpp-unified" | "hidpp-status" | "hidpp-voltage"
    std::string detail;
};

// HID++ 短/长报文双通道封装。
// 接收器把 0x10(短)/0x11(长) 报文分在两个 FF00 接口上（C54D 实测：
// FF00:0001=短 6B，FF00:0002=长 19B）。按报文 ID 自动路由；
// 单接口设备只给 short_path，两种报文都走它。默认构造 = 无通道（测试注入用）。
class HidppLink : public HidppChannel {
public:
    HidppLink() = default;
    HidppLink(const std::string& short_path, const std::string& long_path = std::string(),
              DevFactory factory = hidapi_factory());

    void open();
    void close();
    bool has_long() const { return long_ != nullptr; }

    // 按 wire[0] 选发送通道；双通道时两路输入轮询等应答（应答长度与请求无关，
    // 短请求的长应答会出现在长通道，所以双通道设备必须两边都听）。
    std::optional<std::pair<Bytes, std::string>> call_multi(const Bytes& wire,
                                                            const ReportIO::Checks& checks,
                                                            const Params* params,
                                                            int timeout_ms) override;
    int write_output(const Bytes& data) override;
    std::optional<Bytes> read_input(int timeout_ms, int size) override;

    // 测试注入假通道（所有权转移给 link）。
    void set_channels(std::unique_ptr<HidppChannel> short_ch,
                      std::unique_ptr<HidppChannel> long_ch = nullptr);

private:
    HidppChannel& tx_for(const Bytes& wire);
    std::unique_ptr<HidppChannel> short_, long_;
};

// 执行一次 feature 调用，返回应答 payload（req-id 回显之后的部分）。
// long_msg 缺省时按参数长度自动选择（>3 字节走长报文）。
// 错误应答（0x8F）或超时返回空。
Bytes call_feature(HidppChannel& link, int dev, int feature_index, int funct = 0,
                   const Bytes& params = {}, int timeout_ms = 500,
                   std::optional<bool> long_msg = std::nullopt);

// Root.GetFeature：查询 feature_id 分配的 index；设备不支持返回空。
std::optional<int> get_feature_index(HidppChannel& io, int dev, int feature_id,
                                     int timeout_ms = 500);

// HID++ ping。返回 (状态, 协议版本)；状态: "2.0"|"1.0"|"empty-slot"|"offline"。
// 双通道时先试长报文（新款设备只应答长报文，实测 C54D 槽位），错误 0x08 换通道重试；
// 单通道只试短报文。实测 PRO X2 的 ping 应答不回显 mark 字节，故校验只到 reqLo。
std::pair<std::string, std::optional<double>> ping(HidppChannel& link, int dev,
                                                   int mark = 66, int timeout_ms = 500);

// 反复 ping 直到设备在线（HID++ 流量本身会唤醒睡眠设备，fwupd #7011 佐证）。
// 返回协议版本；全程不在线返回空。
std::optional<double> ensure_online(HidppChannel& link, int dev, int attempts = 8,
                                    double interval = 1.2, int timeout_ms = 600);

// 支持的报告率列表（Hz，从高到低）。不支持返回空。
std::vector<int> get_poll_rates(HidppChannel& link, int dev, int timeout_ms = 500);

// 当前报告率（Hz）。不支持返回空。
std::optional<int> read_poll_rate(HidppChannel& link, int dev, int timeout_ms = 500);

// 设置报告率；读回验证。返回 (是否成功, 说明)。
std::pair<bool, std::string> set_poll_rate(HidppChannel& link, int dev, int hz,
                                           int timeout_ms = 600);

// 读电量：Unified(0x1004) → Status(0x1000) → Voltage(0x1001) 三路回退。全部不可用返回空。
std::optional<HidppBattery> read_battery(HidppChannel& io, int dev = 0xFF, int timeout_ms = 500);

}  // namespace pt

// vendor_battery.hpp — 国产外设电量协议（M5b，移植自 EZ2.4G 项目，来源已标注）。
// 三套协议，均为"写查询命令 → 听应答"模式（只读查询，不修改设备设置）：
// 1. ATK（前行者 / attack shark 鼠标，VID 0x373B / 0x3554）
// 2. MCHOSE（迈从 C-Media 方案 dongle，VID 0x3837 / 0x1837 / 0x3654 / 0x291D）
// 3. 杰理 JL_RCSP（芯片方案，任意 VID 的 64B 厂商接口通用兜底）
#pragma once
#include <optional>
#include <set>
#include <string>

#include "common.hpp"
#include "report_io.hpp"

namespace pt {

inline const std::set<std::string> ATK_VIDS{"373B", "3554"};
inline const std::set<std::string> MCHOSE_VIDS{"291D", "3837", "1837", "3654"};

struct VendorBattery {
    int percent = 0;
    std::string source;   // "atk1" / "atk2" / "mchose" / "jl"
    std::string detail;
};

// ---- 各协议（测试也直接调用） ----
std::optional<VendorBattery> read_atk(ReportIO& io, double timeout = 0.8, int attempts = 3);
std::optional<VendorBattery> read_mchose(ReportIO& io, double timeout = 1.0);
std::optional<VendorBattery> read_jl(ReportIO& io, double seconds = 1.2);

// 按 VID 路由厂商协议；杰理作为通用兜底最后试。
std::optional<VendorBattery> try_vendor_battery(const std::string& vid_hex, ReportIO& io);

// ---- 内部函数（对测试开放，对应 Python 下划线函数） ----
int atk1_checksum(int report_id, const Bytes& cmd);
bool atk1_valid(const Bytes& rep);
Bytes build_jl_frame(int seq, const Bytes& payload, int report_id, int total = 64,
                     bool include_zero = true);
std::optional<Bytes> parse_jl(const Bytes& report);

}  // namespace pt

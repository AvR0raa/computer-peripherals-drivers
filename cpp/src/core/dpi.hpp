// dpi.hpp — 鼠标 DPI 读写（M2.5b）— HID++ Adjustable DPI 0x2201 / Extended 0x2202。
// 布局移植自 Solaar settings_templates（AdjustableDpi + ExtendedAdjustableDpi +
// produce_dpi_list）：
// 0x2201 Adjustable DPI:
// - GetSensorDpiList: funct 1，params [sensorIdx, direction, page]；
//   应答数据段 = 大端 16 位 DPI 序列直到 0x0000；val>>13==0b111 为步进编码
//   [0xE000|step, last]，展开 range(prev+step, last+1, step)
// - GetSensorDpi:  funct 2，payload [X, dpiHi, dpiLo, ...] → DPI = BE16@offset1
// - SetSensorDpi:  funct 3，params [sensorIdx, dpiHi, dpiLo]
// 0x2202 Extended Adjustable DPI（PRO X2 等新款用这个，Solaar #2207/#2229）:
// - GetInfo:       funct 1，params [0x00]；payload[2] bit0=Y轴 bit1=LOD
// - GetDpiList:    funct 2，params [0, axis, page]；数据从 payload[3:] 起
// - GetDpi:        funct 5，X@[1:3](0→默认@[3:5])，Y@[5:7](0→@[7:9])，LOD@[9]
// - SetDpi:        funct 6，params [0x00, Xhi, Xlo, ...]
// 写安全：读旧值 → 写新值 → 读回验证 → 不匹配回滚旧值（plan.md 风险表）。
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "hidpp.hpp"

namespace pt {

struct DpiOutcome {
    bool ok = false;
    std::optional<int> wrote;
    std::optional<int> old;
    std::optional<int> readback;
    bool rolled_back = false;
    std::string feature;    // "0x2201" | "0x2202"
    std::vector<std::string> log;
};

// 设备支持的 DPI 档位表（含步进区间展开）。不支持返回空。
std::vector<int> get_dpi_list(HidppChannel& link, int dev, int timeout_ms = 500);

struct DpiRead {
    std::optional<int> x;
    std::optional<int> y;
    std::optional<int> lod;
    std::optional<int> default_dpi;
};

// 读当前 DPI。失败各字段为空。
DpiRead read_dpi(HidppChannel& link, int dev, int timeout_ms = 500);

// 带安全网的 DPI 写入：读旧 → 写 → 读回验证 → 失败回滚。
DpiOutcome set_dpi(HidppChannel& link, int dev, int new_dpi, int timeout_ms = 600);

}  // namespace pt

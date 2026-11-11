// fingerprint.hpp — 指纹匹配：VID/PID + 品牌表 → 设备状态标注（任务 1.3）。
// 状态三分类（对应 plan.md 验收标准）：
//   KNOWN   —— 协议库有该 VID:PID 条目，M2 可走直通引擎
//   SDK     —— 品牌 VID 已知且品牌有官方 SDK，走 SDK 桥路线
//   UNKNOWN —— 可进入学习模式（品牌已知时会附带品牌名）
#pragma once
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "common.hpp"
#include "device_manager.hpp"
#include "json_util.hpp"

namespace pt {

struct ProtocolDBError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// 协议库路径：C++ 版与 Python 版共享同一份 protocol_db/db.json。
// 优先级: 环境变量 PERIPHERAL_TOOL_DB > 源码树（开发构建） > 相对路径。
std::filesystem::path default_db_path();

enum class DeviceStatus { Known, Sdk, Unknown };

// CLI 状态列标记与 JSON 状态值。
const char* status_mark(DeviceStatus s);    // "OK " / "SDK" / "???"
const char* status_value(DeviceStatus s);   // "known" / "sdk" / "unknown"

struct FingerprintResult {
    DeviceStatus status;
    std::optional<std::string> brand;   // 品牌英文名；未识别为空
    std::optional<std::string> brand_zh;
    std::vector<std::string> sdk;       // 官方 SDK 名称（无则空）
    Json entry;                         // db.devices 命中的完整条目（is_null()=未命中）
    std::string detail;                 // 一句话解释
};

// 校验协议库整体结构，非法抛 ProtocolDBError。
void validate_db(const Json& db);

// 判断是否笔记本内置/系统虚拟设备（brands.internal 标记 + HID over I2C bus_type=3）。
bool is_internal_device(const Json& db, const DeviceInfo& device);

// 读取并校验协议库。缺失/JSON 语法错/结构非法抛 ProtocolDBError。
Json load_db(const std::filesystem::path& path = default_db_path());

// 校验后原子写回协议库（tmp + rename，写坏不伤原文件）。
void save_db(const Json& db, const std::filesystem::path& path = default_db_path());

// 对单个设备标注状态。
FingerprintResult fingerprint(const DeviceInfo& device, const Json& db);

}  // namespace pt

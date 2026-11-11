// json_util.hpp — nlohmann::ordered_json 别名与 JSON 小工具。
// schema 校验（fingerprint）、协议引擎（report_io）、学习/档案模块共用；
// 纯逻辑模块（descriptor/device_manager）不依赖本文件，保持零 JSON 开销。
#pragma once
#include <map>
#include <string>

#include <nlohmann/json.hpp>

namespace pt {

// 有序 JSON：保持键序与 Python dict/原文件一致，save_db 往返不重排键。
using Json = nlohmann::ordered_json;

// 协议参数表（对应 Python 的 params dict: {名称: 整数值}）。
using Params = std::map<std::string, int>;

// 0-255 整数。nlohmann 中 bool 不是 number_integer，天然排除 Python 的 bool 陷阱。
inline bool json_is_byte(const Json& v) {
    return v.is_number_integer() && v >= 0 && v <= 255;
}

// 4 位十六进制串（VID/PID/usage 匹配键），等价 Python re.fullmatch(r"[0-9A-Fa-f]{4}")
inline bool is_hex4(const std::string& s) {
    if (s.size() != 4) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

}  // namespace pt

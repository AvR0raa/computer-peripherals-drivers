// common.hpp — 公共小工具：字节类型与十六进制格式化。
// 各 core 模块共用的仅限此文件；模块之间禁止互相 include（组合发生在 app 层）。
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN  // 只要 GetModuleFileNameW；hidapi 那边自己带完整 windows.h
#endif
#include <windows.h>
#endif

namespace pt {

using Bytes = std::vector<std::uint8_t>;

// 整数 → 大写十六进制固定位宽，如 hex(0x46d, 4) == "046D"（对应 Python f"{v:04X}"）。
inline std::string hex(std::uint64_t value, int w) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%0*llX", w, static_cast<unsigned long long>(value));
    return buf;
}

// 字节流 → "0a ff 00" 风格小写（对应 Python data.hex(" ")）。
inline std::string to_hex(const Bytes& data) {
    std::string out;
    out.reserve(data.size() * 3);
    char buf[4];
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i) out.push_back(' ');
        std::snprintf(buf, sizeof buf, "%02x", data[i]);
        out += buf;
    }
    return out;
}

// 模板占位符 "{name}"：首字符字母/下划线，其余字母/数字/下划线
// （等价 Python re ^\{[A-Za-z_]\w*\}$；fingerprint 校验与 report_io 渲染共用）
inline bool is_placeholder(const std::string& s) {
    if (s.size() < 3 || s.front() != '{' || s.back() != '}') return false;
    const std::string inner = s.substr(1, s.size() - 2);
    const auto id_start = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    const auto id_char = [&id_start](char c) { return id_start(c) || (c >= '0' && c <= '9'); };
    if (!id_start(inner[0])) return false;
    for (std::size_t i = 1; i < inner.size(); ++i)
        if (!id_char(inner[i])) return false;
    return true;
}

// 计算型占位符 "{#algo}"（小写字母+下划线）→ algo；非占位符返回空
// （等价 Python re ^\{#([a-z_]+)\}$）
inline std::optional<std::string> computed_algo(const std::string& s) {
    if (s.size() < 4 || s.front() != '{' || s[1] != '#' || s.back() != '}') return std::nullopt;
    const std::string inner = s.substr(2, s.size() - 3);
    if (inner.empty()) return std::nullopt;
    for (char c : inner)
        if (!(c == '_' || (c >= 'a' && c <= 'z'))) return std::nullopt;
    return inner;
}

// 可执行文件所在目录（分发版数据文件定位用；非 Windows 回退为当前目录）。
inline std::filesystem::path exe_dir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::filesystem::path(buf).parent_path();
#endif
    return std::filesystem::current_path();
}

}  // namespace pt

// diffing.cpp — 差分分析实现（core/diffing.py 平移）。
#include "diffing.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

namespace pt {

namespace {

Bytes hex_to_bytes(const std::string& data) {
    Bytes out;
    if (data.empty()) return out;
    std::string packed;
    packed.reserve(data.size());
    for (char c : data)
        if (c != ' ') packed.push_back(c);
    if (packed.size() % 2 != 0) return out;  // 非法 hex（录制数据不会出现）
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t i = 0; i + 1 < packed.size(); i += 2) {
        const int hi = nibble(packed[i]), lo = nibble(packed[i + 1]);
        if (hi < 0 || lo < 0) return Bytes(packed.size() / 2, 0);
        out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::map<GroupKey, CaptureEntry> last_by_group(const std::vector<CaptureEntry>& entries) {
    std::map<GroupKey, CaptureEntry> last;
    for (const auto& e : entries) {
        if (e.kind == "error") continue;
        last[{e.source, e.kind, e.report_id}] = e;
    }
    return last;
}

std::string byte_text(const std::optional<int>& value) {
    if (!value) return "--";
    char buf[8];
    std::snprintf(buf, sizeof buf, "0x%02X", *value);
    return buf;
}

}  // namespace

std::set<GroupKey> volatile_groups(const std::vector<CaptureEntry>& entries, int threshold) {
    std::map<GroupKey, std::set<std::string>> seen;
    for (const auto& e : entries) {
        if (e.kind == "error" || e.data.empty()) continue;
        seen[{e.source, e.kind, e.report_id}].insert(e.data);
    }
    std::set<GroupKey> out;
    for (const auto& [key, values] : seen)
        if (static_cast<int>(values.size()) >= threshold) out.insert(key);
    return out;
}

std::vector<GroupDiff> diff_sessions(const std::vector<CaptureEntry>& before,
                                     const std::vector<CaptureEntry>& after) {
    const auto last_before = last_by_group(before);
    const auto last_after = last_by_group(after);
    std::set<GroupKey> noise = volatile_groups(before);
    const auto noise_after = volatile_groups(after);
    noise.insert(noise_after.begin(), noise_after.end());

    std::vector<GroupDiff> diffs;
    for (const auto& [key, entry_after] : last_after) {
        const Bytes a_data = hex_to_bytes(entry_after.data);
        const auto it = last_before.find(key);
        GroupDiff d;
        d.source = std::get<0>(key);
        d.kind = std::get<1>(key);
        d.report_id = std::get<2>(key);
        d.after_len = static_cast<int>(a_data.size());
        d.is_volatile = noise.count(key) != 0;
        if (it == last_before.end()) {
            for (std::size_t i = 0; i < a_data.size(); ++i)
                d.changes.push_back({static_cast<int>(i), std::nullopt, a_data[i]});
            d.before_len = 0;
            d.is_new = true;
            diffs.push_back(std::move(d));
            continue;
        }
        const Bytes b_data = hex_to_bytes(it->second.data);
        if (b_data == a_data) continue;
        d.before_len = static_cast<int>(b_data.size());
        const int max_len = std::max<int>(d.before_len, d.after_len);
        for (int i = 0; i < max_len; ++i) {
            const std::optional<int> bv =
                i < static_cast<int>(b_data.size())
                    ? std::optional<int>(b_data[static_cast<std::size_t>(i)])
                    : std::nullopt;
            const std::optional<int> av =
                i < static_cast<int>(a_data.size())
                    ? std::optional<int>(a_data[static_cast<std::size_t>(i)])
                    : std::nullopt;
            if (bv != av) d.changes.push_back({i, bv, av});
        }
        diffs.push_back(std::move(d));
    }
    return diffs;
}

std::vector<std::string> format_diff(const GroupDiff& diff) {
    std::vector<std::string> lines;
    const std::string head =
        diff.source + " " + diff.kind + " id=" + std::to_string(diff.report_id);
    std::string marks;
    if (diff.is_new) marks += "新报文组";
    if (diff.is_volatile) {
        if (!marks.empty()) marks += "/";
        marks += "疑似噪声流";
    }
    const std::string prefix = head + (marks.empty() ? "" : "（" + marks + "）");
    if (diff.is_new) {
        lines.push_back(prefix + ": " + std::to_string(diff.after_len) + " 字节");
        std::string content;
        for (const auto& c : diff.changes) {
            if (!c.after) continue;
            char buf[8];
            std::snprintf(buf, sizeof buf, "%02X", *c.after);
            if (!content.empty()) content += " ";
            content += buf;
        }
        lines.push_back("  内容: " + content);
        return lines;
    }
    lines.push_back(prefix + ": " + std::to_string(diff.changes.size()) + " 字节变化 （"
                    + std::to_string(diff.before_len) + "B → " + std::to_string(diff.after_len)
                    + "B）");
    const int shown = std::min<std::size_t>(16, diff.changes.size());
    for (int i = 0; i < shown; ++i) {
        const auto& c = diff.changes[static_cast<std::size_t>(i)];
        char offset_buf[16];
        std::snprintf(offset_buf, sizeof offset_buf, "[%3d]", c.offset);
        lines.push_back(std::string("  ") + offset_buf + " " + byte_text(c.before) + " → "
                        + byte_text(c.after));
    }
    if (diff.changes.size() > 16)
        lines.push_back("  … 还有 " + std::to_string(diff.changes.size() - 16) + " 个变化字节");
    return lines;
}

}  // namespace pt

// descriptor.cpp — 描述符解析器实现（core/descriptor.py 的忠实平移）。
#include "descriptor.hpp"

#include <cstdlib>

namespace pt {

CollectionNode::~CollectionNode() = default;

namespace {

constexpr std::uint8_t LONG_ITEM_PREFIX = 0xFE;
constexpr std::uint8_t ITEM_SIZE[4] = {0, 1, 2, 4};  // 前缀 bit0-1 -> 数据字节数

const char* main_report_type(std::uint8_t tag) {
    switch (tag) {
        case 0x8: return "input";
        case 0x9: return "output";
        case 0xB: return "feature";
        default:  return nullptr;
    }
}

const char* collection_kind(std::uint32_t v) {
    switch (v) {
        case 0x00: return "Physical";
        case 0x01: return "Application";
        case 0x02: return "Logical";
        case 0x03: return "Report";
        case 0x04: return "Named Array";
        case 0x05: return "Usage Switch";
        case 0x06: return "Usage Modifier";
        default:   return nullptr;
    }
}

const char* usage_page_name(std::uint32_t page) {
    switch (page) {
        case 0x01: return "Generic Desktop";
        case 0x02: return "Simulation";
        case 0x05: return "Game Pad";
        case 0x06: return "Keyboard/Keypad";
        case 0x07: return "LED";
        case 0x08: return "Button";
        case 0x09: return "Tablet";
        case 0x0C: return "Consumer";
        default:   return nullptr;
    }
}

const char* root_usage_name(std::uint32_t usage) {
    switch (usage) {
        case 0x00010001: return "Pointer";
        case 0x00010002: return "Mouse";
        case 0x00010004: return "Joystick";
        case 0x00010005: return "Game Pad";
        case 0x00010006: return "Keyboard";
        case 0x00010080: return "System Control";
        case 0x000C0001: return "Consumer Control";
        default:         return nullptr;
    }
}

// 小端无符号解析（HID 条目数据为小端）。
std::uint32_t unsigned_le(const std::uint8_t* p, std::size_t n) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < n; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
    return v;
}

// 按字节宽度做小端有符号解析（全局 min/max 为有符号值）。
int signed_le(const std::uint8_t* p, std::size_t n) {
    const std::uint32_t v = unsigned_le(p, n);
    const unsigned bits = static_cast<unsigned>(n) * 8;
    if (bits < 32 && v >= (1u << (bits - 1)))
        return static_cast<int>(v) - static_cast<int>(1u << bits);
    return static_cast<int>(v);
}

// 短 usage（<=0xFFFF）与当前 Usage Page 拼成 32 位；完整 32 位值原样保留。
std::uint32_t resolve_usage(std::uint32_t value, std::uint32_t page) {
    return value <= 0xFFFF ? (page << 16) | value : value;
}

}  // namespace

std::string format_usage(std::uint32_t usage) {
    const std::uint32_t page = usage >> 16;
    if (page >= 0xFF00)
        return "0x" + hex(usage, 8) + " (Vendor-defined page 0x" + hex(page, 4) + ")";
    if (const char* root = root_usage_name(usage))
        return "0x" + hex(usage, 8) + " (" + root + ")";
    if (const char* name = usage_page_name(page))
        return "0x" + hex(usage, 8) + " (" + name + ")";
    return "0x" + hex(usage, 8) + " (page 0x" + hex(page, 4) + ")";
}

DescriptorInfo parse_report_descriptor(const Bytes& data) {
    DescriptorInfo info;
    const std::size_t n = data.size();
    std::size_t i = 0;

    // 全局条目状态
    std::uint32_t g_page = 0, g_report_size = 0, g_report_id = 0, g_report_count = 0;
    std::optional<int> g_logical_min, g_logical_max;
    struct Global { std::uint32_t page, size, id, count; std::optional<int> lmin, lmax; };
    std::vector<Global> g_stack;

    // 局部条目状态（每个主条目后清零，HID 规范 §6.2.2.8）
    std::vector<std::uint32_t> l_usages;
    std::optional<std::uint32_t> l_usage_min, l_usage_max;

    // 集合树；stack 只观察已挂到树上的节点（所有权在 info.collections）
    std::vector<CollectionNode*> stack;

    while (i < n) {
        const std::uint8_t prefix = data[i++];

        if (prefix == LONG_ITEM_PREFIX) {
            // 长条目：FE <size> <tag> <data...>，仅跳过
            if (i + 2 > n)
                throw DescriptorError("偏移 " + std::to_string(i - 1) + ": 长条目头被截断");
            const std::size_t lsize = data[i];
            i += 2 + lsize;
            if (i > n)
                throw DescriptorError("偏移 " + std::to_string(i - 2) + ": 长条目数据被截断");
            continue;
        }

        const std::size_t size = ITEM_SIZE[prefix & 0x3];
        const std::uint8_t item_type = (prefix >> 2) & 0x3;
        const std::uint8_t tag = (prefix >> 4) & 0xF;

        if (i + size > n)
            throw DescriptorError("偏移 " + std::to_string(i - 1) + ": 条目数据被截断（需 "
                                  + std::to_string(size) + " 字节）");
        const std::uint8_t* payload = data.data() + i;
        i += size;

        if (item_type == 0) {                                   // ---- Main ----
            if (const char* rtype = main_report_type(tag)) {
                const auto key = std::make_pair(std::string(rtype), static_cast<int>(g_report_id));
                int offset = 0;
                if (auto it = info.report_bits.find(key); it != info.report_bits.end())
                    offset = it->second;

                ReportField f;
                f.report_type = rtype;
                f.report_id = static_cast<int>(g_report_id);
                f.offset = offset;
                f.size = static_cast<int>(g_report_size);
                f.count = static_cast<int>(g_report_count);
                f.usages.reserve(l_usages.size());
                for (std::uint32_t u : l_usages) f.usages.push_back(resolve_usage(u, g_page));
                if (!f.usages.empty())
                    f.usage = f.usages[0];
                else if (l_usage_min)
                    f.usage = resolve_usage(*l_usage_min, g_page);
                if (l_usage_min) f.usage_min = resolve_usage(*l_usage_min, g_page);
                if (l_usage_max) f.usage_max = resolve_usage(*l_usage_max, g_page);
                f.logical_min = g_logical_min;
                f.logical_max = g_logical_max;
                f.flags = static_cast<int>(unsigned_le(payload, size));
                f.is_constant = (f.flags & 0x1) != 0;

                info.fields.push_back(f);
                if (!stack.empty()) stack.back()->children.emplace_back(f);
                info.report_bits[key] = offset + static_cast<int>(g_report_size * g_report_count);
            } else if (tag == 0xA) {                            // Collection
                const std::uint32_t value = size ? unsigned_le(payload, size) : 0;
                auto node = std::make_unique<CollectionNode>();
                node->kind = collection_kind(value) ? collection_kind(value)
                                                    : "0x" + hex(value, 2);
                if (!l_usages.empty()) node->usage = l_usages[0];
                CollectionNode* raw = node.get();
                if (!stack.empty()) {
                    stack.back()->children.emplace_back(std::move(node));
                } else {
                    info.collections.push_back(std::move(node));
                    if (raw->usage) info.application_usages.push_back(*raw->usage);
                }
                stack.push_back(raw);
            } else if (tag == 0xC) {                            // End Collection
                if (stack.empty())
                    throw DescriptorError("偏移 " + std::to_string(i - 1)
                                          + ": End Collection 无匹配的 Collection");
                stack.pop_back();
            }
            // 其余主条目 tag（0x7+ 保留位）忽略；主条目后清空局部状态
            l_usages.clear();
            l_usage_min.reset();
            l_usage_max.reset();

        } else if (item_type == 1) {                            // ---- Global ----
            switch (tag) {
                case 0x0:                                       // Usage Page
                    g_page = unsigned_le(payload, size);
                    info.usage_pages.insert(g_page);
                    if (g_page >= 0xFF00) info.has_vendor_page = true;
                    break;
                case 0x1: g_logical_min = signed_le(payload, size); break;  // Logical Minimum
                case 0x2: g_logical_max = signed_le(payload, size); break;  // Logical Maximum
                case 0x7: g_report_size = unsigned_le(payload, size); break;  // Report Size
                case 0x8: g_report_id = unsigned_le(payload, size); break;    // Report ID
                case 0x9: g_report_count = unsigned_le(payload, size); break; // Report Count
                case 0xA:                                       // Push
                    g_stack.push_back({g_page, g_report_size, g_report_id, g_report_count,
                                       g_logical_min, g_logical_max});
                    break;
                case 0xB:                                       // Pop
                    if (!g_stack.empty()) {
                        const Global g = g_stack.back();
                        g_stack.pop_back();
                        g_page = g.page;
                        g_report_size = g.size;
                        g_report_id = g.id;
                        g_report_count = g.count;
                        g_logical_min = g.lmin;
                        g_logical_max = g.lmax;
                    }
                    break;
                default: break;  // 0x3-0x6 / 0xC-0xF 保留位忽略
            }

        } else {                                                // ---- Local ----
            const std::uint32_t value = unsigned_le(payload, size);
            if (tag == 0x0)                                     // Usage
                l_usages.push_back(resolve_usage(value, g_page));
            else if (tag == 0x1)                                // Usage Minimum
                l_usage_min = value;
            else if (tag == 0x2)                                // Usage Maximum
                l_usage_max = value;
            // 0x3+（Designator/String 实际量等）忽略
        }
    }

    if (!stack.empty())
        throw DescriptorError("描述符结束仍有未闭合的 Collection");
    return info;
}

std::vector<std::string> summary(const DescriptorInfo& info) {
    std::vector<std::string> lines;
    for (std::uint32_t usage : info.application_usages)
        lines.push_back("顶层集合: " + format_usage(usage));
    for (const auto& [key, bits] : info.report_bits) {
        const std::string rid = key.second ? std::to_string(key.second) : "-";
        lines.push_back(key.first + " report id=" + rid + ": " + std::to_string(bits)
                        + " bits (" + std::to_string((bits + 7) / 8) + " bytes)");
    }
    if (info.has_vendor_page) {
        std::string pages;
        for (std::uint32_t p : info.usage_pages) {  // set 已排序，等价 Python sorted()
            if (p < 0xFF00) continue;
            if (!pages.empty()) pages += ", ";
            pages += "0x" + hex(p, 4);
        }
        lines.push_back("⚠ 厂商自定义 Usage Page: " + pages + "（可能含配置功能）");
    }
    return lines;
}

}  // namespace pt

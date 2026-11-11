// probe.cpp — vendor 通道探测实现（core/probe.py 平移）。
#include "probe.hpp"

namespace pt {

std::vector<int> feature_report_ids(const DescriptorInfo& info) {
    std::vector<int> ids;
    for (const auto& f : info.fields) {
        if (f.report_type == "feature" && !f.is_constant
            && std::find(ids.begin(), ids.end(), f.report_id) == ids.end())
            ids.push_back(f.report_id);
    }
    return ids.empty() ? std::vector<int>{0} : ids;
}

ProbeResult probe_interface(const std::string& path, int usage_page, int usage,
                            const ReadDescriptorFn& read_descriptor, DevFactory factory) {
    ProbeResult result;
    result.usage_page = usage_page;
    result.usage = usage;
    const Bytes raw = read_descriptor(path);
    if (raw.empty()) {
        result.error = "描述符读取失败（设备被系统占用或接口不可读）";
        return result;
    }
    result.descriptor_len = static_cast<int>(raw.size());
    const auto info = parse_report_descriptor(raw);
    result.feature_ids = feature_report_ids(info);

    ReportIO io(path, std::move(factory));
    io.open();
    for (int report_id : result.feature_ids) {
        try {
            result.reads[std::to_string(report_id)] = io.get_feature(report_id, 64);
        } catch (...) {
            result.reads[std::to_string(report_id)] = std::nullopt;
        }
    }
    return result;
}

std::vector<ProbeResult> probe_vendor_interfaces(const std::vector<DeviceInfo>& ifaces,
                                                 const ReadDescriptorFn& read_descriptor,
                                                 DevFactory factory) {
    std::vector<ProbeResult> results;
    for (const auto& d : ifaces) {
        if (d.usage_page >= 0xFF00)
            results.push_back(probe_interface(d.path, d.usage_page, d.usage, read_descriptor,
                                              factory));
    }
    return results;
}

std::vector<ProbeResult> suspect_rawhid_interfaces(const std::vector<DeviceInfo>& ifaces,
                                                   const ReadDescriptorFn& read_descriptor,
                                                   DevFactory factory) {
    std::vector<ProbeResult> results;
    for (const auto& d : ifaces) {
        if (d.usage_page != 0x0001 || d.usage != 0x0000) continue;
        const Bytes raw = read_descriptor(d.path);
        if (raw.empty()) continue;
        const auto info = parse_report_descriptor(raw);
        const auto big_field = [](const ReportField& f) {
            return !f.is_constant && f.size * f.count >= 512;
        };
        const bool has_input = std::any_of(info.fields.begin(), info.fields.end(),
                                           [&](const ReportField& f) {
                                               return f.report_type == "input" && big_field(f);
                                           });
        const bool has_output = std::any_of(info.fields.begin(), info.fields.end(),
                                            [&](const ReportField& f) {
                                                return f.report_type == "output" && big_field(f);
                                            });
        if (!has_input || !has_output) continue;
        ReportIO io(d.path, std::move(factory));
        io.open();
        const auto pushed = io.read_input(200);  // 被动抓 200ms 内设备主动推送
        ProbeResult r;
        r.usage_page = d.usage_page;
        r.usage = d.usage;
        r.descriptor_len = static_cast<int>(raw.size());
        r.reads["input_passive"] = pushed;
        r.note = "rawhid 疑似配置通道（64字节 in/out，写入能力待 M4 学习模式验证）";
        results.push_back(std::move(r));
    }
    return results;
}

}  // namespace pt

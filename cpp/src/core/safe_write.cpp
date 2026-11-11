// safe_write.cpp — 写安全机制实现（core/safe_write.py 平移）。
#include "safe_write.hpp"

#include <cstdio>
#include <ctime>

namespace pt {

namespace {

void log_step(WriteOutcome& out, const std::string& message) {
    char ts[16];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    out.log.push_back("[" + std::string(ts) + "] " + message);
}

// feature params 定义 + 调用者覆盖 → 完整参数（缺省回填 default）。
Params merged_params(const Json& feature, const Params* params) {
    Params merged;
    if (params) merged = *params;
    if (feature.contains("params")) {
        for (auto it = feature["params"].begin(); it != feature["params"].end(); ++it)
            merged.emplace(it.key(), it.value().value("default", 0));
    }
    return merged;
}

std::string hex_or_none(const std::optional<Bytes>& data) {
    return data ? to_hex(*data) : "<无响应>";
}

std::string hex_id(int report_id) { return "0x" + hex(static_cast<unsigned>(report_id), 2); }

std::optional<Bytes> read_via_feature(ReportIO& io, const Json& read_feature,
                                      const Params& params, WriteOutcome& out, int timeout_ms) {
    const auto result = execute_feature(io, read_feature, &params, timeout_ms);
    log_step(out, "读(" + read_feature.value("id", std::string("?")) + "): "
                      + hex_or_none(result.response));
    return result.response;
}

bool expect_ok(const std::optional<Bytes>& readback, const ByteExpect& expect) {
    if (!readback) return false;
    for (const auto& [index, byte] : expect) {
        if (index < 0 || index >= static_cast<int>(readback->size())) return false;
        if ((*readback)[static_cast<std::size_t>(index)] != static_cast<std::uint8_t>(byte))
            return false;
    }
    return true;
}

}  // namespace

WriteOutcome safe_write(ReportIO& io, const Json& write_feature, const Json* read_feature,
                        const Params* new_params, const ByteExpect* expect,
                        const Params* rollback_params, int timeout_ms) {
    WriteOutcome out;
    const std::string kind = write_feature.value("kind", std::string());
    const std::string report = write_feature.value("report", std::string());

    if (kind != "write") {
        log_step(out, "拒绝: feature kind='" + kind + "' 不是 write（安全机制只放行 write）");
        return out;
    }
    if (report != "feature" && read_feature == nullptr) {
        log_step(out, "拒绝: output 报文写入必须配对 read 功能（无法读回验证）");
        return out;
    }

    // ---- 1. 读旧值 ----
    Params read_params;
    int report_id = 0, size = 0;
    if (report == "feature") {
        const Params merged = merged_params(write_feature, new_params);
        Bytes data;
        try {
            data = render_template(write_feature["template"], merged);
        } catch (const std::exception& exc) {
            log_step(out, "拒绝: 模板渲染失败 " + std::string(exc.what()));
            return out;
        }
        if (data.empty()) {
            log_step(out, "拒绝: 模板渲染结果为空");
            return out;
        }
        report_id = data[0];
        size = static_cast<int>(data.size());
        out.old_value = io.get_feature(report_id, size);
        log_step(out, "读旧值(feature id=" + hex_id(report_id) + "): " + hex_or_none(out.old_value));
    } else {
        // 只传 read_feature 里定义过的参数
        if (read_feature && read_feature->contains("params") && new_params) {
            for (const auto& [k, v] : *new_params)
                if (read_feature->at("params").contains(k)) read_params[k] = v;
        }
        out.old_value = read_via_feature(io, *read_feature, read_params, out, timeout_ms);
    }

    // ---- 2. 写新值 ----
    Bytes data;
    try {
        data = render_template(write_feature["template"],
                               merged_params(write_feature, new_params));
    } catch (const std::exception& exc) {
        log_step(out, "中止: 新值模板渲染失败 " + std::string(exc.what()));
        return out;
    }
    out.wrote = data;
    if (report == "feature") {
        io.send_feature(data);
        log_step(out, "写(feature): " + to_hex(data));
    } else {
        io.write_output(data);
        log_step(out, "写(output): " + to_hex(data));
    }

    // ---- 3. 读回验证 ----
    if (report == "feature") {
        out.readback = io.get_feature(report_id, size);
        log_step(out, "读回(feature id=" + hex_id(report_id) + "): " + hex_or_none(out.readback));
    } else {
        out.readback = read_via_feature(io, *read_feature, read_params, out, timeout_ms);
    }

    if (expect != nullptr) {
        out.verified = expect_ok(out.readback, *expect);
        std::string expect_text;
        for (const auto& [i, byte] : *expect) {
            if (!expect_text.empty()) expect_text += ", ";
            expect_text += std::to_string(i) + ": '" + hex(static_cast<unsigned char>(byte), 2) + "'";
        }
        log_step(out, std::string("验证: ") + (out.verified ? "通过" : "失败")
                          + " (期望 {" + expect_text + "})");
        if (!out.verified && out.old_value) {
            // ---- 4. 回滚旧值 ----
            std::optional<Bytes> confirm;
            if (report == "feature") {
                log_step(out, "回滚: 重写旧值 " + to_hex(*out.old_value));
                io.send_feature(*out.old_value);
                confirm = io.get_feature(report_id, size);
                out.rolled_back = (confirm == out.old_value);
            } else if (rollback_params != nullptr) {
                std::optional<Bytes> old_data;
                try {
                    old_data = render_template(
                        write_feature["template"],
                        merged_params(write_feature, rollback_params));
                } catch (const std::exception& exc) {
                    log_step(out, "回滚失败: 旧值模板渲染失败 " + std::string(exc.what()));
                }
                if (old_data) {
                    log_step(out, "回滚: 重写旧值 " + to_hex(*old_data));
                    io.write_output(*old_data);
                    confirm = read_via_feature(io, *read_feature, read_params, out, timeout_ms);
                    out.rolled_back = confirm.has_value();
                }
            } else {
                log_step(out, "回滚失败: output 路径未提供 rollback_params（无法构造旧值报文）");
            }
            log_step(out, std::string("回滚") + (out.rolled_back ? "成功" : "失败")
                              + ": 读回 " + hex_or_none(confirm));
        } else if (!out.verified) {
            log_step(out, "无旧值可回滚（读旧值阶段未取得响应）");
        }
    } else {
        log_step(out, "未提供 expect：已写入但无法自动验证（M2 只读原则下属正常调试流程）");
    }

    out.ok = out.readback.has_value() && (expect == nullptr || out.verified);
    return out;
}

}  // namespace pt

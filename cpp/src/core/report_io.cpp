// report_io.cpp — 引擎实现（core/report_io.py 平移）。
#include "report_io.hpp"

#include <algorithm>
#include <chrono>
#include <map>

#include "hid_api.hpp"

namespace pt {

namespace {

// 模板元素 → 一个字节。int 直接用；"{param}" 从 params 解析；"{#algo}" 计算。
int resolve_byte(const Json& element, const Params* params, const std::string& what,
                 const Bytes& rendered = {}) {
    int value;
    if (element.is_string()) {
        const std::string s = element.get<std::string>();
        if (is_placeholder(s)) {
            const std::string name = s.substr(1, s.size() - 2);
            if (!params || params->find(name) == params->end())
                throw RenderError(what + ": 参数 \"" + name + "\" 未提供");
            value = params->at(name);
        } else if (auto algo = computed_algo(s)) {
            if (*algo == "negsum") {  // 校验和 = 0xFF-(前面字节之和&0xFF)（ROYUAN 等方案）
                int sum = 0;
                for (std::uint8_t b : rendered) sum += b;
                return 0xFF - (sum & 0xFF);
            }
            throw RenderError(what + ": 未知算法 \"" + s + "\"");
        } else {
            throw RenderError(what + ": 非法占位符 \"" + s + "\"");
        }
    } else {
        if (!json_is_byte(element))
            throw RenderError(what + ": 值不是合法字节 (0-255)");
        value = element.get<int>();
    }
    if (value < 0 || value > 255)
        throw RenderError(what + ": 值不是合法字节 (0-255)");
    return value;
}

// checks: [(label, check)]，返回命中的下标或 -1。
int match_branches(const Bytes& response, const ReportIO::Checks& checks, const Params* params) {
    for (std::size_t i = 0; i < checks.size(); ++i) {
        if (checks[i].second.is_null() || match_response(response, checks[i].second, params))
            return static_cast<int>(i);
    }
    return -1;
}

std::map<std::string, std::optional<int>> extract_or_empty(const Bytes& response,
                                                           const Json* spec) {
    if (spec && spec->is_object() && spec->contains("extract"))
        return extract_fields(response, (*spec)["extract"]);
    return {};
}

// report=feature 路径：send 后立即 get 同 ID 读回，按分支匹配（无超时轮询）。
FeatureResult execute_feature_report(ReportIO& io, const Bytes& data,
                                     const ReportIO::Checks& checks, const Json* response_spec,
                                     const Json* alt, const Params& params) {
    io.open();
    io.send_feature(data);
    const int min_len = response_spec ? response_spec->value("min_len",
                                                            static_cast<int>(data.size()))
                                      : static_cast<int>(data.size());
    const int size = std::max(static_cast<int>(data.size()), min_len);
    const int report_id = data.empty() ? 0 : data[0];
    const auto response = io.get_feature(report_id, size);
    if (!response)
        return FeatureResult{false, std::nullopt, std::nullopt, {}};
    const int hit = match_branches(*response, checks, &params);
    if (hit < 0)
        // feature 命令的响应不匹配任何分支：原样返回未匹配响应
        return FeatureResult{false, response, std::optional<std::string>("未匹配"), {}};
    const auto& label = checks[static_cast<std::size_t>(hit)].first;
    if (label == "primary")
        return FeatureResult{true, response, std::nullopt, extract_or_empty(*response, response_spec)};
    return FeatureResult{false, response, label, extract_or_empty(*response, alt)};
}

FeatureResult execute_output_report(ReportIO& io, const Bytes& data,
                                    const ReportIO::Checks& checks, const Json* response_spec,
                                    const Json* alt, const Params& params, int timeout_ms,
                                    bool listen_only) {
    std::optional<std::pair<Bytes, std::string>> result;
    if (listen_only)
        result = io.read_until(checks, &params, timeout_ms);   // 监听型：零写入
    else
        result = io.call_multi(data, checks, &params, timeout_ms);
    if (!result)
        return FeatureResult{false, std::nullopt, std::nullopt, {}};
    const auto& [response, label] = *result;
    if (label == "primary")
        return FeatureResult{true, response, std::nullopt, extract_or_empty(response, response_spec)};
    return FeatureResult{false, response, label, extract_or_empty(response, alt)};
}

// ---- 真实 hidapi 设备（DevFactory 默认实现） ----

class HidapiDevice final : public HidDevice {
public:
    explicit HidapiDevice(const std::string& path) : handle_(hid::open_path(path)) {}

    int write(const Bytes& data) override { return hid::write(handle_.get(), data); }

    Bytes read(int timeout_ms, int size) override {
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(size), 0);
        const int n = hid::read_timeout(handle_.get(), buf.data(), size, timeout_ms);
        if (n <= 0) return {};
        buf.resize(static_cast<std::size_t>(n));
        return buf;
    }

    int send_feature(const Bytes& data) override { return hid::send_feature(handle_.get(), data); }

    Bytes get_feature(int report_id, int size) override {
        std::vector<std::uint8_t> buf(static_cast<std::size_t>(size), 0);
        if (!buf.empty()) buf[0] = static_cast<std::uint8_t>(report_id);
        const int n = hid::get_feature(handle_.get(), buf.data(), size);
        if (n < 0) return {};
        buf.resize(static_cast<std::size_t>(n));
        return buf;
    }

private:
    hid::Handle handle_;
};

}  // namespace

DevFactory hidapi_factory() {
    return [](const std::string& path) -> std::unique_ptr<HidDevice> {
        return std::make_unique<HidapiDevice>(path);
    };
}

// ---------------------------------------------------------------------------
// 纯函数
// ---------------------------------------------------------------------------

Bytes render_template(const Json& template_, const Params& params) {
    Bytes out;
    out.reserve(template_.size());
    int i = 0;
    for (const Json& element : template_) {
        out.push_back(static_cast<std::uint8_t>(
            resolve_byte(element, &params, "template[" + std::to_string(i++) + "]", out)));
    }
    return out;
}

bool match_response(const Bytes& response, const Json& check, const Params* params) {
    if (response.size() < check.size()) return false;  // 长度不足直接 False
    std::size_t i = 0;
    for (const Json& want : check) {
        const std::size_t idx = i++;
        if (want.is_null()) continue;
        if (want.is_string()) {
            const int resolved = resolve_byte(want, params, "check[" + std::to_string(idx) + "]");
            if (static_cast<int>(response[idx]) != resolved) return false;
            continue;
        }
        if (want.is_array()) {
            bool found = false;
            for (const Json& v : want)
                if (v.is_number_integer() && v.get<int>() == static_cast<int>(response[idx])) {
                    found = true;
                    break;
                }
            if (!found) return false;
            continue;
        }
        if (!want.is_number_integer() || want.get<int>() != static_cast<int>(response[idx]))
            return false;
    }
    return true;
}

std::map<std::string, std::optional<int>> extract_fields(const Bytes& response,
                                                         const Json& extract) {
    std::map<std::string, std::optional<int>> out;
    for (auto it = extract.begin(); it != extract.end(); ++it) {
        const int index = it.value().get<int>();
        out[it.key()] = index < static_cast<int>(response.size())
                            ? std::optional<int>(response[static_cast<std::size_t>(index)])
                            : std::nullopt;  // 越界给 None
    }
    return out;
}

// ---------------------------------------------------------------------------
// ReportIO
// ---------------------------------------------------------------------------

ReportIO::ReportIO(std::string path, DevFactory factory, HistoryCb on_history)
    : path_(std::move(path)), factory_(std::move(factory)),
      on_history_(std::move(on_history)) {}

ReportIO::~ReportIO() { close(); }

void ReportIO::open() {
    if (dev_) return;
    try {
        dev_ = factory_(path_);
    } catch (const std::exception& exc) {
        throw ReportError("打开设备失败 (" + path_ + "): " + exc.what());
    }
}

void ReportIO::close() {
    if (!dev_) return;
    try {
        dev_->close();
    } catch (...) {  // 关闭失败不影响清理
    }
    dev_.reset();
}

void ReportIO::log(const char* direction, const Bytes& data) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    HistoryEntry entry{std::chrono::duration<double>(now).count(), direction, data};
    history.push_back(std::move(entry));
    if (on_history_) {
        try {
            on_history_(history.back());
        } catch (...) {  // 订阅方异常不影响设备通信
        }
    }
}

int ReportIO::write_output(const Bytes& data) {
    open();
    const int written = dev_->write(data);
    if (written < 0)
        throw ReportError("write 失败: " + std::to_string(written));
    log("tx", data);
    return written;
}

std::optional<Bytes> ReportIO::read_input(int timeout_ms, int size) {
    open();
    Bytes data = dev_->read(timeout_ms, size);
    if (data.empty()) return std::nullopt;
    log("rx", data);
    return data;
}

void ReportIO::send_feature(const Bytes& data) {
    open();
    const int result = dev_->send_feature(data);
    if (result < 0)
        throw ReportError("send_feature_report 失败: " + std::to_string(result));
    log("tx", data);
}

std::optional<Bytes> ReportIO::get_feature(int report_id, int size) {
    open();
    Bytes data = dev_->get_feature(report_id, size);
    if (data.empty()) return std::nullopt;
    log("rx", data);
    return data;
}

std::optional<Bytes> ReportIO::call(const Bytes& data, const Json* check, const Params* params,
                                    int timeout_ms) {
    Checks checks;
    checks.emplace_back("primary", check ? *check : Json());
    auto result = call_multi(data, checks, params, timeout_ms);
    if (!result) return std::nullopt;
    return result->first;
}

std::optional<std::pair<Bytes, std::string>> ReportIO::call_multi(const Bytes& data,
                                                                  const Checks& checks,
                                                                  const Params* params,
                                                                  int timeout_ms) {
    write_output(data);
    return read_until(checks, params, timeout_ms);
}

std::optional<std::pair<Bytes, std::string>> ReportIO::read_until(const Checks& checks,
                                                                  const Params* params,
                                                                  int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(timeout_ms);
    while (true) {
        const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      deadline - std::chrono::steady_clock::now())
                                      .count();
        if (remaining_ms <= 0) return std::nullopt;
        auto response = read_input(static_cast<int>(std::min<long long>(remaining_ms, 50)));
        if (response) {
            for (const auto& [label, check] : checks) {
                if (check.is_null() || match_response(*response, check, params))
                    return std::make_pair(std::move(*response), label);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// execute_feature
// ---------------------------------------------------------------------------

FeatureResult execute_feature(ReportIO& io, const Json& feature, const Params* params,
                              int timeout_ms) {
    Params merged;
    if (params) merged = *params;
    if (feature.contains("params")) {
        for (auto it = feature["params"].begin(); it != feature["params"].end(); ++it)
            merged.emplace(it.key(), it.value().value("default", 0));
    }
    const Bytes data = render_template(feature["template"], merged);

    const Json* response_spec =
        feature.contains("response") ? &feature["response"] : nullptr;
    const Json* alt =
        response_spec && response_spec->contains("alt") ? &(*response_spec)["alt"] : nullptr;

    ReportIO::Checks checks;
    if (response_spec && response_spec->contains("check")) {
        checks.emplace_back("primary", (*response_spec)["check"]);
    } else if (!alt) {
        checks.emplace_back("primary", Json());  // null = 匹配第一个响应
    }
    if (alt) {
        std::string label = "alt";
        if (alt->contains("label") && alt->at("label").is_string()) {
            const std::string l = alt->at("label").get<std::string>();
            if (!l.empty()) label = l;  // 对应 Python (alt.get("label") or "alt")
        }
        checks.emplace_back(std::move(label), (*alt)["check"]);
    }

    const std::string report = feature.value("report", std::string("output"));
    if (report == "feature")
        return execute_feature_report(io, data, checks, response_spec, alt, merged);
    const bool listen_only = report == "input";  // 监听型：零写入
    return execute_output_report(io, data, checks, response_spec, alt, merged, timeout_ms,
                                 listen_only);
}

}  // namespace pt

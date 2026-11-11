// fingerprint.cpp — 协议库加载/校验/指纹实现（core/fingerprint.py 平移）。
#include "fingerprint.hpp"

#include <cstdio>
#include <fstream>

namespace pt {

namespace {

// ---- schema v2 protocol 块校验（_validate_protocol 及其子函数） ----

void validate_match(const std::string& where, const Json& match) {
    for (const char* key : {"usage_page", "usage"}) {
        const std::string value = match.value(key, std::string());
        if (!is_hex4(value))
            throw ProtocolDBError(where + "." + key + ": 须为 4 位十六进制字符串");
    }
}

void validate_params(const std::string& where, const Json* params) {
    if (!params) return;
    if (!params->is_object())
        throw ProtocolDBError(where + ".params: 必须是 object");
    for (auto it = params->begin(); it != params->end(); ++it) {
        const std::string pwhere = where + ".params." + it.key();
        if (!it.value().is_object() || it.value().value("type", std::string()) != "int")
            throw ProtocolDBError(pwhere + ": 仅支持 {'type': 'int', ...}");
        for (const char* key : {"default", "min", "max"}) {
            if (it.value().contains(key) && !it.value()[key].is_number_integer())
                throw ProtocolDBError(pwhere + "." + key + ": 必须是整数");
        }
        if (it.value().contains("min") && it.value().contains("max")
            && it.value()["min"].get<long long>() > it.value()["max"].get<long long>())
            throw ProtocolDBError(pwhere + ": min > max");
    }
}

void validate_response(const std::string& where, const Json& response) {
    if (!response.is_object())
        throw ProtocolDBError(where + ".response: 必须是 object");
    // 主分支 + 可选 alt 分支
    std::vector<std::pair<std::string, const Json*>> branches{{"response", &response}};
    if (response.contains("alt"))
        branches.emplace_back("response.alt", &response["alt"]);
    for (const auto& [path, branch] : branches) {
        if (!branch->is_object())
            throw ProtocolDBError(where + "." + path + ": 必须是 object");
        if (!branch->contains("check"))
            throw ProtocolDBError(where + "." + path + ": 缺少 check");
        for (const Json& element : (*branch)["check"]) {
            bool ok = element.is_null() || json_is_byte(element)
                      || (element.is_string() && is_placeholder(element.get<std::string>()));
            if (element.is_array()) {
                ok = true;
                for (const Json& v : element)
                    if (!json_is_byte(v)) ok = false;
            }
            if (!ok)
                throw ProtocolDBError(where + "." + path + ".check: 非法元素");
        }
        if (branch->contains("extract")) {
            for (auto ex = branch->at("extract").begin(); ex != branch->at("extract").end(); ++ex) {
                if (!ex.value().is_number_integer() || ex.value() < 0)
                    throw ProtocolDBError(where + "." + path + ".extract." + ex.key()
                                          + ": 必须是非负整数索引");
            }
        }
    }
}

void validate_protocol(const std::string& vid_pid, const Json& protocol) {
    const std::string where = "devices[" + vid_pid + "].protocol";
    if (!protocol.is_object())
        throw ProtocolDBError(where + ": 必须是 object");
    for (const char* key : {"name", "transport", "features"})
        if (!protocol.contains(key))
            throw ProtocolDBError(where + ": 缺少 " + key);
    if (!protocol["name"].is_string())
        throw ProtocolDBError(where + ".name: 必须是字符串");
    const Json& features = protocol["features"];
    if (!features.is_array() || features.empty())
        throw ProtocolDBError(where + ".features: 必须是非空数组");

    const Json& transport = protocol["transport"];
    if (!transport.is_object())
        throw ProtocolDBError(where + ".transport: 必须是 object");
    if (!transport.contains("match") || !transport["match"].is_object())
        throw ProtocolDBError(where + ".transport.match: 必须是 object");
    validate_match(where + ".transport.match", transport["match"]);

    static const char* VALID_KINDS[] = {"command", "read", "write"};
    static const char* VALID_REPORTS[] = {"output", "feature", "input"};  // input=监听型

    int index = 0;
    for (const Json& feature : features) {
        const std::string fid = feature.is_object() && feature.contains("id")
                                    ? feature["id"].get<std::string>() : "?";
        const std::string fwhere = where + ".features[" + std::to_string(index++) + "](" + fid + ")";
        if (!feature.is_object())
            throw ProtocolDBError(fwhere + ": 必须是 object");
        for (const char* key : {"id", "label", "kind", "report", "template"})
            if (!feature.contains(key))
                throw ProtocolDBError(fwhere + ": 缺少 " + key);
        bool kind_ok = false, report_ok = false;
        for (const char* k : VALID_KINDS) kind_ok = kind_ok || feature["kind"] == k;
        for (const char* r : VALID_REPORTS) report_ok = report_ok || feature["report"] == r;
        if (!kind_ok)
            throw ProtocolDBError(fwhere + ".kind: 必须是 command/read/write 之一");
        if (!report_ok)
            throw ProtocolDBError(fwhere + ".report: 必须是 output/feature/input 之一");

        const Json& template_ = feature["template"];
        if (!template_.is_array())
            throw ProtocolDBError(fwhere + ".template: 必须是数组");
        for (const Json& element : template_) {
            bool valid = json_is_byte(element);
            if (element.is_string()) {
                const std::string s = element.get<std::string>();
                valid = valid || is_placeholder(s);
                if (auto algo = computed_algo(s))
                    valid = valid || (*algo == "negsum");  // 计算型占位符当前仅 negsum
            }
            if (!valid)
                throw ProtocolDBError(fwhere + ".template: 元素须为 0-255 整数、{参数} 或 {#算法} 占位符");
        }
        if (feature["kind"] == "write" && !feature.contains("read_from"))
            throw ProtocolDBError(fwhere + ": kind=write 必须提供 read_from（读回验证用）");
        if (feature.contains("match"))  // feature 级接口覆盖（多接口设备）
            validate_match(fwhere + ".match", feature["match"]);
        if (feature.contains("params"))
            validate_params(fwhere, &feature["params"]);
        if (feature.contains("response"))
            validate_response(fwhere, feature["response"]);
    }
}

}  // namespace

std::filesystem::path default_db_path() {
    if (const char* env = std::getenv("PERIPHERAL_TOOL_DB"))
        return std::filesystem::path(env);
    // 分发版：exe 同级 protocol_db/db.json（安装包布局）
    std::error_code ec;
    const auto beside_exe = exe_dir() / "protocol_db" / "db.json";
    if (std::filesystem::is_regular_file(beside_exe, ec)) return beside_exe;
#ifdef PT_SOURCE_ROOT
    return std::filesystem::path(PT_SOURCE_ROOT) / ".." / "protocol_db" / "db.json";
#else
    return std::filesystem::path("protocol_db") / "db.json";
#endif
}

const char* status_mark(DeviceStatus s) {
    switch (s) {
        case DeviceStatus::Known: return "OK ";
        case DeviceStatus::Sdk: return "SDK";
        default: return "???";
    }
}

const char* status_value(DeviceStatus s) {
    switch (s) {
        case DeviceStatus::Known: return "known";
        case DeviceStatus::Sdk: return "sdk";
        default: return "unknown";
    }
}

void validate_db(const Json& db) {
    const std::string schema = db.value("$schema", "");
    if (schema.rfind("protocol-db/", 0) != 0)
        throw ProtocolDBError("不支持的 schema: \"" + schema + "\"");
    for (const char* key : {"brands", "devices"})
        if (!db.contains(key) || !db[key].is_object())
            throw ProtocolDBError(std::string("协议库缺少 ") + key + " 表或类型不是 object");
    const Json& devices = db["devices"];
    for (auto it = devices.begin(); it != devices.end(); ++it) {
        if (it.key().find(':') == std::string::npos)
            throw ProtocolDBError("devices 键必须是 VID:PID 格式: " + it.key());
        if (it.value().is_object() && it.value().contains("protocol"))
            validate_protocol(it.key(), it.value()["protocol"]);
    }
}

bool is_internal_device(const Json& db, const DeviceInfo& device) {
    const auto brands_it = db.find("brands");
    if (brands_it != db.end() && brands_it->is_object()) {
        const auto brand_it = brands_it->find(device.vid_hex());
        if (brand_it != brands_it->end() && brand_it->is_object()
            && brand_it->value("internal", false))
            return true;
    }
    return device.bus_type == 3;
}

Json load_db(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path))
        throw ProtocolDBError("协议库不存在: " + path.string());
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw ProtocolDBError("协议库不存在: " + path.string());
    const std::string text((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
    Json db;
    try {
        db = Json::parse(text);
    } catch (const Json::parse_error& exc) {
        throw ProtocolDBError(std::string("协议库 JSON 语法错误: ") + exc.what());
    }
    validate_db(db);
    return db;
}

void save_db(const Json& db, const std::filesystem::path& path) {
    validate_db(db);
    const std::string text = db.dump(2, ' ', false, Json::error_handler_t::replace) + "\n";
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f)
            throw ProtocolDBError("无法写入临时文件: " + tmp.string());
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f)
            throw ProtocolDBError("写入临时文件失败: " + tmp.string());
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);  // Windows 亦为原子替换
    if (ec) {
        std::filesystem::remove(tmp, ec);
        throw ProtocolDBError("替换协议库失败: " + ec.message());
    }
}

FingerprintResult fingerprint(const DeviceInfo& device, const Json& db) {
    const std::string key = device.vidpid();

    Json entry;  // null = 未命中
    const auto devices_it = db.find("devices");
    if (devices_it != db.end() && devices_it->is_object()) {
        const auto eit = devices_it->find(key);
        if (eit != devices_it->end()) entry = *eit;
    }

    std::optional<std::string> name, name_zh;
    std::vector<std::string> sdk;
    const auto brands_it = db.find("brands");
    if (brands_it != db.end() && brands_it->is_object()) {
        const auto bit = brands_it->find(device.vid_hex());
        if (bit != brands_it->end() && bit->is_object()) {
            if (auto n = bit->find("name"); n != bit->end() && n->is_string())
                name = n->get<std::string>();
            if (auto nz = bit->find("name_zh"); nz != bit->end() && nz->is_string())
                name_zh = nz->get<std::string>();
            if (auto sk = bit->find("sdk"); sk != bit->end() && sk->is_array())
                for (const auto& s : *sk)
                    if (s.is_string()) sdk.push_back(s.get<std::string>());
        }
    }

    if (!entry.is_null()) {
        const std::string verified = entry.value("verified", false) ? "已验证" : "未验证";
        return {DeviceStatus::Known, name, name_zh, std::move(sdk), std::move(entry),
                key + " 在协议库中（可直通，验证状态: " + verified + "）"};
    }
    if (!name)
        return {DeviceStatus::Unknown, std::nullopt, std::nullopt, {}, Json(),
                key + " 完全未知，可进入学习模式"};
    const std::string display = name_zh.value_or(*name);
    if (!sdk.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < sdk.size(); ++i) {
            if (i) joined += "/";
            joined += sdk[i];
        }
        return {DeviceStatus::Sdk, name, name_zh, std::move(sdk), Json(),
                "品牌 " + display + " 已知且有官方 SDK（" + joined + "），该具体型号暂无协议条目"};
    }
    return {DeviceStatus::Unknown, name, name_zh, {}, Json(),
            "品牌 " + display + " 已知，该型号暂无协议条目（可学习）"};
}

}  // namespace pt

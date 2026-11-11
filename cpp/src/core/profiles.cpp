// profiles.cpp — 配置档案实现（core/profiles.py 平移）。
#include "profiles.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

#include "common.hpp"

namespace pt {

namespace {
constexpr const char* SCHEMA = "device-profile/v1";
}  // namespace

std::filesystem::path default_profile_dir() {
    // 分发版判定：exe 同级存在 protocol_db/db.json（安装包布局）→ 档案放 exe 同级。
    // （不能用"exe 同级 profiles/ 目录已存在"判定：首次保存时它必然不存在，
    //   会错误回退到编译期写死的开发机源码路径，在用户机器上创建垃圾目录。）
    std::error_code ec;
    if (std::filesystem::is_regular_file(exe_dir() / "protocol_db" / "db.json", ec))
        return exe_dir() / "profiles";
#ifdef PT_SOURCE_ROOT
    return std::filesystem::path(PT_SOURCE_ROOT) / ".." / "profiles";
#else
    return std::filesystem::path("profiles");
#endif
}

std::string safe_name(const std::string& name) {
    std::string cleaned;
    for (char c : name) {
        switch (c) {
            case '\\': case '/': case ':': case '*': case '?': case '"':
            case '<': case '>': case '|':
                cleaned += '_';
                break;
            default:
                cleaned += c;
        }
    }
    // strip 首尾空白
    const auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t begin = 0, end = cleaned.size();
    while (begin < end && is_space(static_cast<unsigned char>(cleaned[begin]))) ++begin;
    while (end > begin && is_space(static_cast<unsigned char>(cleaned[end - 1]))) --end;
    cleaned = cleaned.substr(begin, end - begin);
    return cleaned.empty() ? "unnamed" : cleaned;
}

std::filesystem::path profile_dir(const std::string& vidpid,
                                  const std::filesystem::path& directory) {
    std::string sub = vidpid;
    std::replace(sub.begin(), sub.end(), ':', '_');
    return directory / sub;
}

std::filesystem::path save_profile(const DeviceProfile& profile,
                                   const std::filesystem::path& directory) {
    const auto target =
        profile_dir(profile.vidpid, directory) / (safe_name(profile.name) + ".json");
    std::error_code ec;
    std::filesystem::create_directories(target.parent_path(), ec);

    Json payload = Json::object();
    payload["$schema"] = SCHEMA;
    payload["vidpid"] = profile.vidpid;
    payload["name"] = profile.name;
    payload["created_ts"] = profile.created_ts;
    Json features = Json::array();
    for (const auto& f : profile.features) {
        Json params = Json::object();
        for (const auto& [k, v] : f.params) params[k] = v;
        features.push_back(Json{{"id", f.feature_id}, {"params", params}});
    }
    payload["features"] = features;

    std::ofstream f(target, std::ios::binary);
    if (!f) throw ProfileError("无法写入档案: " + target.string());
    f << payload.dump(2, ' ', false, Json::error_handler_t::replace) << "\n";
    return target;
}

DeviceProfile load_profile(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw ProfileError("档案不存在: " + path.string());
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Json data;
    try {
        data = Json::parse(text);
    } catch (const Json::parse_error& exc) {
        throw ProfileError(std::string("档案 JSON 语法错误: ") + exc.what());
    }
    if (!data.is_object() || data.value("$schema", std::string()) != SCHEMA)
        throw ProfileError("不支持的档案 schema: " + data.value("$schema", std::string("(无)")));
    if (!data.contains("vidpid") || !data["vidpid"].is_string()
        || data["vidpid"].get<std::string>().find(':') == std::string::npos)
        throw ProfileError("档案缺少合法 vidpid");

    DeviceProfile profile;
    profile.vidpid = data["vidpid"].get<std::string>();
    profile.name = data.contains("name") && data["name"].is_string() && !data["name"].empty()
                       ? data["name"].get<std::string>()
                       : path.stem().string();
    profile.created_ts = data.value("created_ts", 0.0);
    if (data.contains("features") && data["features"].is_array()) {
        for (const auto& f : data["features"]) {
            ProfileFeature pf;
            pf.feature_id = f.value("id", std::string(""));
            if (f.contains("params") && f["params"].is_object())
                for (auto it = f["params"].begin(); it != f["params"].end(); ++it)
                    pf.params[it.key()] = it.value().get<int>();
            profile.features.push_back(std::move(pf));
        }
    }
    return profile;
}

std::vector<std::filesystem::path> list_profiles(const std::string& vidpid,
                                                 const std::filesystem::path& directory) {
    const auto base = profile_dir(vidpid, directory);
    std::error_code ec;
    if (!std::filesystem::is_directory(base, ec)) return {};
    std::vector<std::filesystem::path> out;
    for (const auto& p : std::filesystem::directory_iterator(base, ec))
        if (p.path().extension() == ".json") out.push_back(p.path());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return std::filesystem::last_write_time(a) > std::filesystem::last_write_time(b);
    });
    return out;
}

}  // namespace pt

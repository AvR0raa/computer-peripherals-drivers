// wizard.cpp — 学习草稿生成与合并实现（core/wizard.py 平移）。
#include "wizard.hpp"

#include <algorithm>
#include <cctype>

namespace pt {

Json build_drafts(const std::vector<GroupDiff>& diffs, const std::string& note,
                  const Json& label_match, int max_changes) {
    Json drafts = Json::array();
    for (const auto& d : diffs) {
        if (d.is_volatile) continue;
        const char* report;
        Json template_ = Json::array();
        if (d.kind == "feature_read") {
            report = "feature";
            template_.push_back(d.report_id);
        } else if (d.kind == "input") {
            report = "input";  // 监听型：零写入
        } else {
            continue;
        }
        Json changes = Json::array();
        const int n = std::min<int>(static_cast<int>(d.changes.size()), max_changes);
        for (int i = 0; i < n; ++i) {
            const auto& c = d.changes[static_cast<std::size_t>(i)];
            changes.push_back(Json::array({c.offset, c.before ? Json(*c.before) : Json(),
                                           c.after ? Json(*c.after) : Json()}));
        }
        const int min_len = std::max(d.after_len, 1);
        Json wildcard_check = Json::array();
        for (int i = 0; i < min_len; ++i) wildcard_check.push_back(Json());  // null 通配

        // id: learned_<source 小写, ':'/' ' 换 '_'>_<report_id>
        std::string src = d.source;
        std::transform(src.begin(), src.end(), src.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::replace(src.begin(), src.end(), ':', '_');
        std::replace(src.begin(), src.end(), ' ', '_');

        Json draft = Json::object();
        draft["id"] = std::string(LEARNED_PREFIX) + src + "_" + std::to_string(d.report_id);
        draft["label"] = note.empty() ? "学习模式捕获" : note;
        draft["kind"] = "read";
        draft["report"] = report;
        draft["template"] = template_;
        Json response = Json::object();
        response["min_len"] = min_len;
        response["check"] = wildcard_check;  // 通配：草稿阶段不做严格匹配
        draft["response"] = response;
        Json learned = Json::object();
        learned["source"] = d.source;
        learned["changes"] = changes;
        learned["is_new"] = d.is_new;
        learned["before_len"] = d.before_len;
        learned["after_len"] = d.after_len;
        draft["learned"] = learned;
        if (label_match.is_object()) {
            const auto it = label_match.find(d.source);
            if (it != label_match.end()) draft["match"] = *it;
        }
        drafts.push_back(std::move(draft));
    }
    return drafts;
}

Json build_protocol_block(const Json& match, const std::vector<Json>& drafts) {
    Json block = Json::object();
    block["name"] = "learned";
    Json transport = Json::object();
    transport["match"] = match.is_object() && !match.empty()
                             ? match
                             : Json{{"usage_page", "0000"}, {"usage", "0000"}};
    transport["response_timeout_ms"] = 600;
    block["transport"] = transport;
    block["features"] = drafts;
    return block;
}

Json& merge_into_db(Json& db, const std::string& vid_pid, const Json& protocol_block,
                    const std::string& note) {
    Json& devices = db["devices"];
    Json& entry = devices.contains(vid_pid) ? devices[vid_pid] : (devices[vid_pid] = Json::object());
    if (!entry.contains("brand")) entry["brand"] = "Unknown";
    entry["source"] = "learning-mode";
    if (!entry.contains("verified") || entry["verified"] != true) entry["verified"] = false;
    if (!note.empty()) entry["note"] = "学习模式: " + note;

    if (!entry.contains("protocol")) {
        entry["protocol"] = protocol_block;
        return entry;
    }
    Json& existing = entry["protocol"];
    Json by_id = Json::object();
    if (existing.contains("features") && existing["features"].is_array())
        for (const auto& f : existing["features"])
            by_id[f.value("id", std::string("?"))] = f;
    for (const auto& feature : protocol_block["features"])
        by_id[feature.at("id").get<std::string>()] = feature;  // 同 id 覆盖（重学以新为准）
    Json features = Json::array();  // 对应 Python list(by_id.values())
    for (auto it = by_id.begin(); it != by_id.end(); ++it) features.push_back(it.value());
    existing["features"] = features;
    return entry;
}

}  // namespace pt

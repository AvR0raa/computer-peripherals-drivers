// migration.cpp — 配置迁移实现（plan.md §6，M-C6）。
#include "migration.hpp"

#include <algorithm>
#include <cmath>

namespace pt::migration {

DeviceIntent intent_from_read(const DpiRead& dpi, std::optional<int> poll_rate) {
    DeviceIntent intent;
    if (dpi.x && *dpi.x > 0) {
        intent.dpi = *dpi.x;
    }
    if (poll_rate && *poll_rate > 0) {
        intent.poll_rate = *poll_rate;
    }
    return intent;
}

DeviceIntent intent_from_profile(const DeviceProfile& profile) {
    // 参数名优先级：value/dpi/x/hz/rate 这类"值字段"，避免误取 mode/mask 等小标志
    static const char* kValueNames[] = {"value", "dpi", "x", "hz", "rate"};
    auto pick = [](const std::map<std::string, int>& params) -> std::optional<int> {
        for (const char* name : kValueNames) {
            const auto it = params.find(name);
            if (it != params.end() && it->second > 0) {
                return it->second;
            }
        }
        std::optional<int> fallback;
        for (const auto& [name, value] : params) {
            (void)name;
            if (value > 0 && (!fallback || value > *fallback)) {
                fallback = value;    // 无值字段名时取最大正数（mode/mask 类小值自然落选）
            }
        }
        return fallback;
    };

    DeviceIntent intent;
    for (const auto& feature : profile.features) {
        const std::string& id = feature.feature_id;
        if (!intent.dpi && id.find("dpi") != std::string::npos) {
            intent.dpi = pick(feature.params);
        }
        if (!intent.poll_rate &&
            (id.find("rate") != std::string::npos || id.find("poll") != std::string::npos)) {
            const auto v = pick(feature.params);
            if (v && *v >= 100) {
                intent.poll_rate = v;
            }
        }
    }
    return intent;
}

int nearest(const std::vector<int>& supported, int target) {
    if (supported.empty()) {
        return target;
    }
    int best = supported[0];
    long best_dist = std::labs(static_cast<long>(best) - target);
    for (int v : supported) {
        const long dist = std::labs(static_cast<long>(v) - target);
        if (dist < best_dist || (dist == best_dist && v < best)) {
            best = v;
            best_dist = dist;
        }
    }
    return best;
}

MigrationPlan plan_migration(const DeviceIntent& intent, const MouseCapabilities& caps) {
    MigrationPlan plan;
    if (intent.dpi) {
        if (!caps.dpi_support || caps.dpi_list.empty()) {
            plan.skipped.push_back("dpi: 新设备不支持 DPI 调节或未提供档位表");
        } else {
            const int to = nearest(caps.dpi_list, *intent.dpi);
            plan.writes.push_back({"dpi", *intent.dpi, to});
        }
    }
    if (intent.poll_rate) {
        if (!caps.poll_support || caps.poll_rates.empty()) {
            plan.skipped.push_back("poll_rate: 新设备不支持报告率调节");
        } else {
            const int to = nearest(caps.poll_rates, *intent.poll_rate);
            plan.writes.push_back({"poll_rate", *intent.poll_rate, to});
        }
    }
    return plan;
}

std::vector<ExecutedWrite> execute_migration(MouseApi& api, const MigrationPlan& plan) {
    std::vector<ExecutedWrite> results;
    for (const auto& write : plan.writes) {
        if (write.setting == "dpi") {
            const DpiOutcome out = api.set_dpi(write.to);
            std::string msg;
            for (const auto& line : out.log) {
                if (!msg.empty()) {
                    msg += " | ";
                }
                msg += line;
            }
            results.push_back({"dpi", out.ok, msg});
        } else if (write.setting == "poll_rate") {
            const auto [ok, msg] = api.set_poll_rate(write.to);
            results.push_back({"poll_rate", ok, msg});
        }
    }
    return results;
}

}  // namespace pt::migration

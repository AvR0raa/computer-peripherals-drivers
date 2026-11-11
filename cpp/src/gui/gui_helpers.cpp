// gui_helpers.cpp — 展示辅助实现。
#include "gui_helpers.hpp"

#include <algorithm>

#include "core/hid_api.hpp"

namespace pt::gui {

QString status_text(DeviceStatus s) {
    switch (s) {
        case DeviceStatus::Known: return "OK 已知(可直通)";
        case DeviceStatus::Sdk: return "SDK 有官方SDK";
        default: return "??? 未知(可学习)";
    }
}

QString status_icon(DeviceStatus s) {
    switch (s) {
        case DeviceStatus::Known: return "✅";
        case DeviceStatus::Sdk: return "🔌";
        default: return "❓";
    }
}

QColor status_color(DeviceStatus s) {
    switch (s) {
        case DeviceStatus::Known: return QColor("#2ecc71");
        case DeviceStatus::Sdk: return QColor("#3498db");
        default: return QColor("#95a5a6");
    }
}

QString type_icon_name(const QString& type) {
    if (type == "鼠标") return "🖱️";
    if (type == "键盘") return "⌨️";
    if (type == "耳机" || type == "音频设备") return "🎧";
    if (type == "接收器") return "📡";
    return "🔌";
}

QString type_icon(const QString& type) { return type_icon_name(type); }

std::pair<QString, QString> device_type_info(const QString& key,
                                      const std::vector<DeviceInfo>& ifaces,
                                      const Json& entry) {
    if (entry.is_object()) {
        const QString t = qstr(entry.value("type", std::string()));
        if (t == "鼠标" || t == "键盘" || t == "耳机" || t == "接收器" || t == "音频设备"
            || t == "外设")
            return {type_icon(t), t};
    }
    std::set<unsigned> usages;
    for (const auto& d : ifaces) usages.insert(d.usage);
    if (usages.count(0x0002)) return {"🖱️", "鼠标"};
    if (usages.count(0x0006)) return {"⌨️", "键盘"};
    if (usages.count(0x000C) || usages.count(0x000B)) return {"🎧", "音频设备"};
    return {"🔌", "外设"};
}

QString connection_text(const QString& key, const std::vector<DeviceInfo>& ifaces) {
    (void)ifaces;
    // 已知接收器 PID（罗技 Unifying/Lightspeed、海盗船）
    static const std::set<QString> RECEIVER_PIDS = {"C54D", "C232", "1B65"};
    const QString vidpid = key.left(4);
    if (vidpid == "046D" && RECEIVER_PIDS.count(key.mid(5))) return "无线接收器";
    if (vidpid == "046D") return "有线";
    return "";
}

Bytes read_descriptor(const std::string& path) {
    try {
        auto dev = hid::open_path(path);
        return hid::get_report_descriptor(dev.get());
    } catch (...) {
        return {};
    }
}

}  // namespace pt::gui

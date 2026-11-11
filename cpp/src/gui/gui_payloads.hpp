// gui_payloads.hpp — AdvancedWindow 多个实现文件共享的 Worker 载荷类型。
#pragma once
#include <QString>

#include <memory>
#include <optional>
#include <vector>

#include "core/dpi.hpp"

// DPI 面板读取载荷（ok=false 时 err 给出人话原因）
struct DpiReadPayloadShared {
    pt::DpiRead dpi;
    std::vector<int> lst;
    bool ok = false;
    QString err;
};

struct TextPayload {
    QString text;
};

inline std::shared_ptr<void> make_text_payload(const QString& s) {
    return std::make_shared<TextPayload>(TextPayload{s});
}

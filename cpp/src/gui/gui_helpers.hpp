// gui_helpers.hpp — 状态着色/类型图标/样式表等展示辅助（gui.py 顶层常量与函数平移）。
#pragma once
#include <QColor>
#include <QString>

#include <set>
#include <utility>
#include <vector>

#include "core/device_manager.hpp"
#include "core/fingerprint.hpp"
#include "core/json_util.hpp"

namespace pt::gui {

inline const QString APP_VERSION = "v0.9.0-cpp";

QString status_text(DeviceStatus s);   // "OK 已知(可直通)" / ...
QString status_icon(DeviceStatus s);   // "✅" / "🔌" / "❓"
QColor status_color(DeviceStatus s);

inline const QString STYLE_SHEET = R"(
QMainWindow, QDialog { background: #0D1117; }
#appTitle { font-size: 22px; font-weight: 800; color: #F0F4FA; }
#appSubtitle { font-size: 12px; color: #8A94A7; }
#card { background: #161B27; border: 1px solid #232B3B; border-radius: 16px; }
#card:hover { border: 1px solid #4F8CFF; }
#cardTitle { font-size: 16px; font-weight: 700; color: #F0F4FA; }
#cardSub { font-size: 12px; color: #8A94A7; }
#batteryPct { font-size: 24px; font-weight: 800; }
QProgressBar { background: #232B3B; border: none; border-radius: 4px; }
QPushButton { background: qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #4F8CFF, stop:1 #6C5CE7);
              color: white; border: none; border-radius: 9px; padding: 8px 16px;
              font-size: 13px; font-weight: 600; }
QPushButton:hover { background: #5B97FF; }
QPushButton:disabled { background: #2A3547; color: #5A6579; }
QPushButton#ghostBtn { background: #1C2333; color: #C9D2E3; border: 1px solid #2A3547; }
QPushButton#ghostBtn:hover { background: #232B3B; }
QSlider::groove:horizontal { height: 6px; background: #232B3B; border-radius: 3px; }
QSlider::handle:horizontal { width: 18px; margin: -7px 0; border-radius: 9px; background: #4F8CFF; }
QSlider::sub-page:horizontal { background: #4F8CFF66; border-radius: 3px; }
QStatusBar { background: transparent; color: #8A94A7; }
QScrollArea { background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }
QToolTip { background: #1C2333; color: #F0F4FA; border: 1px solid #2A3547; }
QMenu { background: #1C2333; color: #F0F4FA; border: 1px solid #2A3547; }
)";

// 类型 emoji 图标（协议库 type 字段 → 图标）
QString type_icon_name(const QString& type);   // 图标名，未知返回"外设"图标
QString type_icon(const QString& type);

// (emoji, 类型名)：协议库 type 字段优先，否则按 usage 启发式
std::pair<QString, QString> device_type_info(const QString& key,
                                      const std::vector<DeviceInfo>& ifaces,
                                      const Json& entry);

// 有线/无线标识：接收器=无线，其余罗技设备=有线直连
QString connection_text(const QString& key, const std::vector<DeviceInfo>& ifaces);

// 打开设备只读描述符；失败返回空
Bytes read_descriptor(const std::string& path);

// QString 版 hex（Qt 代码里直接进 QString::arg）
inline QString qhex(std::uint64_t v, int w) { return QString::fromStdString(hex(v, w)); }

// std::string ↔ QString（UTF-8）
inline QString qstr(const std::string& s) { return QString::fromStdString(s); }
inline std::string sstr(const QString& s) { return s.toStdString(); }

}  // namespace pt::gui

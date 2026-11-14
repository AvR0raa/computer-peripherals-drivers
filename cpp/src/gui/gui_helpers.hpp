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

inline const QString APP_VERSION = "v0.10.0";

QString status_text(DeviceStatus s);   // "OK 已知(可直通)" / ...
QString status_icon(DeviceStatus s);   // "✅" / "🔌" / "❓"
QColor status_color(DeviceStatus s);

inline const QString STYLE_SHEET = R"(
* { font-family: "Segoe UI Variable", "Segoe UI", "Microsoft YaHei UI"; }
QMainWindow, QDialog { background: #0B0E14; }
#central { background: #0B0E14; }
#appTitle { font-size: 24px; font-weight: 800; color: #F5F8FF; letter-spacing: 0.5px; }
#appSubtitle { font-size: 12px; color: #7C869C; }
#card { background: #12161F; border: 1px solid #1D2534; border-radius: 18px; }
#card:hover { border: 1px solid #4F8CFF; }
#cardTitle { font-size: 16px; font-weight: 700; color: #F5F8FF; }
#cardSub { font-size: 12px; color: #7C869C; }
#batteryPct { font-size: 26px; font-weight: 800; }
QProgressBar { background: #1D2534; border: none; border-radius: 5px; }
QProgressBar::chunk { border-radius: 5px; }
QPushButton { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                stop:0 #4F8CFF, stop:1 #7C5CFF);
              color: white; border: none; border-radius: 10px; padding: 8px 18px;
              font-size: 13px; font-weight: 600; }
QPushButton:hover { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                stop:0 #6B9DFF, stop:1 #8E76FF); }
QPushButton:pressed { background: #3E78D9; }
QPushButton:disabled { background: #1D2534; color: #4A5470; }
QPushButton#ghostBtn { background: #151B28; color: #B9C4DB;
                        border: 1px solid #2A3547; font-weight: 500; }
QPushButton#ghostBtn:hover { background: #1D2534; border-color: #3A4A66; }
QSlider::groove:horizontal { height: 6px; background: #1D2534; border-radius: 3px; }
QSlider::handle:horizontal { width: 18px; margin: -7px 0; border-radius: 9px;
                             background: #4F8CFF; }
QSlider::handle:horizontal:hover { background: #6B9DFF; }
QSlider::sub-page:horizontal { background: qlineargradient(x1:0,y1:0,x2:1,y2:0,
                               stop:0 #4F8CFF, stop:1 #7C5CFF); border-radius: 3px; }
QStatusBar { background: transparent; color: #67718A; font-size: 12px; }
QScrollArea { background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }
QToolTip { background: #151B28; color: #F0F4FA; border: 1px solid #2A3547;
           border-radius: 6px; padding: 6px; }
QMenu { background: #151B28; color: #F0F4FA; border: 1px solid #2A3547;
        border-radius: 8px; padding: 6px; }
QMenu::item { padding: 6px 24px; border-radius: 6px; }
QMenu::item:selected { background: #4F8CFF33; }
QMenu::separator { height: 1px; background: #2A3547; margin: 4px 8px; }
QLineEdit { background: #12161F; color: #F0F4FA; border: 1px solid #2A3547;
            border-radius: 8px; padding: 7px 10px; }
QLineEdit:focus { border-color: #4F8CFF; }
QComboBox { background: #12161F; color: #F0F4FA; border: 1px solid #2A3547;
            border-radius: 8px; padding: 6px 10px; }
QComboBox:hover { border-color: #3A4A66; }
QComboBox QAbstractItemView { background: #151B28; color: #F0F4FA;
                              selection-background-color: #4F8CFF55;
                              border: 1px solid #2A3547; }
QSpinBox { background: #12161F; color: #F0F4FA; border: 1px solid #2A3547;
           border-radius: 8px; padding: 5px 8px; }
QTableWidget { background: #12161F; color: #F0F4FA; border: 1px solid #2A3547;
               border-radius: 10px; gridline-color: #1D2534; }
QTableWidget::item:selected { background: #4F8CFF55; }
QHeaderView::section { background: #151B28; color: #8A94A7;
                       border: none; padding: 6px; }
QPlainTextEdit { background: #12161F; color: #C9D2E3; border: 1px solid #2A3547;
                 border-radius: 10px; }
QListWidget { background: #12161F; color: #F0F4FA; border: 1px solid #2A3547;
              border-radius: 10px; padding: 4px; }
QListWidget::item { border-radius: 8px; padding: 6px; margin: 2px; }
QListWidget::item:hover { background: #1D2534; }
QListWidget::item:selected { background: #4F8CFF44; color: #F0F4FA; }
QCheckBox { color: #C9D2E3; }
QCheckBox::indicator { width: 16px; height: 16px; border-radius: 4px;
                       border: 1px solid #2A3547; background: #12161F; }
QCheckBox::indicator:checked { background: #4F8CFF; border-color: #4F8CFF; }
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

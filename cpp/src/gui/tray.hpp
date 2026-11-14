// tray.hpp — 系统托盘常驻（M-C4 收尾）：托盘图标（电量环）+ 右键菜单 + 定时电量扫描。
// plan v4 原初形态：低占用托盘工具；关窗最小化到托盘，退出走托盘菜单。
#pragma once
#include <QObject>
#include <QSystemTrayIcon>

#include <memory>

class MainWindow;
class QMenu;
class QTimer;

namespace pt::gui {

class TrayController : public QObject {
    Q_OBJECT
public:
    explicit TrayController(MainWindow* window, QObject* parent = nullptr);
    ~TrayController() override;

private:
    void rebuild_menu();
    void update_icon();
    void scan_once();

public:
    // 低電量氣泡（同一設備只在跨過閾值時提醒一次）
    void show_low_battery(const QString& name, int percent);
    void show_running_hint();       // 首次最小化到托盘的提示

    MainWindow* window_;
    QSystemTrayIcon tray_;
    std::unique_ptr<QMenu> menu_;
    QTimer* scan_timer_ = nullptr;      // 周期触发窗口的电量扫描
    QTimer* ui_timer_ = nullptr;        // 周期刷新托盘图标/提示（读缓存，零设备 I/O）
    QStringList low_alerted_;           // 已提醒过低电量的设备
};

// 电量环图标（0-100）；无读数给灰色问号环。
QIcon battery_ring_icon(const std::optional<int>& percent);

// 托盘提示文本：全部设备名 + 电量摘要（读窗口缓存，纯文本拼装）。
QString tray_tooltip_for(class MainWindow* window);

}  // namespace pt::gui

// main_window.hpp — 用户客户端主窗口：单页设备卡片墙（电量 + DPI）。（gui.py MainWindow 平移）
#pragma once
#include <QLabel>
#include <QMainWindow>
#include <QSettings>

#include <map>
#include <memory>

#include "core/battery.hpp"
#include "core/device_manager.hpp"
#include "core/fingerprint.hpp"
#include "dpi_dialog.hpp"
#include "workers.hpp"

class QGridLayout;
class QProgressBar;
class QScrollArea;

class AdvancedWindow;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QStringList hidden_list() const;
    void hide_device(const QString& key);
    void build_ui();
    QFrame* build_card(const QString& key);
    void refresh_devices();
    void rebuild_cards();
    void show_card_menu(const QString& key, QFrame* card);
    void open_dpi(const QString& key);
    void open_advanced();
    void on_battery_all();
    void start_watcher();
    void on_hotplug(const pt::DeviceEvent& event);

    pt::Json db_;
    std::map<QString, std::vector<pt::DeviceInfo>> devices_;
    std::map<QString, pt::BatteryResult> battery_cache_;
    pt::gui::HotplugWatcher* watcher_ = nullptr;
    AdvancedWindow* advanced_ = nullptr;
    QSettings settings_;
    QGridLayout* grid_ = nullptr;
};


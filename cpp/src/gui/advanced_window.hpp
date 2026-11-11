// advanced_window.hpp — 高级模式窗口（协议开发）：总览 / 设备 / 学习模式 三标签页。
// （gui.py AdvancedWindow 平移；动态控件/档案/批量探测/学习工作流全部保留）
#pragma once
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>

#include <map>
#include <memory>
#include <optional>

#include "core/battery.hpp"
#include "core/diffing.hpp"
#include "core/fingerprint.hpp"
#include "core/recorder.hpp"
#include "workers.hpp"

class QComboBox;
class QGridLayout;
class QPushButton;
class QVBoxLayout;

class AdvancedWindow : public QMainWindow {
    Q_OBJECT
public:
    AdvancedWindow();

protected:
    void closeEvent(QCloseEvent* event) override;

signals:
    void logLine(const QString& line);   // hex 日志追加（工作线程 → GUI 线程）

private:
    struct ProtoControl {
        pt::Json feature;
        std::map<QString, QSpinBox*> spins;
    };

    // ---- UI 构建 ----
    void build_ui();
    QWidget* build_dashboard_tab();
    QWidget* build_learn_tab();

    // ---- 设备列表 / 热插拔 ----
    void refresh_devices();
    QString current_key() const;
    void start_watcher();
    void on_hotplug(const pt::DeviceEvent& event);

    // ---- 详情页 ----
    void on_select();
    void clear_protocol_panel();
    void build_protocol_panel(const QString& key, const std::vector<pt::DeviceInfo>& ifaces,
                              const pt::FingerprintResult& result);
    void build_dpi_panel(const QString& key, const std::vector<pt::DeviceInfo>& ifaces);

    // ---- 总览仪表板（任务 3.6）----
    void refresh_dashboard();
    void goto_device(const QString& key);
    void on_battery_all();

    // ---- 学习模式页（M4.4）----
    void refresh_learn_targets();
    void learn_tick();
    void on_learn_start();
    void on_learn_mark();
    void on_learn_stop();
    void on_learn_commit();

    // ---- 动作 ----
    void on_read_descriptor();
    void on_vendor_probe();
    void on_batch_probe();
    void on_run_feature(const QString& key, const pt::Json& feature,
                        const std::map<QString, QSpinBox*>& spins);

    // ---- 配置档案（任务 3.5）----
    void reload_profile_combo(const QString& key);
    void on_profile_save();
    void on_profile_load();
    void on_profile_apply();

    // ---- 工作线程：spawn_worker 统一自清理（finished→deleteLater，无 parent） ----
    void spawn_worker(pt::gui::Worker* worker);
    void append_log(const QString& line);

    pt::Json db_;
    std::map<QString, std::vector<pt::DeviceInfo>> devices_;
    pt::gui::HotplugWatcher* watcher_ = nullptr;
    std::vector<ProtoControl> proto_controls_;
    QSettings settings_;
    std::map<QString, pt::BatteryResult> battery_cache_;

    // 设备页
    QWidget* devices_tab_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QListWidget* list_ = nullptr;
    QLabel* count_label_ = nullptr;
    QLabel* detail_title_ = nullptr;
    QTableWidget* iface_table_ = nullptr;
    QPushButton* desc_btn_ = nullptr;
    QPushButton* vendor_probe_btn_ = nullptr;
    QLabel* desc_status_ = nullptr;
    QPlainTextEdit* desc_output_ = nullptr;
    QVBoxLayout* protocol_box_ = nullptr;
    QLineEdit* profile_name_ = nullptr;
    QComboBox* profile_combo_ = nullptr;

    // 总览页
    QGridLayout* dash_grid_ = nullptr;

    // 学习模式页
    QListWidget* learn_targets_ = nullptr;
    QPushButton* learn_start_btn_ = nullptr;
    QPushButton* learn_mark_btn_ = nullptr;
    QPushButton* learn_stop_btn_ = nullptr;
    QPushButton* learn_commit_btn_ = nullptr;
    QLineEdit* learn_note_ = nullptr;
    QLabel* learn_status_ = nullptr;
    QPlainTextEdit* learn_output_ = nullptr;
    std::unique_ptr<pt::Recorder> learn_recorder_;
    std::optional<double> learn_mark_ts_;
    std::vector<pt::GroupDiff> learn_diffs_;
    std::map<QString, pt::Json> learn_label_meta_;
    QTimer* learn_timer_ = nullptr;

    // 日志
    QPlainTextEdit* log_ = nullptr;
};

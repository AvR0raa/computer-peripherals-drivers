// dpi_dialog.hpp — 简洁 DPI 调节弹窗：读取 → 滑条 → 应用（安全写）。（gui.py DpiDialog 平移）
#pragma once
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QSlider>
#include <QSpinBox>

#include <vector>

#include "core/device_manager.hpp"
#include "core/hidpp.hpp"
#include "workers.hpp"

class DpiDialog : public QDialog {
    Q_OBJECT
public:
    DpiDialog(QWidget* parent, const QString& key, std::vector<pt::DeviceInfo> ifaces,
              const QString& product);

private:
    std::unique_ptr<pt::HidppLink> build_link();  // 任务线程内创建/关闭（与 Python 一致）
    int slot() const;                             // 接收器配对走槽位 1；无长通道直连 0xFF
    void track(pt::gui::Worker* worker);
    void read_current();
    void apply_rate();
    void apply();

    std::vector<pt::DeviceInfo> ifaces_;
    QLabel* status_ = nullptr;
    QSpinBox* spin_ = nullptr;
    QSlider* slider_ = nullptr;
    QLabel* info_ = nullptr;
    QComboBox* rate_combo_ = nullptr;
};

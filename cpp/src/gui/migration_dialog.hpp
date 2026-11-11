// migration_dialog.hpp — 配置迁移预览面板（M-C6 GUI，plan.md §6）。
// 流程：选来源/目标设备 → 生成预览（意图∩能力，就近取档）→ 确认 → 执行（安全写）。
#pragma once
#include <QDialog>

#include <memory>

#include "core/migration.hpp"

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

class MainWindow;

namespace pt::gui {

class MigrationDialog : public QDialog {
    Q_OBJECT
public:
    explicit MigrationDialog(MainWindow* window);

private:
    std::vector<DeviceInfo> ifaces_for(const QString& key) const;
    void on_preview();
    void on_execute();

    MainWindow* window_;
    QComboBox* src_ = nullptr;
    QComboBox* dst_ = nullptr;
    QTableWidget* preview_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* skipped_ = nullptr;
    QPushButton* exec_btn_ = nullptr;
    std::optional<pt::migration::MigrationPlan> plan_;
    QString intent_text_;
};

}  // namespace pt::gui

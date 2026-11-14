// learning_wizard.hpp — 主页学习向导（面向普通用户）。
// 与高级窗口的学习模式同一套引擎（core recorder/diffing/wizard），
// 但流程简化为 5 步指引，接口自动勾选推荐项。
#pragma once
#include <QDialog>

#include <memory>
#include <optional>

#include "core/diffing.hpp"
#include "core/recorder.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTimer;

class MainWindow;

namespace pt::gui {

class LearningWizard : public QDialog {
    Q_OBJECT
public:
    explicit LearningWizard(MainWindow* window);

private:
    void rebuild_targets(const QString& selected_key = {});
    void on_start();
    void on_mark();
    void on_stop();
    void on_commit();
    void tick();

    MainWindow* window_;
    QComboBox* device_ = nullptr;
    QListWidget* targets_ = nullptr;
    QPushButton* start_btn_ = nullptr;
    QPushButton* mark_btn_ = nullptr;
    QPushButton* stop_btn_ = nullptr;
    QPushButton* commit_btn_ = nullptr;
    QLineEdit* note_ = nullptr;
    QLabel* status_ = nullptr;
    QPlainTextEdit* output_ = nullptr;
    QTimer* tick_timer_ = nullptr;

    std::unique_ptr<Recorder> recorder_;
    std::optional<double> mark_ts_;
    std::vector<pt::GroupDiff> diffs_;
};

}  // namespace pt::gui

// migration_dialog.cpp — 配置迁移预览面板实现。
#include "migration_dialog.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QVBoxLayout>

#include "core/mouse_api.hpp"
#include "main_window.hpp"
#include "workers.hpp"

namespace pt::gui {

namespace {

QString device_label(const QString& key, const std::vector<DeviceInfo>& ifaces) {
    if (ifaces.empty()) return key;
    const QString product = QString::fromStdString(ifaces[0].product);
    return product.isEmpty() ? key : product;
}

// 把 DpiRead/报告率采集成人读意图描述
QString intent_text(const pt::migration::DeviceIntent& intent) {
    QStringList parts;
    if (intent.dpi) parts << QString("DPI %1").arg(*intent.dpi);
    if (intent.poll_rate) parts << QString("报告率 %1Hz").arg(*intent.poll_rate);
    return parts.isEmpty() ? "（无可迁移项）" : parts.join(" · ");
}

}  // namespace

MigrationDialog::MigrationDialog(MainWindow* window)
    : QDialog(window), window_(window) {
    setWindowTitle("配置迁移 — 换设备自动套用");
    setMinimumWidth(560);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    auto* tip = new QLabel(
        "把「来源设备」上调好的设置迁移到「目标设备」：\n"
        "意图值不在新设备档位表中时自动就近取档；执行走安全写（读回验证+失败回滚）。");
    tip->setWordWrap(true);
    layout->addWidget(tip);

    auto* pick_row = new QHBoxLayout();
    pick_row->addWidget(new QLabel("来源设备（已调好的）："));
    src_ = new QComboBox();
    pick_row->addWidget(src_, 1);
    pick_row->addWidget(new QLabel(" → "));
    pick_row->addWidget(new QLabel("目标设备："));
    dst_ = new QComboBox();
    pick_row->addWidget(dst_, 1);
    layout->addLayout(pick_row);

    preview_ = new QTableWidget(0, 3);
    preview_->setHorizontalHeaderLabels({"设置项", "意图值", "将写入（就近取档）"});
    preview_->horizontalHeader()->setStretchLastSection(true);
    preview_->verticalHeader()->setVisible(false);
    preview_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    preview_->setMinimumHeight(140);
    layout->addWidget(preview_);

    skipped_ = new QLabel("");
    skipped_->setStyleSheet("color:#FFB020; font-size:12px;");
    skipped_->setWordWrap(true);
    layout->addWidget(skipped_);

    status_ = new QLabel("两台设备都需在线——睡眠中的罗技设备会被 HID++ 流量自动唤醒");
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* btns = new QHBoxLayout();
    auto* preview_btn = new QPushButton("生成预览");
    connect(preview_btn, &QPushButton::clicked, this, [this] { on_preview(); });
    exec_btn_ = new QPushButton("执行迁移");
    exec_btn_->setEnabled(false);
    connect(exec_btn_, &QPushButton::clicked, this, [this] { on_execute(); });
    auto* close_btn = new QPushButton("关闭");
    close_btn->setObjectName("ghostBtn");
    connect(close_btn, &QPushButton::clicked, this, &QDialog::reject);
    btns->addWidget(preview_btn);
    btns->addWidget(exec_btn_);
    btns->addStretch(1);
    btns->addWidget(close_btn);
    layout->addLayout(btns);

    // 下拉填充：有 MouseApi 后端的设备
    for (const auto& [key, ifaces] : window_->devices()) {
        if (!pt::detect(ifaces)) continue;
        const QString label = QString("%1  (%2)").arg(device_label(key, ifaces), key);
        src_->addItem(label, key);
        dst_->addItem(label, key);
    }
    if (src_->count() >= 2) {
        dst_->setCurrentIndex(1);        // 默认错开来源/目标
    }
    if (src_->count() < 2) {
        status_->setText("需要至少两台可控制的鼠标类设备才能迁移");
    }
}

std::vector<DeviceInfo> MigrationDialog::ifaces_for(const QString& key) const {
    const auto it = window_->devices().find(key);
    return it != window_->devices().end() ? it->second : std::vector<DeviceInfo>{};
}

void MigrationDialog::on_preview() {
    const QString src_key = src_->currentData().toString();
    const QString dst_key = dst_->currentData().toString();
    if (src_key.isEmpty() || dst_key.isEmpty() || src_key == dst_key) {
        status_->setText("请选择两台不同的设备");
        return;
    }
    exec_btn_->setEnabled(false);
    preview_->setRowCount(0);
    skipped_->setText("");
    status_->setText("读取中…（两台设备都会被尝试唤醒）");

    const auto src_ifaces = ifaces_for(src_key);
    const auto dst_ifaces = ifaces_for(dst_key);

    auto* worker = new Worker(
        [src_ifaces, dst_ifaces]() -> std::shared_ptr<void> {
            auto src_api = pt::detect(src_ifaces);
            auto dst_api = pt::detect(dst_ifaces);
            if (!src_api || !dst_api) {
                throw std::runtime_error("有设备不支持鼠标设置 API");
            }
            const auto caps = src_api->capabilities();
            if (!caps.dpi_support && !caps.poll_support) {
                throw std::runtime_error(
                    "来源设备无响应或不支持读取（notes: " +
                    (caps.notes.empty() ? std::string("无") : caps.notes.front()) + "）");
            }
            const DpiRead dpi = src_api->get_dpi();
            const auto poll = src_api->get_poll_rate();
            const auto intent = pt::migration::intent_from_read(dpi, poll);
            const auto dst_caps = dst_api->capabilities();
            auto* payload = new std::pair<pt::migration::DeviceIntent, pt::migration::MigrationPlan>(
                intent, pt::migration::plan_migration(intent, dst_caps));
            return std::shared_ptr<void>(payload);
        });

    connect(worker, &Worker::done, this, [this](const JobResult& result) {
        if (!result.ok) {
            status_->setText("读取失败: " + result.error);
            return;
        }
        const auto& value = *static_cast<std::pair<pt::migration::DeviceIntent, pt::migration::MigrationPlan>*>(
            result.data.get());
        intent_text_ = intent_text(value.first);
        plan_ = value.second;

        preview_->setRowCount(0);
        for (const auto& w : plan_->writes) {
            const int row = preview_->rowCount();
            preview_->insertRow(row);
            preview_->setItem(row, 0, new QTableWidgetItem(QString::fromStdString(w.setting)));
            preview_->setItem(row, 1, new QTableWidgetItem(QString::number(w.from)));
            preview_->setItem(row, 2, new QTableWidgetItem(QString::number(w.to)));
        }
        QStringList skipped;
        for (const auto& s : plan_->skipped) skipped << QString::fromStdString(s);
        skipped_->setText(skipped.join("；"));
        status_->setText("预览已生成 — 意图: " + intent_text_ +
                         "。确认无误后点「执行迁移」。");
        exec_btn_->setEnabled(!plan_->writes.empty());
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MigrationDialog::on_execute() {
    if (!plan_ || plan_->writes.empty()) return;
    const QString dst_key = dst_->currentData().toString();
    const auto dst_ifaces = ifaces_for(dst_key);
    exec_btn_->setEnabled(false);
    status_->setText("执行迁移中…（逐项安全写）");

    const pt::migration::MigrationPlan plan = *plan_;
    auto* worker = new Worker(
        [dst_ifaces, plan]() -> std::shared_ptr<void> {
            auto api = pt::detect(dst_ifaces);
            if (!api) throw std::runtime_error("目标设备不支持鼠标设置 API");
            const auto results = pt::migration::execute_migration(*api, plan);
            return std::shared_ptr<void>(
                new std::vector<pt::migration::ExecutedWrite>(results));
        });

    connect(worker, &Worker::done, this, [this](const JobResult& result) {
        if (!result.ok) {
            status_->setText("执行出错: " + result.error);
            return;
        }
        const auto& results = *static_cast<std::vector<pt::migration::ExecutedWrite>*>(
            result.data.get());
        QString text;
        int ok_count = 0;
        for (const auto& r : results) {
            text += QString("%1: %2  %3\n")
                        .arg(QString::fromStdString(r.setting),
                             r.ok ? QString("✅") : QString("❌"),
                             QString::fromStdString(r.msg));
            if (r.ok) ++ok_count;
        }
        status_->setText(QString("迁移完成 — %1/%2 项成功\n%3")
                             .arg(ok_count)
                             .arg(static_cast<int>(results.size()))
                             .arg(text));
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

}  // namespace pt::gui

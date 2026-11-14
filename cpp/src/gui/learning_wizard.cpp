// learning_wizard.cpp — 主页学习向导实现（指引完整 + 简单，5 步）。
#include "learning_wizard.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

#include "core/descriptor.hpp"
#include "core/probe.hpp"
#include "core/diffing.hpp"
#include "core/fingerprint.hpp"
#include "core/wizard.hpp"
#include "gui_helpers.hpp"
#include "main_window.hpp"
#include "workers.hpp"

namespace pt::gui {

namespace {

Json item_info(const QListWidgetItem* item) {
    return Json::parse(item->data(Qt::UserRole).toByteArray().toStdString());
}

}  // namespace

LearningWizard::LearningWizard(MainWindow* window)
    : QDialog(window), window_(window) {
    setWindowTitle("电量学习 — 让软件学会你的设备");
    setMinimumSize(640, 560);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(10);

    auto* guide = new QLabel(
        "<b>五步教会软件读你设备的电量/设置（约 3 分钟）：</b><br>"
        "① 选设备（下面已自动选好）　② 点「开始录制」<br>"
        "③ 点「标记起点」　④ <b>打开该设备的官方驱动软件，改一次设置</b>"
        "（比如切换灯效 / 调一次电量显示）<br>"
        "⑤ 回这里点「停止并分析」→ 填一句说明 →「保存到设备库」<br>"
        "<small>全程只读录制，不会写坏设备。学到的协议立即生效。</small>");
    guide->setWordWrap(true);
    layout->addWidget(guide);

    auto* dev_row = new QHBoxLayout();
    dev_row->addWidget(new QLabel("① 设备："));
    device_ = new QComboBox();
    dev_row->addWidget(device_, 1);
    layout->addLayout(dev_row);

    targets_ = new QListWidget();
    targets_->setToolTip("推荐已自动勾选（厂商自定义 / rawhid 接口）");
    targets_->setMinimumHeight(110);
    layout->addWidget(targets_, 1);

    auto* btn_row = new QHBoxLayout();
    start_btn_ = new QPushButton("② 开始录制");
    mark_btn_ = new QPushButton("③ 标记起点");
    mark_btn_->setEnabled(false);
    stop_btn_ = new QPushButton("⑤ 停止并分析");
    stop_btn_->setEnabled(false);
    btn_row->addWidget(start_btn_);
    btn_row->addWidget(mark_btn_);
    btn_row->addWidget(stop_btn_);
    btn_row->addStretch(1);
    layout->addLayout(btn_row);

    output_ = new QPlainTextEdit();
    output_->setReadOnly(true);
    output_->setFont(QFont("Consolas", 9));
    output_->setPlaceholderText("差分分析结果将显示在这里…");
    output_->setMaximumHeight(140);
    layout->addWidget(output_);

    auto* note_row = new QHBoxLayout();
    note_row->addWidget(new QLabel("④ 你改了什么："));
    note_ = new QLineEdit();
    note_->setPlaceholderText("例：切换了一次灯效 / 修改电量显示");
    note_row->addWidget(note_, 1);
    commit_btn_ = new QPushButton("保存到设备库");
    commit_btn_->setEnabled(false);
    note_row->addWidget(commit_btn_);
    layout->addLayout(note_row);

    status_ = new QLabel("就绪 — 选好设备后点「开始录制」");
    status_->setWordWrap(true);
    layout->addWidget(status_);

    // 设备下拉
    for (const auto& [key, ifaces] : window_->devices()) {
        QString name = ifaces.empty() ? key
                                      : (ifaces[0].product.empty()
                                             ? key : QString::fromStdString(ifaces[0].product));
        device_->addItem(QString("%1  (%2)").arg(name, key), key);
    }
    connect(device_, &QComboBox::currentIndexChanged, this,
            [this](int) { rebuild_targets(); });

    connect(start_btn_, &QPushButton::clicked, this, [this] { on_start(); });
    connect(mark_btn_, &QPushButton::clicked, this, [this] { on_mark(); });
    connect(stop_btn_, &QPushButton::clicked, this, [this] { on_stop(); });
    connect(commit_btn_, &QPushButton::clicked, this, [this] { on_commit(); });

    tick_timer_ = new QTimer(this);
    tick_timer_->setInterval(500);
    connect(tick_timer_, &QTimer::timeout, this, [this] { tick(); });

    rebuild_targets();
}

void LearningWizard::rebuild_targets(const QString& selected_key) {
    const QString key = selected_key.isEmpty() && device_->currentIndex() >= 0
                            ? device_->currentData().toString()
                            : selected_key;
    targets_->clear();
    if (key.isEmpty()) return;
    const auto it = window_->devices().find(key);
    if (it == window_->devices().end()) return;
    for (const auto& d : it->second) {
        const bool is_vendor = d.usage_page >= 0xFF00;
        const bool is_rawhid = d.usage_page == 0x0001 && d.usage == 0x0000;
        if (!is_vendor && !is_rawhid) continue;
        const QString label = QString("UP%1-U%2%3")
                                  .arg(QString::fromStdString(hex(d.usage_page, 4)),
                                       QString::fromStdString(hex(d.usage, 4)),
                                       is_vendor ? "　推荐" : "　rawhid");
        auto* item = new QListWidgetItem(label);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(is_vendor ? Qt::Checked : Qt::Checked);
        Json info = Json::object();
        info["label"] = QString("UP%1-U%2").arg(
                              QString::fromStdString(hex(d.usage_page, 4)),
                              QString::fromStdString(hex(d.usage, 4)))
                            .toStdString();
        info["vidpid"] = key.toStdString();
        info["path"] = d.path;
        info["usage_page"] = d.usage_page;
        info["usage"] = d.usage;
        item->setData(Qt::UserRole,
                      QVariant::fromValue(QByteArray::fromStdString(info.dump())));
        targets_->addItem(item);
    }
}

void LearningWizard::on_start() {
    std::vector<RecorderTarget> targets;
    for (int idx = 0; idx < targets_->count(); ++idx) {
        auto* item = targets_->item(idx);
        if (item->checkState() != Qt::Checked) continue;
        const Json info = Json::parse(
            item->data(Qt::UserRole).toByteArray().toStdString());
        std::vector<int> feature_ids;
        if (info["usage_page"].get<unsigned>() >= 0xFF00) {
            const Bytes raw = read_descriptor(info["path"].get<std::string>());
            if (!raw.empty())
                for (int i : feature_report_ids(parse_report_descriptor(raw)))
                    if (i) feature_ids.push_back(i);
        }
        targets.push_back(RecorderTarget{info["label"].get<std::string>(),
                                         info["path"].get<std::string>(),
                                         feature_ids});
    }
    if (targets.empty()) {
        status_->setText("没有勾选任何接口");
        return;
    }
    recorder_ = std::make_unique<Recorder>(targets);
    recorder_->start();
    mark_ts_.reset();
    diffs_.clear();
    output_->clear();
    start_btn_->setEnabled(false);
    mark_btn_->setEnabled(true);
    stop_btn_->setEnabled(true);
    commit_btn_->setEnabled(false);
    tick_timer_->start();
    status_->setText("录制中… 已自动勾选推荐接口。下一步：点「标记起点」");
}

void LearningWizard::on_mark() {
    if (!recorder_) return;
    mark_ts_ = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    status_->setText("起点已标记 —— 现在打开官方驱动软件改一次设置，改完回来点「停止并分析」");
}

void LearningWizard::on_stop() {
    if (!recorder_) return;
    tick_timer_->stop();
    recorder_->stop();
    const auto entries = recorder_->entries();
    recorder_.reset();
    std::vector<CaptureEntry> before, after;
    if (!mark_ts_) {
        after = entries;
        status_->setText("⚠ 未标记起点：全部按「新报文」处理");
    } else {
        for (const auto& e : entries) (e.ts < *mark_ts_ ? before : after).push_back(e);
        status_->setText(QString("分析完成（起点前 %1 条 / 后 %2 条）")
                             .arg(before.size())
                             .arg(after.size()));
    }
    diffs_ = diff_sessions(before, after);
    QStringList lines;
    for (const auto& d : diffs_)
        for (const auto& l : format_diff(d)) lines << qstr(l);
    if (lines.isEmpty()) lines << "没有检测到差异（确认录制期间在官方软件里改了设置？）";
    output_->setPlainText(lines.join("\n"));
    start_btn_->setEnabled(true);
    mark_btn_->setEnabled(false);
    stop_btn_->setEnabled(false);
    const bool has = std::any_of(diffs_.begin(), diffs_.end(),
                                 [](const GroupDiff& d) { return !d.is_volatile; });
    commit_btn_->setEnabled(has);
}

void LearningWizard::on_commit() {
    if (diffs_.empty()) return;
    const std::string note = note_->text().trimmed().toStdString();
    Json label_match = Json::object();
    for (int idx = 0; idx < targets_->count(); ++idx) {
        const Json info = Json::parse(
            targets_->item(idx)->data(Qt::UserRole).toByteArray().toStdString());
        label_match[info["label"].get<std::string>()] = Json{
            {"usage_page", hex(info["usage_page"].get<unsigned>(), 4)},
            {"usage", hex(info["usage"].get<unsigned>(), 4)}};
    }
    const Json drafts = build_drafts(diffs_, note, label_match);
    if (drafts.empty()) {
        status_->setText("没有可入库的草稿（只有噪声流？）");
        return;
    }
    try {
        Json db = load_db(default_db_path());
        std::map<std::string, std::vector<Json>> by_device;
        for (const auto& draft : drafts) {
            const std::string source = draft["learned"]["source"].get<std::string>();
            for (int idx = 0; idx < targets_->count(); ++idx) {
                const Json info = Json::parse(
                    targets_->item(idx)->data(Qt::UserRole).toByteArray().toStdString());
                if (info["label"].get<std::string>() == source) {
                    by_device[info["vidpid"].get<std::string>()].push_back(draft);
                    break;
                }
            }
        }
        for (const auto& [vidpid, device_drafts] : by_device) {
            const Json match = device_drafts.front().contains("match")
                                   ? device_drafts.front()["match"]
                                   : Json();
            merge_into_db(db, vidpid, build_protocol_block(match, device_drafts),
                          note);
        }
        save_db(db, default_db_path());
        window_->refresh_devices();
        output_->appendPlainText(
            QString("✅ 已入库 %1 条草稿（unverified）— 电量扫描将自动尝试新协议")
                .arg(static_cast<int>(drafts.size())));
        status_->setText("✅ 已保存到设备库！下次「读取全部电量」会自动用上新协议");
        commit_btn_->setEnabled(false);
    } catch (const std::exception& exc) {
        status_->setText(QString("入库失败: %1").arg(exc.what()));
    }
}

void LearningWizard::tick() {
    if (recorder_)
        status_->setText(QString("录制中… 已捕获 %1 条")
                             .arg(static_cast<int>(recorder_->entries().size())));
}

}  // namespace pt::gui

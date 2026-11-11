// advanced_window2.cpp — 高级窗口：总览仪表板 / 学习模式 / 描述符与探测动作 / 配置档案。
// （gui.py AdvancedWindow 后半部分平移）
#include "advanced_window.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

#include "core/descriptor.hpp"
#include "core/hidpp.hpp"
#include "core/profiles.hpp"
#include "core/probe.hpp"
#include "core/report_io.hpp"
#include "core/safe_write.hpp"
#include "core/wizard.hpp"
#include "gui_helpers.hpp"
#include "gui_payloads.hpp"

using namespace pt;
using namespace pt::gui;

namespace {

QString now_clock() {
    return QDateTime::currentDateTime().toString("HH:mm:ss");
}

QString params_text(const Params& params) {
    QStringList parts;
    for (const auto& [k, v] : params) parts << QString("%1=%2").arg(qstr(k)).arg(v);
    return "{" + parts.join(", ") + "}";
}

}  // namespace

// ---------------------------------------------------------------------------
// 总览仪表板（任务 3.6）
// ---------------------------------------------------------------------------

QWidget* AdvancedWindow::build_dashboard_tab() {
    auto* page = new QWidget();
    auto* outer = new QVBoxLayout(page);
    auto* head = new QHBoxLayout();
    head->addWidget(new QLabel(
        "<b>设备总览</b> — 插拔实时更新，点「查看」进入设备页控制"));
    head->addStretch(1);
    auto* battery_btn = new QPushButton("读取全部电量");
    battery_btn->setToolTip("罗技走 HID++ 电量，其余扫标准 HID Battery 页(0x85)；零写入");
    connect(battery_btn, &QPushButton::clicked, this, [this] { on_battery_all(); });
    head->addWidget(battery_btn);
    outer->addLayout(head);
    outer->addWidget(new QLabel(
        "<small>快速上手：①「设备」页选中设备看详情/控制（罗技设备有 DPI 面板） "
        "② 未知设备去「学习模式」页逆向 ③ 罗技鼠标也可用命令行 cli.exe mouse</small>"));
    auto* dash_host = new QWidget();
    dash_grid_ = new QGridLayout(dash_host);
    dash_grid_->setSpacing(8);
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setWidget(dash_host);
    outer->addWidget(scroll, 1);
    return page;
}

void AdvancedWindow::refresh_dashboard() {
    QGridLayout& grid = *dash_grid_;
    while (grid.count()) {
        auto* item = grid.takeAt(0);
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }
    if (devices_.empty()) {
        grid.addWidget(new QLabel("没有发现 HID 设备（插上外设或点刷新）"), 0, 0);
        return;
    }
    int index = 0;
    for (const auto& [key, ifaces] : devices_) {
        const DeviceInfo& first = ifaces.front();
        const auto result = fingerprint(first, db_);
        auto* card = new QGroupBox(first.product.empty() ? "(未命名)" : qstr(first.product));
        auto* card_layout = new QVBoxLayout(card);
        auto* status_label = new QLabel(status_text(result.status));
        status_label->setStyleSheet(
            QString("color: %1; font-weight: bold;").arg(status_color(result.status).name()));
        int feature_count = 0;
        if (!result.entry.is_null() && result.entry.contains("protocol"))
            feature_count = static_cast<int>(result.entry["protocol"].value("features", Json::array()).size());
        QString battery_text;
        const auto cached = battery_cache_.find(key);
        if (cached != battery_cache_.end()) battery_text = "\n电量: " + qstr(cached->second.display());
        auto* info = new QLabel(
            QString("%1\n品牌: %2 · %3 接口 · 协议功能 %4 个%5")
                .arg(key, result.brand_zh ? qstr(*result.brand_zh)
                              : result.brand ? qstr(*result.brand)
                                             : QString("?"))
                .arg(ifaces.size())
                .arg(feature_count)
                .arg(battery_text));
        auto* view_btn = new QPushButton("查看");
        connect(view_btn, &QPushButton::clicked, card, [this, key] { goto_device(key); });
        card_layout->addWidget(status_label);
        card_layout->addWidget(info);
        card_layout->addWidget(view_btn);
        grid.addWidget(card, index / 3, index++ % 3);
    }
}

void AdvancedWindow::goto_device(const QString& key) {
    tabs_->setCurrentWidget(devices_tab_);
    for (int i = 0; i < list_->count(); ++i)
        if (list_->item(i)->data(Qt::UserRole).toString() == key) {
            list_->setCurrentRow(i);
            break;
        }
}

void AdvancedWindow::on_battery_all() {
    // 全部设备电量扫描（后台线程，零写入路径）
    const auto grouped = devices_;
    emit logLine(QString("── 电量扫描开始（%1 台设备）──").arg(grouped.size()));

    auto* worker = new Worker(
        [grouped]() -> std::shared_ptr<void> {
            std::map<std::string, std::vector<pt::DeviceInfo>> grouped_std;
            for (const auto& [k, v] : grouped) grouped_std[sstr(k)] = v;
            const auto results = scan_all(
                grouped_std, [](const std::string& path) { return read_descriptor(path); });
            return std::make_shared<std::vector<BatteryResult>>(results);
        },
        this);
    connect(worker, &Worker::done, this, [this](const JobResult& result) {
        if (!result.ok) {
            emit logLine("电量扫描出错: " + result.error);
            return;
        }
        const auto& value = *static_cast<std::vector<BatteryResult>*>(result.data.get());
        battery_cache_.clear();
        for (const auto& r : value) {
            battery_cache_[qstr(r.vidpid)] = r;
            emit logLine(QString("[%1] %2: 电量 %3 — %4")
                             .arg(qstr(r.vidpid), qstr(r.product), qstr(r.display()),
                                  qstr(r.detail)));
        }
        emit logLine("── 电量扫描完成 ──");
        refresh_dashboard();
    });
    spawn_worker(worker);
}

// ---------------------------------------------------------------------------
// 学习模式页（M4.4）
// ---------------------------------------------------------------------------

QWidget* AdvancedWindow::build_learn_tab() {
    auto* page = new QWidget();
    auto* layout = new QVBoxLayout(page);

    auto* tip = new QLabel(
        "逆向未知设备配置协议：① 勾选要录制的接口 → ② 开始录制 → ③ 点「标记起点」"
        "→ ④ 去官方驱动软件改一次设置（如 DPI 800→1600）→ ⑤ 停止并分析 → ⑥ 填说明后入库（unverified）");
    tip->setWordWrap(true);
    layout->addWidget(tip);

    learn_targets_ = new QListWidget();
    learn_targets_->setToolTip("vendor 自定义页(≥0xFF00)与 rawhid(usage=0) 接口");
    layout->addWidget(learn_targets_, 1);

    auto* btn_row = new QHBoxLayout();
    learn_start_btn_ = new QPushButton("开始录制");
    learn_mark_btn_ = new QPushButton("标记起点");
    learn_stop_btn_ = new QPushButton("停止并分析");
    learn_commit_btn_ = new QPushButton("生成草稿入库（unverified）");
    learn_mark_btn_->setEnabled(false);
    learn_stop_btn_->setEnabled(false);
    learn_commit_btn_->setEnabled(false);
    connect(learn_start_btn_, &QPushButton::clicked, this, [this] { on_learn_start(); });
    connect(learn_mark_btn_, &QPushButton::clicked, this, [this] { on_learn_mark(); });
    connect(learn_stop_btn_, &QPushButton::clicked, this, [this] { on_learn_stop(); });
    connect(learn_commit_btn_, &QPushButton::clicked, this, [this] { on_learn_commit(); });
    btn_row->addWidget(learn_start_btn_);
    btn_row->addWidget(learn_mark_btn_);
    btn_row->addWidget(learn_stop_btn_);
    btn_row->addStretch(1);
    btn_row->addWidget(learn_commit_btn_);
    layout->addLayout(btn_row);

    learn_note_ = new QLineEdit();
    learn_note_->setPlaceholderText(
        "描述你刚做了什么（例: DPI 800→1600 / RGB 改红色 / 关闭呼吸灯）");
    layout->addWidget(learn_note_);

    learn_status_ = new QLabel("空闲");
    layout->addWidget(learn_status_);

    learn_output_ = new QPlainTextEdit();
    learn_output_->setReadOnly(true);
    learn_output_->setFont(QFont("Consolas", 9));
    learn_output_->setPlaceholderText("差分分析结果…");
    layout->addWidget(learn_output_, 1);

    learn_timer_ = new QTimer(this);
    learn_timer_->setInterval(500);
    connect(learn_timer_, &QTimer::timeout, this, [this] { learn_tick(); });
    return page;
}

void AdvancedWindow::refresh_learn_targets() {
    // 从当前设备列表填充可录制接口（保留勾选状态）
    std::set<QString> prev_checked;
    for (int idx = 0; idx < learn_targets_->count(); ++idx)
        if (learn_targets_->item(idx)->checkState() == Qt::Checked)
            prev_checked.insert(learn_targets_->item(idx)->text());
    learn_targets_->clear();
    for (const auto& [key, ifaces] : devices_) {
        const QString product = (ifaces.front().product.empty() ? "(未命名)"
                                                                : qstr(ifaces.front().product))
                                    .left(20);
        for (const auto& d : ifaces) {
            const bool is_vendor = d.usage_page >= 0xFF00;
            const bool is_rawhid = d.usage_page == 0x0001 && d.usage == 0x0000;
            if (!is_vendor && !is_rawhid) continue;
            const QString label =
                QString("%1/UP%2-U%3").arg(key, qhex(d.usage_page, 4), qhex(d.usage, 4));
            auto* item = new QListWidgetItem(QString("%1  %2").arg(label, product));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(prev_checked.count(label) ? Qt::Checked : Qt::Unchecked);
            Json info = Json::object();
            info["label"] = label.toStdString();
            info["vidpid"] = key.toStdString();
            info["path"] = d.path;
            info["usage_page"] = d.usage_page;
            info["usage"] = d.usage;
            const QVariant v = QVariant::fromValue(
                QByteArray::fromStdString(info.dump()));  // Json 塞进 UserRole（序列化）
            item->setData(Qt::UserRole, v);
            learn_targets_->addItem(item);
        }
    }
}

void AdvancedWindow::learn_tick() {
    if (learn_recorder_)
        learn_status_->setText(
            QString("录制中… 已捕获 %1 条").arg(learn_recorder_->entries().size()));
}

static Json item_to_json(const QListWidgetItem* item) {
    const QByteArray raw = item->data(Qt::UserRole).toByteArray();
    return Json::parse(raw.toStdString());
}

void AdvancedWindow::on_learn_start() {
    std::vector<RecorderTarget> targets;
    std::map<QString, Json> meta;
    for (int idx = 0; idx < learn_targets_->count(); ++idx) {
        auto* item = learn_targets_->item(idx);
        if (item->checkState() != Qt::Checked) continue;
        const Json info = item_to_json(item);
        std::vector<int> feature_ids;
        if (info["usage_page"].get<unsigned>() >= 0xFF00) {
            const Bytes raw = read_descriptor(info["path"].get<std::string>());
            if (!raw.empty())
                for (int i : feature_report_ids(parse_report_descriptor(raw)))
                    if (i) feature_ids.push_back(i);
        }
        targets.push_back(RecorderTarget{info["label"].get<std::string>(),
                                         info["path"].get<std::string>(), feature_ids});
        meta[qstr(info["label"].get<std::string>())] = info;
    }
    if (targets.empty()) {
        learn_status_->setText("没有勾选任何接口");
        return;
    }
    learn_label_meta_ = meta;
    learn_recorder_ = std::make_unique<Recorder>(targets);
    learn_recorder_->start();
    learn_mark_ts_.reset();
    learn_diffs_.clear();
    learn_output_->clear();
    learn_start_btn_->setEnabled(false);
    learn_mark_btn_->setEnabled(true);
    learn_stop_btn_->setEnabled(true);
    learn_commit_btn_->setEnabled(false);
    learn_timer_->start();
    learn_status_->setText("录制中… 先点「标记起点」，再去官方软件改设置");
}

void AdvancedWindow::on_learn_mark() {
    if (!learn_recorder_) return;
    learn_mark_ts_ = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    learn_status_->setText("起点已标记 —— 现在去官方软件改设置，改完回来点「停止并分析」");
}

void AdvancedWindow::on_learn_stop() {
    if (!learn_recorder_) return;
    learn_timer_->stop();
    learn_recorder_->stop();
    const auto entries = learn_recorder_->entries();
    learn_recorder_.reset();
    std::vector<CaptureEntry> before, after;
    if (!learn_mark_ts_) {
        after = entries;
        learn_status_->setText("⚠ 未标记起点：全部按「新报文」处理");
    } else {
        for (const auto& e : entries) (e.ts < *learn_mark_ts_ ? before : after).push_back(e);
        learn_status_->setText(QString("分析完成（起点前 %1 条 / 后 %2 条）")
                                   .arg(before.size())
                                   .arg(after.size()));
    }
    learn_diffs_ = diff_sessions(before, after);
    QStringList lines;
    for (const auto& d : learn_diffs_)
        for (const auto& l : format_diff(d)) lines << qstr(l);
    if (lines.isEmpty()) lines << "没有检测到差异（确认录制期间在官方软件里改了设置？）";
    learn_output_->setPlainText(lines.join("\n"));
    learn_start_btn_->setEnabled(true);
    learn_mark_btn_->setEnabled(false);
    learn_stop_btn_->setEnabled(false);
    const bool has_draft =
        std::any_of(learn_diffs_.begin(), learn_diffs_.end(),
                    [](const GroupDiff& d) { return !d.is_volatile; });
    learn_commit_btn_->setEnabled(has_draft);
}

void AdvancedWindow::on_learn_commit() {
    if (learn_diffs_.empty()) return;
    const QString note = learn_note_->text().trimmed();
    Json label_match = Json::object();
    for (const auto& [label, info] : learn_label_meta_)
        label_match[label.toStdString()] = Json{
            {"usage_page", hex(info["usage_page"].get<unsigned>(), 4)},
            {"usage", hex(info["usage"].get<unsigned>(), 4)}};
    const Json drafts = build_drafts(learn_diffs_, sstr(note), label_match);
    if (drafts.empty()) {
        learn_status_->setText("没有可入库的草稿（只有噪声流？）");
        return;
    }
    std::map<QString, Json> by_device;
    for (const auto& draft : drafts) {
        const QString source = qstr(draft["learned"]["source"].get<std::string>());
        const auto it = learn_label_meta_.find(source);
        if (it != learn_label_meta_.end()) {
            const QString vidpid = qstr(it->second.value("vidpid", std::string()));
            if (!vidpid.isEmpty()) by_device[vidpid].push_back(draft);
        }
    }
    try {
        for (const auto& [vidpid, device_drafts] : by_device) {
            const Json match = device_drafts.front().contains("match")
                                   ? device_drafts.front()["match"]
                                   : Json();
            const Json block = build_protocol_block(match, device_drafts);
            merge_into_db(db_, sstr(vidpid), block, note.isEmpty() ? std::string() : sstr(note));
        }
        save_db(db_, default_db_path());
    } catch (const std::exception& exc) {
        learn_status_->setText(QString("入库失败: %1").arg(exc.what()));
        return;
    }
    QStringList vidpids;
    for (const auto& [vidpid, _] : by_device) vidpids << vidpid;
    emit logLine(QString("── 学习模式入库: %1 条草稿 → %2（unverified）")
                     .arg(QString::number(static_cast<int>(drafts.size())), vidpids.join(", ")));
    learn_status_->setText(
        QString("✅ 已入库 %1 条草稿（unverified）。设备页切到该设备即可看到新功能按钮")
            .arg(drafts.size()));
    db_ = load_db(default_db_path());
    refresh_devices();
    learn_commit_btn_->setEnabled(false);
}

// ---------------------------------------------------------------------------
// 描述符 / vendor 探测 / 批量探测
// ---------------------------------------------------------------------------

void AdvancedWindow::on_read_descriptor() {
    const QString key = current_key();
    if (key.isEmpty()) return;
    const auto ifaces = devices_.at(key);
    desc_btn_->setEnabled(false);
    desc_status_->setText("读取中…");

    auto* worker = new Worker(
        [ifaces]() -> std::shared_ptr<void> {
            QStringList parts;
            for (const auto& d : ifaces) {
                const Bytes raw = read_descriptor(d.path);
                const QString header = QString("── 接口 %1 UP:0x%2 U:0x%3")
                                           .arg(d.interface_number)
                                           .arg(qhex(d.usage_page, 4), qhex(d.usage, 4));
                if (raw.empty()) {
                    parts << header + "\n   (读取失败：设备被系统占用或接口不可读)";
                    continue;
                }
                QStringList lines{header};
                for (const auto& line : summary(parse_report_descriptor(raw)))
                    lines << "   " + qstr(line);
                parts << lines.join("\n");
            }
            return make_text_payload(parts.join("\n\n"));
        },
        this);
    connect(worker, &Worker::done, this, [this](const JobResult& result) {
        desc_btn_->setEnabled(true);
        desc_status_->setText("");
        desc_output_->setPlainText(!result.ok ? "错误: " + result.error
                                              : static_cast<TextPayload*>(result.data.get())->text);
    });
    spawn_worker(worker);
}

void AdvancedWindow::on_vendor_probe() {
    // 单设备 vendor 配置通道只读探测（任务: 多设备测试）
    const QString key = current_key();
    if (key.isEmpty()) return;
    const auto ifaces = devices_.at(key);
    vendor_probe_btn_->setEnabled(false);
    desc_status_->setText("只读探测中…");

    auto* worker = new Worker(
        [key, ifaces]() -> std::shared_ptr<void> {
            const auto results =
                probe_vendor_interfaces(ifaces, [](const std::string& p) { return read_descriptor(p); });
            const auto rawhids =
                suspect_rawhid_interfaces(ifaces,
                                          [](const std::string& p) { return read_descriptor(p); });
            if (results.empty() && rawhids.empty())
                return make_text_payload(
                    QString("%1: 没有发现 vendor 自定义接口或 rawhid 配置通道\n"
                            "说明: 配置通道可能藏在标准接口里，先看描述符解析结果")
                        .arg(key));
            QStringList lines;
            for (const auto& r : results) {
                const QString head = QString("UP:0x%1 U:0x%2")
                                         .arg(qhex(r.usage_page, 4), qhex(r.usage, 4));
                if (r.error) {
                    lines << head + "  " + qstr(*r.error);
                    continue;
                }
                for (const auto& [report_id, data] : r.reads)
                    lines << QString("%1  feature id=%2: %3")
                                 .arg(head, qstr(report_id),
                                      data ? qstr(to_hex(*data)) : QString("<读取失败>"));
            }
            for (const auto& r : rawhids) {
                const auto it = r.reads.find("input_passive");
                const std::optional<Bytes>& data =
                    it != r.reads.end() ? it->second : std::optional<Bytes>{};
                lines << QString("UP:0x%1 U:0x%2  [rawhid] %3\n    被动监听: %4")
                             .arg(qhex(r.usage_page, 4), qhex(r.usage, 4),
                                  r.note ? qstr(*r.note) : "",
                                  data ? qstr(to_hex(*data)) : QString("<200ms 内无主动推送>"));
            }
            return make_text_payload(lines.join("\n"));
        },
        this);
    connect(worker, &Worker::done, this, [this](const JobResult& result) {
        vendor_probe_btn_->setEnabled(true);
        desc_status_->setText("");
        emit logLine(result.ok ? "Vendor 只读探测 ── "
                                     + static_cast<TextPayload*>(result.data.get())->text
                               : "Vendor 只读探测出错: " + result.error);
    });
    spawn_worker(worker);
}

void AdvancedWindow::on_batch_probe() {
    // 批量探测：多设备测试入口。已知品牌逐台探测并汇总在线状态。
    const auto devices = devices_;
    emit logLine(QString("── 批量探测开始（%1 台设备）──").arg(devices.size()));

    auto* worker = new Worker(
        [this, devices]() -> std::shared_ptr<void> {
            // 载荷: vector<pair<key, 状态>>
            struct ProbeOutcome {
                QString key;
                QString status;
            };
            auto outcomes = std::make_shared<std::vector<std::pair<QString, QString>>>();
            for (const auto& [key, ifaces] : devices) {
                const DeviceInfo& first = ifaces.front();
                const auto result = fingerprint(first, db_);
                if (!result.brand) {
                    emit logLine(QString("[%1] 跳过（未知品牌，非外设厂商）").arg(key));
                    continue;
                }
                const QString label =
                    QString("%1 %2 %3")
                        .arg(key, result.brand_zh ? qstr(*result.brand_zh)
                                                  : qstr(*result.brand),
                             qstr(first.product))
                        .trimmed();

                // 1) 有协议 + ping → ping 探测
                const Json entry = result.entry.is_null() ? Json::object() : result.entry;
                const Json proto =
                    entry.contains("protocol") ? entry["protocol"] : Json::object();
                const Json* ping = nullptr;
                if (proto.contains("features"))
                    for (const auto& f : proto["features"])
                        if (f.value("id", std::string()) == "ping") ping = &f;
                if (!proto.is_null() && ping) {
                    const Json& match = proto["transport"]["match"];
                    const DeviceInfo* target = nullptr;
                    for (const auto& d : ifaces)
                        if (qhex(d.usage_page, 4) == qstr(match["usage_page"].get<std::string>())
                            && qhex(d.usage, 4) == qstr(match["usage"].get<std::string>()))
                            target = &d;
                    if (!target) {
                        emit logLine(QString("[%1] 协议接口未枚举到（UP:%2 U:%3）")
                                         .arg(label, qstr(match["usage_page"].get<std::string>()),
                                              qstr(match["usage"].get<std::string>())));
                        outcomes->emplace_back(key, "无接口");
                        continue;
                    }
                    ReportIO io(target->path);
                    io.set_history_callback([this](const HistoryEntry& e) {
                        emit logLine(QString("%1 %2")
                                         .arg(e.direction == "tx" ? "TX>" : "RX<",
                                              qstr(to_hex(e.data))));
                    });
                    bool ok = false;
                    std::string err;
                    try {
                        io.open();
                        const auto r =
                            execute_feature(io, *ping, nullptr, 400);
                        if (!r.response)
                            emit logLine(QString("[%1] ping 无响应").arg(label));
                        else if (r.ok)
                            emit logLine(
                                QString("[%1] ping ✅ HID++ %2.%3")
                                    .arg(label)
                                    .arg(r.decoded.count("major") && r.decoded.at("major")
                                             ? *r.decoded.at("major")
                                             : 0)
                                    .arg(r.decoded.count("minor") && r.decoded.at("minor")
                                             ? *r.decoded.at("minor")
                                             : 0));
                        else
                            emit logLine(QString("[%1] ping 应答: %2 error=0x%3")
                                             .arg(label,
                                                  r.branch ? qstr(*r.branch) : QString("未匹配"),
                                                  QString::number(
                                                      r.decoded.count("error_code")
                                                              && r.decoded.at("error_code")
                                                          ? *r.decoded.at("error_code")
                                                          : 0,
                                                      16)));
                        ok = r.response != std::nullopt;
                    } catch (const std::exception& exc) {
                        err = exc.what();
                    }
                    io.close();
                    if (!err.empty()) {
                        emit logLine(QString("[%1] ping 出错: %2").arg(label, qstr(err)));
                        outcomes->emplace_back(key, "出错");
                        continue;
                    }
                    outcomes->emplace_back(key, ok ? "在线" : "不在线");
                    continue;
                }

                // 2) 无协议 → vendor 只读探测 + rawhid 通道识别
                const auto results = probe_vendor_interfaces(
                    ifaces, [](const std::string& p) { return read_descriptor(p); });
                const auto rawhids = suspect_rawhid_interfaces(
                    ifaces, [](const std::string& p) { return read_descriptor(p); });
                if (results.empty() && rawhids.empty()) {
                    emit logLine(QString("[%1] 无 vendor 自定义接口/rawhid 配置通道").arg(label));
                    outcomes->emplace_back(key, "无配置通道");
                    continue;
                }
                int channel_count = 0;
                for (const auto& r : results) {
                    channel_count += static_cast<int>(r.reads.size());
                    for (const auto& [report_id, data] : r.reads)
                        emit logLine(QString("[%1] UP:0x%2 feature id=%3: %4")
                                         .arg(label, qhex(r.usage_page, 4), qstr(report_id),
                                              data ? qstr(to_hex(*data)) : QString("<读取失败>")));
                }
                for (const auto& r : rawhids)
                    emit logLine(QString("[%1] rawhid 配置通道: UP:0x%2 U:0x%3（写入能力待 M4 验证）")
                                     .arg(label, qhex(r.usage_page, 4), qhex(r.usage, 4)));
                outcomes->emplace_back(
                    key, QString("探测到 %1 个配置通道").arg(channel_count + static_cast<int>(rawhids.size())));
            }
            return outcomes;
        },
        this);
    connect(worker, &Worker::done, this, [this](const JobResult& result) {
        if (!result.ok) {
            emit logLine("批量探测出错: " + result.error);
            return;
        }
        const auto& value =
            *static_cast<std::vector<std::pair<QString, QString>>*>(result.data.get());
        int online = 0;
        for (const auto& [k, s] : value)
            if (s == "在线") ++online;
        emit logLine(QString("── 批量探测完成: 已知品牌 %1 台，在线 %2 台 ──")
                         .arg(value.size())
                         .arg(online));
    });
    spawn_worker(worker);
}

void AdvancedWindow::on_run_feature(const QString& key, const Json& feature,
                                    const std::map<QString, QSpinBox*>& spins) {
    const auto& ifaces = devices_.at(key);
    const Json entry = [&] {
        const auto r = fingerprint(ifaces.front(), db_);
        return r.entry.is_null() ? Json::object() : r.entry;
    }();
    const Json& proto = entry["protocol"];
    const Json match = feature.contains("match") ? feature["match"]
                                                 : proto["transport"]["match"];  // 功能级优先
    const DeviceInfo* target = nullptr;
    for (const auto& d : ifaces)
        if (qhex(d.usage_page, 4) == qstr(match["usage_page"].get<std::string>())
            && qhex(d.usage, 4) == qstr(match["usage"].get<std::string>()))
            target = &d;
    Params params;
    for (const auto& [name, spin] : spins) params[sstr(name)] = spin->value();
    emit logLine(qstr("── 执行 " + feature.at("id").get<std::string>() + " 参数 ")
                 + params_text(params));

    auto* worker = new Worker(
        [this, ifaces, target, feature, params, entry]() -> std::shared_ptr<void> {
            if (!target)
                return make_text_payload(
                    QString("找不到协议要求的接口（UP:%1 U:%2）")
                        .arg(qstr(entry["protocol"]["transport"]["match"]["usage_page"]
                                      .get<std::string>()),
                             qstr(entry["protocol"]["transport"]["match"]["usage"]
                                      .get<std::string>())));
            ReportIO io(target->path);
            io.set_history_callback([this](const HistoryEntry& e) {
                emit logLine(QString("%1 %2")
                                 .arg(e.direction == "tx" ? "TX>" : "RX<", qstr(to_hex(e.data))));
            });
            QString out;
            try {
                io.open();
                const int timeout_ms =
                    entry["protocol"]["transport"].value("response_timeout_ms", 600);
                const auto result = execute_feature(io, feature, &params, timeout_ms);
                io.close();
                if (!result.response) {
                    out = "无响应（超时）";
                } else {
                    out = QString("响应 %1 字节: %2")
                              .arg(QString::number(static_cast<int>(result.response->size())),
                              qstr(to_hex(*result.response)));
                    if (result.ok) {
                        QStringList decoded;
                        for (const auto& [k, v] : result.decoded)
                            decoded << (v ? QString("%1=0x%2").arg(qstr(k), qhex(
                                                          static_cast<unsigned>(*v), 2))
                                          : QString("%1=%2").arg(qstr(k), "-"));
                        out += "\n✅ 主应答: " + (decoded.isEmpty() ? QString("匹配") : decoded.join(", "));
                    } else if (result.branch) {
                        QStringList decoded;
                        for (const auto& [k, v] : result.decoded)
                            if (v)
                                decoded << QString("%1=0x%2").arg(qstr(k), qhex(
                                                                    static_cast<unsigned>(*v), 2));
                        out += QString("\nℹ 备用应答（%1）: %2").arg(qstr(*result.branch), decoded.join(", "));
                    }
                }
            } catch (const std::exception& exc) {
                io.close();
                return make_text_payload(QString("执行异常: %1").arg(exc.what()));
            }
            return make_text_payload(out);
        },
        this);
    connect(worker, &Worker::done, this, [this](const JobResult& result) {
        emit logLine(result.ok ? "执行完成\n"
                                     + static_cast<TextPayload*>(result.data.get())->text
                               : "执行出错: " + result.error);
    });
    spawn_worker(worker);
}

// ---------------------------------------------------------------------------
// 配置档案（任务 3.5）
// ---------------------------------------------------------------------------

void AdvancedWindow::reload_profile_combo(const QString& key) {
    profile_combo_->clear();
    for (const auto& path : list_profiles(sstr(key), default_profile_dir()))
        profile_combo_->addItem(QString::fromStdString(path.stem().string()),
                                QString::fromStdString(path.string()));
}

void AdvancedWindow::on_profile_save() {
    const QString key = current_key();
    if (key.isEmpty()) return;
    const QString name = profile_name_->text().trimmed().isEmpty()
                             ? QString("profile_%1").arg(
                                 QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"))
                             : profile_name_->text().trimmed();
    DeviceProfile profile;
    profile.vidpid = sstr(key);
    profile.name = sstr(name);
    for (const auto& [feature, spins] : proto_controls_) {
        ProfileFeature pf;
        pf.feature_id = feature.at("id").get<std::string>();
        for (const auto& [n, s] : spins) pf.params[sstr(n)] = s->value();
        profile.features.push_back(std::move(pf));
    }
    std::filesystem::path path;
    try {
        path = save_profile(profile, default_profile_dir());
    } catch (const std::exception& exc) {
        emit logLine(QString("档案保存失败: %1").arg(exc.what()));
        return;
    }
    profile_name_->clear();
    reload_profile_combo(key);
    profile_combo_->setCurrentText(QString::fromStdString(path.stem().string()));
    emit logLine(QString("── 档案已保存: %1（%2，%3 个功能）")
                     .arg(QString::fromStdString(path.filename().string()), key)
                     .arg(static_cast<int>(profile.features.size())));
}

void AdvancedWindow::on_profile_load() {
    const QVariant data = profile_combo_->currentData();
    if (!data.isValid()) return;
    const auto profile = load_profile(std::filesystem::path(sstr(data.toString())));
    int restored = 0;
    for (const auto& pf : profile.features)
        for (const auto& [feature, spins] : proto_controls_) {
            if (feature.at("id").get<std::string>() != pf.feature_id) continue;
            for (const auto& [name, value] : pf.params) {
                const auto it = spins.find(qstr(name));
                if (it != spins.end()) {
                    it->second->setValue(value);
                    ++restored;
                }
            }
        }
    emit logLine(QString("── 档案「%1」已加载到控件（%2 个参数）")
                     .arg(qstr(profile.name))
                     .arg(restored));
}

void AdvancedWindow::on_profile_apply() {
    const QString key = current_key();
    const QVariant data = profile_combo_->currentData();
    if (key.isEmpty() || !data.isValid()) return;
    const auto profile = load_profile(std::filesystem::path(sstr(data.toString())));
    const auto ifaces = devices_.at(key);
    const Json entry = [&] {
        const auto r = fingerprint(ifaces.front(), db_);
        return r.entry.is_null() ? Json::object() : r.entry;
    }();
    if (entry.is_null() || !entry.contains("protocol")) {
        emit logLine(QString("[%1] 无协议库条目，无法应用档案").arg(key));
        return;
    }
    const Json& proto = entry["protocol"];
    std::map<std::string, Json> features_by_id;
    if (proto.contains("features"))
        for (const auto& f : proto["features"]) features_by_id[f["id"].get<std::string>()] = f;
    const int timeout_ms = proto["transport"].value("response_timeout_ms", 600);
    emit logLine(QString("── 应用档案「%1」到 %2（%3 个功能）")
                     .arg(qstr(profile.name), key)
                     .arg(static_cast<int>(profile.features.size())));

    auto* worker = new Worker(
        [this, key, ifaces, entry, proto, features_by_id, profile, timeout_ms]()
            -> std::shared_ptr<void> {
            QStringList lines;
            for (const auto& pf : profile.features) {
                const auto fit = features_by_id.find(pf.feature_id);
                if (fit == features_by_id.end()) {
                    lines << QString("[%1] 协议库中已不存在，跳过").arg(qstr(pf.feature_id));
                    continue;
                }
                const Json& feature = fit->second;
                const Json match = feature.contains("match") ? feature["match"]
                                                             : proto["transport"]["match"];
                const DeviceInfo* target = nullptr;
                for (const auto& d : ifaces)
                    if (qhex(d.usage_page, 4) == qstr(match["usage_page"].get<std::string>())
                        && qhex(d.usage, 4) == qstr(match["usage"].get<std::string>()))
                        target = &d;
                if (!target) {
                    lines << QString("[%1] 接口 UP:%2 未枚举到，跳过")
                                 .arg(qstr(pf.feature_id),
                                      qstr(match["usage_page"].get<std::string>()));
                    continue;
                }
                ReportIO io(target->path);
                io.set_history_callback([this](const HistoryEntry& e) {
                    emit logLine(QString("%1 %2")
                                     .arg(e.direction == "tx" ? "TX>" : "RX<",
                                          qstr(to_hex(e.data))));
                });
                try {
                    io.open();
                    Params params = pf.params;
                    if (feature.value("kind", std::string()) == "write") {
                        const Json* read_feature = nullptr;
                        if (feature.contains("read_from")) {
                            const auto rit =
                                features_by_id.find(feature["read_from"].get<std::string>());
                            if (rit != features_by_id.end()) read_feature = &rit->second;
                        }
                        const auto outcome =
                            safe_write(io, feature, read_feature, &params, nullptr, nullptr,
                                       timeout_ms);
                        lines << QString("[%1] write %2 verified=%3 rolled_back=%4")
                                     .arg(qstr(pf.feature_id), outcome.ok ? "✅" : "❌")
                                     .arg(outcome.verified)
                                     .arg(outcome.rolled_back);
                    } else {
                        const auto result = execute_feature(io, feature, &params, timeout_ms);
                        if (!result.response)
                            lines << QString("[%1] 无响应（超时）").arg(qstr(pf.feature_id));
                        else if (result.ok)
                            lines << QString("[%1] ✅ 主应答").arg(qstr(pf.feature_id));
                        else
                            lines << QString("[%1] ℹ %2")
                                         .arg(qstr(pf.feature_id),
                                              result.branch ? qstr(*result.branch) : "未匹配");
                    }
                    io.close();
                } catch (const std::exception& exc) {
                    io.close();
                    lines << QString("[%1] 异常: %2").arg(qstr(pf.feature_id), qstr(exc.what()));
                }
            }
            return make_text_payload(lines.isEmpty() ? QString("没有可应用的功能")
                                                     : lines.join("\n"));
        },
        this);
    connect(worker, &Worker::done, this, [this](const JobResult& result) {
        emit logLine(result.ok ? "应用完成\n"
                                     + static_cast<TextPayload*>(result.data.get())->text
                               : "应用出错: " + result.error);
    });
    spawn_worker(worker);
}

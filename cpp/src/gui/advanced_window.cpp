// advanced_window.cpp — 高级窗口：构造/UI/设备列表/详情页/协议面板/DPI 面板。
// （gui.py AdvancedWindow 前半部分平移）
#include "advanced_window.hpp"

#include <QComboBox>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QStatusBar>
#include <QVBoxLayout>

#include <algorithm>

#include "core/descriptor.hpp"
#include "core/dpi.hpp"
#include "core/fingerprint.hpp"
#include "core/hidpp.hpp"
#include "core/report_io.hpp"
#include "gui_helpers.hpp"
#include "gui_payloads.hpp"

using namespace pt;
using namespace pt::gui;

AdvancedWindow::AdvancedWindow() : settings_("AvR0raa", "peripheral-tool") {
    setWindowTitle("高级模式（协议开发） — 外设万能调试器 " + APP_VERSION);
    resize(1080, 720);

    db_ = load_db(default_db_path());

    build_ui();
    connect(this, &AdvancedWindow::logLine, this,
            [this](const QString& line) { append_log(line); });

    // 设置持久化（任务 3.7）：窗口几何
    const QVariant geometry = settings_.value("window/geometry");
    if (geometry.isValid()) restoreGeometry(geometry.toByteArray());

    refresh_devices();

    // 恢复上次会话：标签页 + 选中设备
    bool tab_ok = false;
    const int tab_index = settings_.value("ui/tab", 1).toInt(&tab_ok);
    if (tab_ok && 0 <= tab_index && tab_index < tabs_->count())
        tabs_->setCurrentIndex(tab_index);
    const QString last_key = settings_.value("ui/selected_device", "").toString();
    if (!last_key.isEmpty())
        for (int i = 0; i < list_->count(); ++i)
            if (list_->item(i)->data(Qt::UserRole).toString() == last_key) {
                list_->setCurrentRow(i);
                break;
            }

    start_watcher();
}

void AdvancedWindow::build_ui() {
    auto* root = new QWidget();
    auto* layout = new QVBoxLayout(root);

    auto* splitter = new QSplitter(Qt::Vertical);
    auto* top = new QSplitter(Qt::Horizontal);

    // 左：设备列表
    auto* left = new QWidget();
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(0, 0, 0, 0);
    auto* header = new QHBoxLayout();
    auto* title = new QLabel("<b>设备列表</b>");
    count_label_ = new QLabel("");
    auto* batch_btn = new QPushButton("批量探测");
    batch_btn->setToolTip(
        "逐台探测已知品牌设备：有协议的跑 ping，否则只读探测 vendor 配置通道");
    connect(batch_btn, &QPushButton::clicked, this, [this] { on_batch_probe(); });
    auto* refresh_btn = new QPushButton("刷新");
    connect(refresh_btn, &QPushButton::clicked, this, [this] { refresh_devices(); });
    header->addWidget(title);
    header->addStretch(1);
    header->addWidget(count_label_);
    header->addWidget(batch_btn);
    header->addWidget(refresh_btn);
    left_layout->addLayout(header);

    list_ = new QListWidget();
    connect(list_, &QListWidget::itemSelectionChanged, this, [this] { on_select(); });
    left_layout->addWidget(list_);
    top->addWidget(left);

    // 右：详情
    auto* right = new QWidget();
    auto* right_layout = new QVBoxLayout(right);
    right_layout->setContentsMargins(0, 0, 0, 0);

    detail_title_ = new QLabel("选择左侧设备查看详情");
    detail_title_->setTextFormat(Qt::RichText);
    right_layout->addWidget(detail_title_);

    iface_table_ = new QTableWidget(0, 3);
    iface_table_->setHorizontalHeaderLabels({"接口", "UP:USAGE", "说明"});
    iface_table_->horizontalHeader()->setStretchLastSection(true);
    iface_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    iface_table_->verticalHeader()->setVisible(false);
    right_layout->addWidget(iface_table_);

    auto* desc_row = new QHBoxLayout();
    desc_btn_ = new QPushButton("读取并解析报告描述符");
    connect(desc_btn_, &QPushButton::clicked, this, [this] { on_read_descriptor(); });
    vendor_probe_btn_ = new QPushButton("Vendor 只读探测");
    vendor_probe_btn_->setToolTip(
        "对 vendor 自定义接口(usage page ≥ 0xFF00)只读 feature report——多设备测试/逆向起点");
    connect(vendor_probe_btn_, &QPushButton::clicked, this, [this] { on_vendor_probe(); });
    desc_status_ = new QLabel("");
    desc_row->addWidget(desc_btn_);
    desc_row->addWidget(vendor_probe_btn_);
    desc_row->addWidget(desc_status_, 1);
    right_layout->addLayout(desc_row);

    desc_output_ = new QPlainTextEdit();
    desc_output_->setReadOnly(true);
    desc_output_->setFont(QFont("Consolas", 9));
    desc_output_->setPlaceholderText("描述符解析结果…");
    desc_output_->setMaximumHeight(160);
    right_layout->addWidget(desc_output_);

    protocol_box_ = new QVBoxLayout();
    right_layout->addLayout(protocol_box_);

    // 配置档案区（任务 3.5）
    auto* profile_row = new QHBoxLayout();
    profile_name_ = new QLineEdit();
    profile_name_->setPlaceholderText("档案名（保存时用）");
    auto* profile_save_btn = new QPushButton("保存档案");
    profile_save_btn->setToolTip("把当前所有功能控件的参数值存为该设备的配置档案");
    connect(profile_save_btn, &QPushButton::clicked, this, [this] { on_profile_save(); });
    profile_combo_ = new QComboBox();
    auto* profile_load_btn = new QPushButton("加载到控件");
    connect(profile_load_btn, &QPushButton::clicked, this, [this] { on_profile_load(); });
    auto* profile_apply_btn = new QPushButton("应用档案（执行全部）");
    profile_apply_btn->setToolTip("按档案参数逐个执行协议功能；write 功能走安全写路径");
    connect(profile_apply_btn, &QPushButton::clicked, this, [this] { on_profile_apply(); });
    profile_row->addWidget(profile_name_, 1);
    profile_row->addWidget(profile_save_btn);
    profile_row->addWidget(profile_combo_, 1);
    profile_row->addWidget(profile_load_btn);
    profile_row->addWidget(profile_apply_btn);
    right_layout->addLayout(profile_row);
    right_layout->addStretch(1);
    top->addWidget(right);
    top->setSizes({380, 700});

    // 下：hex 日志
    log_ = new QPlainTextEdit();
    log_->setReadOnly(true);
    log_->setFont(QFont("Consolas", 9));
    log_->setPlaceholderText("TX/RX hex 日志…");
    log_->setMaximumHeight(180);

    splitter->addWidget(top);
    splitter->addWidget(log_);
    layout->addWidget(splitter);

    // 顶部标签页：总览（3.6）+ 设备页 + 学习模式页
    devices_tab_ = root;
    tabs_ = new QTabWidget();
    tabs_->addTab(build_dashboard_tab(), "总览");
    tabs_->addTab(root, "设备");
    tabs_->addTab(build_learn_tab(), "学习模式");
    setCentralWidget(tabs_);
}

// ---- 设备列表 ----

void AdvancedWindow::refresh_devices() {
    std::vector<DeviceInfo> devices;
    try {
        devices = list_devices();
    } catch (const HIDError& exc) {
        count_label_->setText("枚举失败: " + qstr(exc.what()));
        return;
    }
    std::map<QString, std::vector<DeviceInfo>> grouped;
    for (const auto& d : devices) grouped[qstr(d.vidpid())].push_back(d);
    devices_ = grouped;
    count_label_->setText(QString("%1 台设备 / %2 接口").arg(grouped.size()).arg(devices.size()));

    const QString selected = current_key();
    list_->clear();
    for (const auto& [key, ifaces] : grouped) {
        const DeviceInfo& first = ifaces.front();
        const auto result = fingerprint(first, db_);
        const QString product =
        first.product.empty() ? QString("(未命名)") : qstr(first.product);
        auto* item = new QListWidgetItem(QString("%1 %2  %3  (%4接口)")
                                             .arg(status_icon(result.status), key, product)
                                             .arg(ifaces.size()));
        item->setForeground(status_color(result.status));
        item->setData(Qt::UserRole, key);
        item->setToolTip(status_text(result.status) + "\n" + qstr(result.detail));
        list_->addItem(item);
        if (key == selected) item->setSelected(true);
    }
    refresh_learn_targets();
    refresh_dashboard();
}

QString AdvancedWindow::current_key() const {
    const auto items = list_->selectedItems();
    return items.isEmpty() ? QString() : items.first()->data(Qt::UserRole).toString();
}

void AdvancedWindow::start_watcher() {
    watcher_ = new HotplugWatcher(this);
    connect(watcher_, &HotplugWatcher::eventHappened, this,
            [this](const DeviceEvent& event) { on_hotplug(event); });
    watcher_->start();
}

void AdvancedWindow::on_hotplug(const DeviceEvent& event) {
    const auto& d = event.device;
    const QString action = event.kind == "added" ? "＋ 接入" : "－ 移除";
    emit logLine(QString("── 热插拔 %1: %2:%3 %4")
                     .arg(action, qstr(d.vid_hex()), qstr(d.pid_hex()),
                          d.product.empty() ? "(未命名)" : qstr(d.product)));
    refresh_devices();
}

void AdvancedWindow::spawn_worker(Worker* worker) {
    // Worker 无 parent（构造时不传窗口）：窗口关闭时任务自然跑完、自清理；
    // done 信号在 spawn 调用点以窗口为接收者上下文连接 → 窗口销毁自动断连。
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void AdvancedWindow::closeEvent(QCloseEvent* event) {
    settings_.setValue("window/geometry", saveGeometry());
    settings_.setValue("ui/tab", tabs_->currentIndex());
    settings_.setValue("ui/selected_device", current_key());
    if (watcher_) {
        watcher_->stop_watching();
        watcher_->wait(2000);  // 轮询 25ms 步进检查 stop，2s 绰绰有余
    }
    if (learn_recorder_) {
        learn_timer_->stop();
        learn_recorder_->stop();
    }
    deleteLater();  // 堆分配窗口：关闭即回收（worker 无 parent，不受影响）
    QMainWindow::closeEvent(event);
}

// ---- 详情页 ----

void AdvancedWindow::on_select() {
    const QString key = current_key();
    desc_output_->clear();
    desc_status_->setText("");
    clear_protocol_panel();
    if (key.isEmpty()) {
        profile_combo_->clear();
        detail_title_->setText("选择左侧设备查看详情");
        return;
    }
    reload_profile_combo(key);
    const auto& ifaces = devices_.at(key);
    const DeviceInfo& first = ifaces.front();
    const auto result = fingerprint(first, db_);

    const QString product_text =
        first.product.empty() ? QString("(未命名)") : qstr(first.product);
    const QString brand_text = result.brand_zh ? qstr(*result.brand_zh)
                                : result.brand ? qstr(*result.brand)
                                               : QString("未知品牌");
    const QString serial_text = first.serial_number.empty() ? "-" : qstr(first.serial_number);
    QString title = QString("<b>%1</b> &nbsp; <span style='color:%2'>%3</span> &nbsp;|&nbsp; 品牌: %4 "
                            "&nbsp;|&nbsp; 序列号: %5")
                        .arg(product_text, status_color(result.status).name(),
                             status_text(result.status), brand_text, serial_text);
    if (!result.entry.is_null()) title += QString("<br><small>%1</small>").arg(qstr(result.detail));
    detail_title_->setText(title);

    iface_table_->setRowCount(0);
    for (std::size_t row_i = 0; row_i < ifaces.size(); ++row_i) {
        const DeviceInfo& d = ifaces[row_i];
        const int row = iface_table_->rowCount();
        iface_table_->insertRow(row);
        const QString vendor_mark = d.usage_page >= 0xFF00 ? "（厂商自定义）" : "";
        iface_table_->setItem(row, 0, new QTableWidgetItem(QString::number(d.interface_number)));
        iface_table_->setItem(
            row, 1,
            new QTableWidgetItem(QString("0x%1:0x%2")
                                     .arg(qhex(d.usage_page, 4), qhex(d.usage, 4))));
        const QString note = row_i == 0 ? qstr(result.detail) : QString();
        iface_table_->setItem(row, 2, new QTableWidgetItem(note + vendor_mark));
    }

    build_protocol_panel(key, ifaces, result);
}

void AdvancedWindow::clear_protocol_panel() {
    proto_controls_.clear();
    while (protocol_box_->count()) {
        QLayoutItem* item = protocol_box_->takeAt(0);
        if (auto* w = item->widget()) {
            w->setParent(nullptr);
            w->deleteLater();
        } else if (auto* lay = item->layout()) {
            delete lay;
        }
        delete item;
    }
}

void AdvancedWindow::build_protocol_panel(const QString& key,
                                          const std::vector<DeviceInfo>& ifaces,
                                          const FingerprintResult& result) {
    // 按协议库动态生成功能控件（任务 3.2）
    const Json& entry = result.entry;
    if (entry.is_null() || !entry.contains("protocol")) {
        if (result.status == DeviceStatus::Unknown) {
            auto* box = new QGroupBox("协议功能");
            auto* lay = new QVBoxLayout(box);
            lay->addWidget(new QLabel("未知设备：M4 学习模式上线后可逆向协议"));
            protocol_box_->addWidget(box);
        }
        return;
    }

    const Json& proto = entry["protocol"];
    const Json& match = proto["transport"]["match"];
    std::vector<const DeviceInfo*> target;
    for (const auto& d : ifaces)
        if (qhex(d.usage_page, 4) == qstr(match["usage_page"].get<std::string>())
            && qhex(d.usage, 4) == qstr(match["usage"].get<std::string>()))
            target.push_back(&d);
    auto* info = new QLabel(
        QString("协议: %1（接口匹配 UP:%2 U:%3%4）")
            .arg(qstr(proto["name"].get<std::string>()), qstr(match["usage_page"].get<std::string>()),
                 qstr(match["usage"].get<std::string>()),
                 target.empty() ? "，⚠ 未枚举到该接口" : "，已找到"));
    protocol_box_->addWidget(info);

    if (proto.contains("features")) {
        for (const auto& feature : proto["features"]) {
            auto* box = new QGroupBox(qstr(feature.value("label", feature.at("id").get<std::string>())));
            auto* form = new QFormLayout(box);
            std::map<QString, QSpinBox*> spinboxes;
            if (feature.contains("params")) {
                for (auto it = feature["params"].begin(); it != feature["params"].end(); ++it) {
                    const Json& spec = it.value();
                    const int low = spec.value("min", 0);
                    const int high = spec.value("max", 255);
                    auto* spin = new QSpinBox();
                    spin->setRange(low, high);
                    spin->setValue(spec.value("default", 0));
                    if (high - low > 3) {  // 范围大 → 配滑条（任务 3.4）
                        auto* slider = new QSlider(Qt::Horizontal);
                        slider->setRange(low, high);
                        slider->setValue(spin->value());
                        connect(spin, &QSpinBox::valueChanged, slider, &QSlider::setValue);
                        connect(slider, &QSlider::valueChanged, spin, &QSpinBox::setValue);
                        form->addRow(qstr(spec.value("label", it.key())), spin);
                        form->addRow(QString(), slider);
                    } else {
                        form->addRow(qstr(spec.value("label", it.key())), spin);
                    }
                    spinboxes[QString::fromStdString(it.key())] = spin;
                }
            }
            ProtoControl control{feature, spinboxes};
            auto* run_btn = new QPushButton("执行");
            connect(run_btn, &QPushButton::clicked, box, [this, key, control] {
                on_run_feature(key, control.feature, control.spins);
            });
            form->addRow(run_btn);
            proto_controls_.push_back(std::move(control));
            protocol_box_->addWidget(box);
        }
    }

    if (result.brand && *result.brand == "Logitech") build_dpi_panel(key, ifaces);
}

void AdvancedWindow::build_dpi_panel(const QString& key, const std::vector<DeviceInfo>& ifaces) {
    // 罗技设备 DPI 控制面板（M2.5b，HID++ 0x2201）
    auto* box = new QGroupBox("鼠标 DPI（HID++ Adjustable DPI）");
    auto* form = new QFormLayout(box);

    auto* slot_spin = new QSpinBox();
    slot_spin->setRange(0, 7);
    slot_spin->setValue(1);
    form->addRow("接收器槽位（1-7，直连选 0=0xFF）", slot_spin);

    auto* dpi_spin = new QSpinBox();
    dpi_spin->setRange(50, 32000);
    dpi_spin->setSingleStep(50);
    dpi_spin->setValue(800);
    auto* dpi_slider = new QSlider(Qt::Horizontal);
    dpi_slider->setRange(50, 32000);
    dpi_slider->setValue(800);
    connect(dpi_spin, &QSpinBox::valueChanged, dpi_slider, &QSlider::setValue);
    connect(dpi_slider, &QSlider::valueChanged, dpi_spin, &QSpinBox::setValue);
    form->addRow("DPI", dpi_spin);
    form->addRow(QString(), dpi_slider);

    auto* info = new QLabel("—");
    info->setWordWrap(true);

    auto* read_btn = new QPushButton("读取当前 DPI / 档位表");
    connect(read_btn, &QPushButton::clicked, box, [this, key, ifaces, slot_spin, dpi_spin, info] {
        const int dev = slot_spin->value() == 0 ? 0xFF : slot_spin->value();
        info->setText("连接中…（设备睡眠时 HID++ 流量会唤醒它，请动一动鼠标）");
        auto* worker = new Worker(
            [ifaces, dev]() -> std::shared_ptr<void> {
                struct ReadResult {
                    DpiRead dpi;
                    std::vector<int> lst;
                    bool ok = false;
                    QString err;
                };
                const DeviceInfo* short_if = nullptr;
                const DeviceInfo* long_if = nullptr;
                for (const auto& d : ifaces) {
                    if (qhex(d.usage_page, 4) == "FF00" && qhex(d.usage, 4) == "0001")
                        short_if = &d;
                    if (qhex(d.usage_page, 4) == "FF00" && qhex(d.usage, 4) == "0002")
                        long_if = &d;
                }
                if (!short_if && !long_if) {
                    auto r = std::make_shared<ReadResult>();
                    r->err = "未找到 FF00 HID++ 接口";
                    return r;
                }
                HidppLink link(short_if ? short_if->path : std::string(),
                               long_if ? long_if->path : std::string());
                link.open();
                struct LinkCloser {
                    HidppLink& l;
                    ~LinkCloser() { l.close(); }
                } closer{link};
                auto r = std::make_shared<ReadResult>();
                if (!ensure_online(link, dev, 10)) {
                    r->err = "设备不在线（请动一动鼠标再试）";
                    return r;
                }
                r->ok = true;
                r->dpi = read_dpi(link, dev);
                r->lst = get_dpi_list(link, dev);
                return r;
            },
            this);
        connect(worker, &Worker::done, this,
                [info, dpi_spin](const JobResult& result) {
                    if (!result.ok) {
                        info->setText("读取出错: " + result.error);
                        return;
                    }
                    const auto& r = *static_cast<DpiReadPayloadShared*>(result.data.get());
                    if (!r.ok) {
                        info->setText(r.err);
                        return;
                    }
                    if (!r.dpi.x) {
                        info->setText("读取失败（设备可能睡眠中，动一动鼠标再试）");
                        return;
                    }
                    const QString range_text =
                        r.lst.empty()
                            ? "档位表未知"
                            : QString("档位 %1-%2（%3 档）").arg(r.lst.front()).arg(r.lst.back()).arg(r.lst.size());
                    QString extra;
                    if (r.dpi.y) extra += QString(" · Y=%1").arg(*r.dpi.y);
                    if (r.dpi.lod) extra += QString(" · LOD=%1").arg(*r.dpi.lod);
                    info->setText(
                        QString("当前 %1 DPI%2 · %3").arg(QString::number(*r.dpi.x), extra, range_text));
                    if (!r.lst.empty()) {
                        dpi_spin->setRange(r.lst.front(), r.lst.back());
                    }
                });
        spawn_worker(worker);
    });

    auto* set_btn = new QPushButton("应用 DPI（读回验证+失败自动回滚）");
    connect(set_btn, &QPushButton::clicked, box, [this, key, ifaces, slot_spin, dpi_spin] {
        const int dev = slot_spin->value() == 0 ? 0xFF : slot_spin->value();
        const int target = dpi_spin->value();
        emit logLine(QString("── DPI 设置: 槽位%1 → %2").arg(slot_spin->value()).arg(target));
        auto* worker = new Worker(
            [ifaces, dev, target]() -> std::shared_ptr<void> {
                const DeviceInfo* short_if = nullptr;
                const DeviceInfo* long_if = nullptr;
                for (const auto& d : ifaces) {
                    if (qhex(d.usage_page, 4) == "FF00" && qhex(d.usage, 4) == "0001")
                        short_if = &d;
                    if (qhex(d.usage_page, 4) == "FF00" && qhex(d.usage, 4) == "0002")
                        long_if = &d;
                }
                if (!short_if && !long_if) return make_text_payload("未找到 FF00 HID++ 接口");
                HidppLink link(short_if ? short_if->path : std::string(),
                               long_if ? long_if->path : std::string());
                link.open();
                struct LinkCloser {
                    HidppLink& l;
                    ~LinkCloser() { l.close(); }
                } closer{link};
                if (!ensure_online(link, dev, 6))
                    return make_text_payload("设备不在线（请动一动鼠标再试）");
                const auto outcome = pt::set_dpi(link, dev, target);
                QStringList lines;
                for (const auto& l : outcome.log) lines << qstr(l);
                return make_text_payload(lines.join("\n"));
            },
            this);
        connect(worker, &Worker::done, this, [this](const JobResult& result) {
            emit logLine(result.ok ? "DPI 结果\n" + static_cast<TextPayload*>(result.data.get())->text
                                   : "DPI 设置出错: " + result.error);
        });
        spawn_worker(worker);
    });

    form->addRow(read_btn);
    form->addRow(set_btn);
    form->addRow(info);
    protocol_box_->addWidget(box);
}

void AdvancedWindow::append_log(const QString& line) {
    log_->appendPlainText(line);
}

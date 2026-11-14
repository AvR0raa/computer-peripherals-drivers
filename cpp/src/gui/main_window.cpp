// main_window.cpp — 用户客户端主窗口实现（gui.py MainWindow 平移）。
#include "main_window.hpp"

#include "migration_dialog.hpp"
#include "tray.hpp"

#include <QContextMenuEvent>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStatusBar>
#include <QVBoxLayout>

#include "advanced_window.hpp"
#include "core/device_manager.hpp"
#include "gui_helpers.hpp"

#include <algorithm>

#include <set>

using namespace pt;
using namespace pt::gui;

namespace {

// 卡片状态徽章文案与颜色
std::pair<QString, const char*> chip_for(DeviceStatus s) {
    switch (s) {
        case DeviceStatus::Known: return {"已识别 · 可控制", "#3DDC97"};
        case DeviceStatus::Sdk: return {"官方 SDK 可用", "#4F8CFF"};
        default: return {"未识别", "#8A94A7"};
    }
}

QDateTime now_time() { return QDateTime::currentDateTime(); }

}  // namespace

MainWindow::MainWindow() : settings_("AvR0raa", "peripheral-tool") {
    setWindowTitle("外设万能调试器 " + APP_VERSION);
    resize(1000, 680);
    setStyleSheet(STYLE_SHEET);

    db_ = load_db(default_db_path());

    build_ui();
    refresh_devices();
    on_battery_all();  // 开机自动读一轮电量
    start_watcher();
    tray_ = new pt::gui::TrayController(this, this);   // 托盘常驻（M-C4 收尾）
}

void MainWindow::build_ui() {
    auto* central = new QWidget();
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(24, 20, 24, 16);
    root->setSpacing(14);

    auto* header = new QHBoxLayout();
    auto* title_box = new QVBoxLayout();
    auto* title = new QLabel("外设万能调试器");
    title->setObjectName("appTitle");
    auto* subtitle = new QLabel("自动识别你的外设 · 电量一目了然 · 罗技鼠标直接调 DPI");
    subtitle->setObjectName("appSubtitle");
    title_box->addWidget(title);
    title_box->addWidget(subtitle);
    header->addLayout(title_box);
    header->addStretch(1);

    auto* adv_btn = new QPushButton("高级模式");
    adv_btn->setObjectName("ghostBtn");
    adv_btn->setToolTip("协议开发：学习模式 / 描述符解析 / 批量探测 / 配置档案");
    connect(adv_btn, &QPushButton::clicked, this, [this] { open_advanced(); });
    auto* battery_btn = new QPushButton("读取全部电量");
    connect(battery_btn, &QPushButton::clicked, this, [this] { on_battery_all(); });
    auto* refresh_btn = new QPushButton("刷新");
    refresh_btn->setObjectName("ghostBtn");
    connect(refresh_btn, &QPushButton::clicked, this, [this] { refresh_devices(); });
    header->addWidget(adv_btn);
    header->addWidget(battery_btn);
    header->addWidget(refresh_btn);
    root->addLayout(header);

    auto* grid_host = new QWidget();
    grid_ = new QGridLayout(grid_host);
    grid_->setSpacing(16);
    grid_->setContentsMargins(4, 4, 12, 12);
    auto* scroll = new QScrollArea();
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(grid_host);
    root->addWidget(scroll, 1);
    setCentralWidget(central);
    statusBar()->showMessage("就绪 — 插拔设备会自动刷新");
}

QStringList MainWindow::hidden_list() const {
    const QVariant v = settings_.value("hidden_devices");
    QStringList out;
    if (v.typeId() == QMetaType::QStringList || v.canConvert<QStringList>())
        for (const QString& s : v.toStringList()) out << s;
    return out;
}

void MainWindow::hide_device(const QString& key) {
    QStringList hidden = hidden_list();
    if (!hidden.contains(key)) {
        hidden.append(key);
        settings_.setValue("hidden_devices", hidden);
    }
    battery_cache_.erase(key);
    refresh_devices();
}

void MainWindow::refresh_devices() {
    std::vector<DeviceInfo> devices;
    try {
        devices = list_devices();
    } catch (const HIDError& exc) {
        statusBar()->showMessage("枚举失败: " + qstr(exc.what()));
        return;
    }
    std::map<QString, std::vector<DeviceInfo>> grouped;
    std::set<QString> internal_keys;
    const QStringList hidden = hidden_list();
    for (const auto& d : devices) {
        const QString key = qstr(d.vidpid());
        if (hidden.contains(key)) continue;
        if (is_internal_device(db_, d) || d.bus_type == 3) {
            internal_keys.insert(key);
            continue;
        }
        grouped[key].push_back(d);
    }
    devices_ = std::move(grouped);
    rebuild_cards();
    if (!internal_keys.empty())
        statusBar()->showMessage(
            QString("已隐藏 %1 个笔记本/系统内部设备（非外设）").arg(internal_keys.size()));
}

void MainWindow::rebuild_cards() {
    while (grid_->count()) {
        auto* item = grid_->takeAt(0);
        if (auto* w = item->widget()) w->deleteLater();
        delete item;
    }
    if (devices_.empty()) {
        auto* empty = new QLabel("没有发现 HID 设备 — 插上外设或点「刷新」");
        empty->setStyleSheet("color:#6B7280; font-size:14px;");
        grid_->addWidget(empty, 0, 0);
        return;
    }
    int index = 0;
    for (const auto& [key, ifaces] : devices_)
        grid_->addWidget(build_card(key), index / 3, index++ % 3);
    statusBar()->showMessage(QString("%1 台设备 · 上次刷新 %2")
                                 .arg(devices_.size())
                                 .arg(now_time().toString("HH:mm:ss")));
}

QFrame* MainWindow::build_card(const QString& key) {
    const auto& ifaces = devices_.at(key);
    const DeviceInfo& first = ifaces.front();
    const auto result = fingerprint(first, db_);
    const Json entry = result.entry.is_null() ? Json::object() : result.entry;
    const auto [icon, type_name] = device_type_info(key, ifaces, entry);
    const QString product =
        first.product.empty() ? QString("(未命名)") : qstr(first.product);

    auto* card = new QFrame();
    card->setObjectName("card");
    card->setMinimumSize(280, 190);
    card->setMaximumHeight(230);
    card->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(card, &QFrame::customContextMenuRequested, card,
            [this, key, card](const QPoint&) { show_card_menu(key, card); });
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(18, 16, 18, 16);
    v->setSpacing(8);

    auto* head = new QHBoxLayout();
    auto* icon_label = new QLabel(icon);
    icon_label->setStyleSheet(
        "font-size: 26px; border: none; background: #1E2A3F;"
        "border-radius: 12px; padding: 6px 10px;");
    auto* name = new QLabel(product.left(20));
    name->setObjectName("cardTitle");
    head->addWidget(icon_label);
    head->addWidget(name, 1);
    const auto [chip_text, chip_color] = chip_for(result.status);
    auto* chip = new QLabel(chip_text);
    chip->setStyleSheet(QString("color:%1; background:%2; border:none;"
                                "border-radius:9px; padding:3px 10px; font-size:11px;"
                                "font-weight:600;")
                            .arg(chip_color, QString(chip_color) + "22"));
    head->addWidget(chip);
    v->addLayout(head);

    const QString brand = result.brand_zh ? qstr(*result.brand_zh)
                          : result.brand  ? qstr(*result.brand)
                                          : QString("未知品牌");
    const QString conn = connection_text(key, ifaces);
    auto* sub = new QLabel(QString("%1 · %2%3 · %4 接口")
                               .arg(brand, type_name,
                                    conn.isEmpty() ? QString() : " · " + conn)
                               .arg(ifaces.size()));
    sub->setObjectName("cardSub");
    v->addWidget(sub);

    const auto battery_it = battery_cache_.find(key);
    const std::optional<int> pct = battery_it != battery_cache_.end()
                                       ? battery_it->second.percent
                                       : std::nullopt;
    const std::optional<int> volt = battery_it != battery_cache_.end()
                                        ? battery_it->second.voltage_mv
                                        : std::nullopt;
    if (pct) {
        const char* color = *pct <= 20 ? "#FF5C5C" : (*pct <= 50 ? "#FFB020" : "#3DDC97");
        auto* blabel = new QLabel(QString("🔋 %1%").arg(*pct));
        blabel->setObjectName("batteryPct");
        blabel->setStyleSheet(QString("color:%1; border:none; background:transparent;").arg(color));
        v->addWidget(blabel);
        auto* bar = new QProgressBar();
        bar->setRange(0, 100);
        bar->setValue(*pct);
        bar->setTextVisible(false);
        bar->setFixedHeight(8);
        bar->setStyleSheet(QString("QProgressBar::chunk { background: %1; }").arg(color));
        v->addWidget(bar);
    } else if (volt) {
        auto* blabel = new QLabel(QString("🔋 %1 mV").arg(*volt));
        blabel->setObjectName("batteryPct");
        blabel->setStyleSheet("color:#8A94A7; border:none; background:transparent;");
        v->addWidget(blabel);
    } else if (result.brand && *result.brand == "Logitech") {
        auto* blabel =
            new QLabel("🔋 睡眠中 — 动一动鼠标后点「读取全部电量」");
        blabel->setStyleSheet(
            "color:#5A6579; font-size:12px; border:none; background:transparent;");
        blabel->setWordWrap(true);
        v->addWidget(blabel);
    } else {
        auto* blabel = new QLabel("🔋 电量未知");
        blabel->setStyleSheet("color:#5A6579; font-size:15px; font-weight:600;"
                              "border:none; background:transparent;");
        v->addWidget(blabel);
    }

    const bool has_ff00 = std::any_of(ifaces.begin(), ifaces.end(), [](const DeviceInfo& d) {
        return qhex(d.usage_page, 4) == "FF00";
    });
    if (result.brand && *result.brand == "Logitech" && has_ff00) {
        auto* dpi_row = new QHBoxLayout();
        auto* dpi_btn = new QPushButton("调节 DPI");
        connect(dpi_btn, &QPushButton::clicked, card, [this, key] { open_dpi(key); });
        dpi_row->addWidget(dpi_btn);
        dpi_row->addStretch(1);
        v->addLayout(dpi_row);
    }
    v->addStretch(1);
    return card;
}

void MainWindow::show_card_menu(const QString& key, QFrame* card) {
    QMenu menu(this);
    QAction* act = menu.addAction("隐藏此设备（不插也不占卡片）");
    QAction* chosen = menu.exec(card->mapToGlobal(card->rect().center()));
    if (chosen == act) hide_device(key);
}

void MainWindow::open_dpi(const QString& key) {
    const auto it = devices_.find(key);
    if (it == devices_.end()) return;
    const QString product = it->second.front().product.empty()
                                ? "鼠标"
                                : qstr(it->second.front().product);
    DpiDialog dlg(this, key, it->second, product);
    dlg.exec();
}

void MainWindow::open_migration() {
    MigrationDialog dlg(this);
    dlg.exec();
}

void MainWindow::open_advanced() {
    advanced_ = new AdvancedWindow();
    advanced_->show();
}

void MainWindow::on_battery_all() {
    const auto grouped = devices_;
    if (grouped.empty()) return;
    statusBar()->showMessage("电量扫描中…（罗技设备睡眠时会先唤醒）");

    // Worker 无 parent：窗口关闭时任务自然跑完、自清理（finished→deleteLater），
    // done 信号以窗口为接收者上下文 → 窗口销毁自动断连，无双析构风险
    auto* worker = new Worker(
        [grouped]() -> std::shared_ptr<void> {
            std::map<std::string, std::vector<pt::DeviceInfo>> grouped_std;
            for (const auto& [k, v] : grouped) grouped_std[sstr(k)] = v;
            const auto results = scan_all(
                grouped_std, [](const std::string& path) { return read_descriptor(path); });
            return std::make_shared<std::vector<BatteryResult>>(results);
        });
    connect(worker, &Worker::done, this, [this](const JobResult& result) {
        if (!result.ok) {
            statusBar()->showMessage("电量扫描出错: " + result.error);
            return;
        }
        const auto& value = *static_cast<std::vector<BatteryResult>*>(result.data.get());
        battery_cache_.clear();
        for (const auto& r : value) battery_cache_[qstr(r.vidpid)] = r;
        std::size_t known = 0;
        for (const auto& r : value) {
            const QString key = qstr(r.vidpid);
            if (r.percent) {
                ++known;
                // 低电量提醒（≤20% 跨过阈值提醒一次；回升到 >30% 重置）
                if (tray_) {
                    const QString name = qstr(r.product).isEmpty()
                                             ? key : qstr(r.product);
                    if (*r.percent <= 20 && !low_alerted_.contains(key)) {
                        low_alerted_.append(key);
                        tray_->show_low_battery(name, *r.percent);
                    } else if (*r.percent > 30) {
                        low_alerted_.removeAll(key);
                    }
                }
            }
        }
        statusBar()->showMessage(
            QString("电量扫描完成 — %1/%2 台有读数").arg(known).arg(value.size()));
        rebuild_cards();
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MainWindow::start_watcher() {
    watcher_ = new HotplugWatcher(this);
    connect(watcher_, &HotplugWatcher::eventHappened, this,
            [this](const DeviceEvent& event) { on_hotplug(event); });
    watcher_->start();
}

void MainWindow::on_hotplug(const DeviceEvent& event) {
    const auto& d = event.device;
    const QString action = event.kind == "added" ? "接入" : "移除";
    statusBar()->showMessage(QString("设备%1: %2:%3 %4")
                                 .arg(action, qstr(d.vid_hex()), qstr(d.pid_hex()),
                                      qstr(d.product)));
    refresh_devices();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    settings_.setValue("window/geometry", saveGeometry());
    // 关窗 = 最小化到托盘（托盘菜单「退出」才真正退出）
    if (tray_ && QSystemTrayIcon::isSystemTrayAvailable() &&
        settings_.value("close_to_tray", true).toBool()) {
        hide();
        event->ignore();
        if (!settings_.value("tray_hint_shown", false).toBool()) {
            tray_->show_running_hint();
            settings_.setValue("tray_hint_shown", true);
        }
        return;
    }
    if (watcher_) {
        watcher_->stop_watching();
        watcher_->wait(2000);
    }
    QMainWindow::closeEvent(event);
}

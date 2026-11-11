// tray.cpp — 系统托盘实现（电量环图标绘制 / 菜单重建 / 定时扫描）。
#include "tray.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QFont>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QTimer>

#include <optional>

#include "core/device_manager.hpp"
#include "main_window.hpp"

namespace pt::gui {

namespace {

QString battery_text(const std::optional<int>& percent) {
    if (!percent) return "电量 —";
    return QString("🔋 %1%").arg(*percent);
}

}  // namespace

QIcon battery_ring_icon(const std::optional<int>& percent) {
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    // 底环
    QPen bg(QColor("#3A4557"), 6);
    p.setPen(bg);
    p.drawEllipse(8, 8, 48, 48);

    if (percent) {
        const QColor color = *percent <= 20 ? QColor("#FF5C5C")
                            : *percent <= 50 ? QColor("#FFB020")
                                             : QColor("#3DDC97");
        QPen fg(color, 6, Qt::SolidLine, Qt::RoundCap);
        p.setPen(fg);
        // 从 12 点方向顺时针画 percent 比例的弧
        const int span = *percent * 360 * 16 / 100;
        p.drawArc(8, 8, 48, 48, 90 * 16, -span);
        // 中心百分比数字
        p.setPen(QColor("#F0F4FA"));
        QFont f = p.font();
        f.setPixelSize(20);
        f.setBold(true);
        p.setFont(f);
        p.drawText(pm.rect(), Qt::AlignCenter, QString::number(*percent));
    } else {
        p.setPen(QColor("#8A94A7"));
        QFont f = p.font();
        f.setPixelSize(26);
        f.setBold(true);
        p.setFont(f);
        p.drawText(pm.rect(), Qt::AlignCenter, "?");
    }
    return QIcon(pm);
}

QString tray_tooltip_for(MainWindow* window) {
    if (!window) return "外设万能调试器";
    QString body;
    for (const auto& [key, ifaces] : window->devices()) {
        const auto it = window->battery_cache().find(key);
        const std::optional<int> pct = it != window->battery_cache().end()
                                           ? it->second.percent
                                           : std::nullopt;
        QString name = ifaces.empty() ? key : (ifaces[0].product.empty()
                                                  ? key : QString::fromStdString(ifaces[0].product));
        body += QString("\n%1  %2").arg(name, battery_text(pct));
    }
    return "外设万能调试器" + body;
}

TrayController::TrayController(MainWindow* window, QObject* parent)
    : QObject(parent), window_(window) {
    tray_.setToolTip(tray_tooltip_for(window_));
    tray_.setIcon(battery_ring_icon(std::nullopt));
    rebuild_menu();

    // 左键：切换主面板显隐
    connect(&tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason != QSystemTrayIcon::Trigger) return;
                MainWindow* w = window_;
                if (w->isVisible() && !w->isMinimized()) {
                    w->hide();
                } else {
                    w->showNormal();
                    w->activateWindow();
                }
            });

    // 托盘菜单打开前重建（设备/电量始终最新）
    connect(menu_.get(), &QMenu::aboutToShow, this, [this] { rebuild_menu(); });

    // 周期电量扫描（默认 90s；EZ2.4G 同款常驻节奏，仅托盘在时开）
    scan_timer_ = new QTimer(this);
    scan_timer_->setInterval(90 * 1000);
    connect(scan_timer_, &QTimer::timeout, this, [this] {
        if (QSystemTrayIcon::isSystemTrayAvailable()) scan_once();
    });
    scan_timer_->start();

    // 托盘图标/提示轻刷新（读缓存，1s）
    ui_timer_ = new QTimer(this);
    ui_timer_->setInterval(1000);
    connect(ui_timer_, &QTimer::timeout, this, [this] { update_icon(); });
    ui_timer_->start();

    tray_.show();
}

TrayController::~TrayController() = default;

void TrayController::rebuild_menu() {
    menu_ = std::make_unique<QMenu>();

    QAction* toggle = menu_->addAction("显示 / 隐藏面板");
    connect(toggle, &QAction::triggered, this, [this] {
        MainWindow* w = window_;
        if (w->isVisible() && !w->isMinimized()) {
            w->hide();
        } else {
            w->showNormal();
            w->activateWindow();
        }
    });
    menu_->addSeparator();

    // 设备 + 电量段（信息展示，不可点）
    for (const auto& [key, ifaces] : window_->devices()) {
        const auto it = window_->battery_cache().find(key);
        const std::optional<int> pct = it != window_->battery_cache().end()
                                           ? it->second.percent
                                           : std::nullopt;
        QString name = ifaces.empty() ? key : (ifaces[0].product.empty()
                                                  ? key : QString::fromStdString(ifaces[0].product));
        QAction* info = menu_->addAction(QString("%1  %2").arg(name, battery_text(pct)));
        info->setEnabled(false);
        info->setIcon(battery_ring_icon(pct));
    }
    if (!window_->devices().empty()) menu_->addSeparator();

    QAction* scan = menu_->addAction("读取全部电量");
    connect(scan, &QAction::triggered, this, [this] { scan_once(); });
    QAction* refresh = menu_->addAction("刷新设备");
    connect(refresh, &QAction::triggered, this, [this] { window_->refresh_devices(); });
    menu_->addSeparator();

    QAction* quit = menu_->addAction("退出");
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);

    tray_.setContextMenu(menu_.get());
    update_icon();
}

void TrayController::update_icon() {
    // 多设备时图标显示第一台有读数的电量；都没有则问号环
    std::optional<int> shown;
    for (const auto& [key, r] : window_->battery_cache()) {
        if (r.percent) {
            shown = r.percent;
            break;
        }
    }
    tray_.setIcon(battery_ring_icon(shown));
    tray_.setToolTip(tray_tooltip_for(window_));
}

void TrayController::scan_once() {
    if (window_) window_->on_battery_all();
}

}  // namespace pt::gui

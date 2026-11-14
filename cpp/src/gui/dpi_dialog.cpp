// dpi_dialog.cpp — DPI 调节弹窗实现（gui.py DpiDialog 平移）。
#include "dpi_dialog.hpp"

#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <stdexcept>

#include "core/dpi.hpp"
#include "core/onboard.hpp"
#include "gui_helpers.hpp"

using namespace pt;
using namespace pt::gui;

namespace {

std::unique_ptr<HidppLink> build_link_for(const std::vector<DeviceInfo>& ifaces) {
    const DeviceInfo* short_if = nullptr;
    const DeviceInfo* long_if = nullptr;
    for (const auto& d : ifaces) {
        if (qhex(d.usage_page, 4) == "FF00" && qhex(d.usage, 4) == "0001") short_if = &d;
        if (qhex(d.usage_page, 4) == "FF00" && qhex(d.usage, 4) == "0002") long_if = &d;
    }
    if (!short_if && !long_if) return nullptr;
    return std::make_unique<HidppLink>(short_if ? short_if->path : std::string(),
                                       long_if ? long_if->path : std::string());
}

// ---- Worker 载荷类型 ----

struct OnboardPayload {
    pt::OnboardInfo info;
};

struct DpiReadPayload {
    DpiRead dpi;
    std::vector<int> lst;
    std::optional<int> current_rate;
    std::vector<int> rates;
    pt::OnboardInfo onboard;
};

struct TextPayload {
    QString text;
};

std::shared_ptr<void> make_text(const QString& s) {
    return std::make_shared<TextPayload>(TextPayload{s});
}

}  // namespace

DpiDialog::DpiDialog(QWidget* parent, const QString& key, std::vector<DeviceInfo> ifaces,
                     const QString& product)
    : QDialog(parent), ifaces_(std::move(ifaces)) {
    (void)key;
    setWindowTitle("调节鼠标 DPI — " + product);
    setStyleSheet(STYLE_SHEET);
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(14);
    status_ = new QLabel("连接中…（设备睡眠时请动一动鼠标，HID++ 流量会唤醒它）");
    status_->setWordWrap(true);
    layout->addWidget(status_);

    auto* row = new QHBoxLayout();
    spin_ = new QSpinBox();
    spin_->setRange(50, 32000);
    spin_->setSingleStep(50);
    spin_->setValue(800);
    slider_ = new QSlider(Qt::Horizontal);
    slider_->setRange(50, 32000);
    slider_->setValue(800);
    connect(spin_, &QSpinBox::valueChanged, slider_, &QSlider::setValue);
    connect(slider_, &QSlider::valueChanged, spin_, &QSpinBox::setValue);
    row->addWidget(slider_, 1);
    row->addWidget(spin_);
    layout->addLayout(row);

    info_ = new QLabel("档位表：读取中…");
    info_->setStyleSheet("color:#6B7280; font-size:12px;");
    layout->addWidget(info_);

    auto* rate_row = new QHBoxLayout();
    auto* rate_label = new QLabel("报告率：");
    rate_combo_ = new QComboBox();
    rate_combo_->addItem("—", QVariant());
    auto* rate_apply = new QPushButton("应用报告率");
    rate_apply->setObjectName("ghostBtn");
    connect(rate_apply, &QPushButton::clicked, this, [this] { apply_rate(); });
    rate_row->addWidget(rate_label);
    rate_row->addWidget(rate_combo_, 1);
    rate_row->addWidget(rate_apply);
    layout->addLayout(rate_row);

    auto* btns = new QHBoxLayout();
    auto* apply_btn = new QPushButton("应用 DPI");
    connect(apply_btn, &QPushButton::clicked, this, [this] { apply(); });
    auto* close_btn = new QPushButton("关闭");
    close_btn->setObjectName("ghostBtn");
    connect(close_btn, &QPushButton::clicked, this, &QDialog::reject);
    btns->addWidget(apply_btn, 1);
    btns->addWidget(close_btn);
    layout->addLayout(btns);

    read_current();
}

int DpiDialog::slot() const {
    // 接收器配对设备走槽位 1；无长通道的直连设备用 0xFF
    for (const auto& d : ifaces_)
        if (qhex(d.usage_page, 4) == "FF00" && qhex(d.usage, 4) == "0002") return 1;
    return 0xFF;
}

void DpiDialog::track(Worker* worker) {
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
}

void DpiDialog::read_current() {
    const int slot_no = slot();

    auto* worker = new Worker(
        [this, slot_no]() -> std::shared_ptr<void> {
            auto link = build_link_for(ifaces_);
            if (!link) return nullptr;  // "未找到 HID++ 接口"
            link->open();
            struct LinkCloser {
                HidppLink& l;
                ~LinkCloser() { l.close(); }
            } closer{*link};
            if (!ensure_online(*link, slot_no, 10))
                throw std::runtime_error("设备不在线（请动一动鼠标再试）");
            auto r = std::make_shared<DpiReadPayload>();
            r->dpi = read_dpi(*link, slot_no);
            r->lst = get_dpi_list(*link, slot_no);
            r->current_rate = read_poll_rate(*link, slot_no);
            r->rates = get_poll_rates(*link, slot_no);
            r->onboard = read_onboard_info(*link, slot_no);
            return r;
        });
    connect(worker, &Worker::done, this, [this](const JobResult& result) {
        if (!result.ok || !result.data) {
            status_->setText(result.ok ? "未找到 HID++ 接口" : "出错: " + result.error);
            return;
        }
        const auto& r = *static_cast<DpiReadPayload*>(result.data.get());
        if (!r.dpi.x) {
            status_->setText("读取失败（设备可能睡眠中，动一动鼠标再试）");
            return;
        }
        spin_->setValue(*r.dpi.x);
        if (!r.lst.empty()) {
            slider_->setRange(r.lst.front(), r.lst.back());
            spin_->setRange(r.lst.front(), r.lst.back());
            info_->setText(QString("支持 %1 档（%2–%3）")
                               .arg(r.lst.size())
                               .arg(r.lst.front())
                               .arg(r.lst.back()));
        }
        rate_combo_->clear();
        if (!r.rates.empty()) {
            for (int hz : r.rates) rate_combo_->addItem(QString("%1 Hz").arg(hz), hz);
            lod_combo_->setCurrentIndex(0);       // 默认保持当前
        profile_combo_->clear();
        if (r.onboard.supported()) {
            profile_combo_->addItem(QString("Host 模式（当前 %1）")
                                        .arg(QString::fromStdString(r.onboard.mode)),
                                    -2);
            profile_combo_->addItem("关闭板载（Host）", -1);
            int n = 1;
            for (const auto& h : r.onboard.headers) {
                profile_combo_->addItem(QString("配置檔 #%1（sector %2）")
                                            .arg(n++).arg(h.sector),
                                        h.sector);
            }
        } else {
            profile_combo_->addItem("设备不支持板载配置档", -1);
        }
        if (r.current_rate && std::find(r.rates.begin(), r.rates.end(), *r.current_rate)
                                      != r.rates.end())
                rate_combo_->setCurrentText(QString("%1 Hz").arg(*r.current_rate));
        } else {
            rate_combo_->addItem("设备不支持", QVariant());
        }
        QString extra;
        if (r.dpi.y) extra += QString(" · Y轴 %1").arg(*r.dpi.y);
        if (r.dpi.lod) extra += QString(" · LOD %1").arg(*r.dpi.lod);
        if (r.current_rate) extra += QString(" · 报告率 %1Hz").arg(*r.current_rate);
        status_->setText(QString("✅ 当前 %1 DPI%2").arg(*r.dpi.x).arg(extra));
    });
    track(worker);
    worker->start();
}

void DpiDialog::apply_rate() {
    const QVariant hz = rate_combo_->currentData();
    if (!hz.isValid()) {
        status_->setText("设备不支持报告率调节");
        return;
    }
    const int slot_no = slot();
    status_->setText(QString("应用报告率 %1Hz 中…（读回验证）").arg(hz.toInt()));

    auto* worker = new Worker(
        [this, slot_no, hz]() -> std::shared_ptr<void> {
            auto link = build_link_for(ifaces_);
            if (!link) return make_text("未找到 HID++ 接口");
            link->open();
            struct LinkCloser {
                HidppLink& l;
                ~LinkCloser() { l.close(); }
            } closer{*link};
            if (!ensure_online(*link, slot_no, 6))
                return make_text("设备不在线（请动一动鼠标再试）");
            const auto [ok, msg] = pt::set_poll_rate(*link, slot_no, hz.toInt());
            return make_text((ok ? "✅ " : "❌ ") + qstr(msg));
        });
    connect(worker, &Worker::done, this, [this](const JobResult& result) {
        status_->setText(!result.ok ? "出错: " + result.error
                                    : static_cast<TextPayload*>(result.data.get())->text);
    });
    track(worker);
    worker->start();
}

void DpiDialog::apply_profile() {
    const int data = profile_combo_ ? profile_combo_->currentData().toInt() : -1;
    if (data == -1) {
        status_->setText("设备不支持板载配置档");
        return;
    }
    const int slot_no = slot();
    status_->setText("切换配置档中…");

    auto* worker = new Worker(
        [this, slot_no, data]() -> std::shared_ptr<void> {
            auto link = build_link_for(ifaces_);
            if (!link) throw std::runtime_error("未找到 HID++ 接口");
            link->open();
            struct LinkCloser {
                HidppLink& l;
                ~LinkCloser() { l.close(); }
            } closer{*link};
            if (!ensure_online(*link, slot_no, 6))
                throw std::runtime_error("设备不在线（请动一动鼠标再试）");
            auto* payload = new std::pair<bool, std::string>(
                data == -2 ? pt::set_onboard_mode(*link, slot_no, "host")
                           : pt::set_active_profile(*link, slot_no, data));
            return std::shared_ptr<void>(payload);
        });

    connect(worker, &Worker::done, this, [this](const JobResult& result) {
        if (!result.ok) {
            status_->setText("出错: " + result.error);
            return;
        }
        const auto& [ok, msg] = *static_cast<std::pair<bool, std::string>*>(
            result.data.get());
        status_->setText(QString("%1 %2").arg(ok ? "✅" : "❌",
                                              QString::fromStdString(msg)));
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void DpiDialog::apply() {
    const int slot_no = slot();
    const int target = spin_->value();
    status_->setText(QString("应用 %1 DPI 中…（读旧→写→读回验证）").arg(target));

    auto* worker = new Worker(
        [this, slot_no, target]() -> std::shared_ptr<void> {
            auto link = build_link_for(ifaces_);
            if (!link) return make_text("未找到 HID++ 接口");
            link->open();
            struct LinkCloser {
                HidppLink& l;
                ~LinkCloser() { l.close(); }
            } closer{*link};
            if (!ensure_online(*link, slot_no, 6))
                return make_text("设备不在线（请动一动鼠标再试）");
            std::optional<int> lod = std::nullopt;
            if (lod_combo_ && lod_combo_->currentData().toInt() >= 0) {
                lod = lod_combo_->currentData().toInt();
            }
            const auto outcome =
                pt::set_dpi(*link, slot_no, target, 600, lod);
            QStringList lines;
            for (const auto& l : outcome.log) lines << qstr(l);
            return make_text(lines.join("\n"));
        });
    connect(worker, &Worker::done, this, [this, target](const JobResult& result) {
        if (!result.ok) {
            status_->setText("出错: " + result.error);
            return;
        }
        const QString text = static_cast<TextPayload*>(result.data.get())->text;
        if (text.contains("验证: 通过"))
            status_->setText(QString("✅ DPI 已设为 %1").arg(target));
        else
            status_->setText(text);
    });
    track(worker);
    worker->start();
}

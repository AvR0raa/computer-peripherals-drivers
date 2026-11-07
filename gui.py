"""gui.py — All-in-One 外设调试工具（M3，PySide6）。

布局（ARCHITECTURE.md §7）：
  左：设备列表（按状态着色，热插拔实时刷新）
  右：详情页（设备信息 / 接口表 / 描述符 / 协议功能动态控件）
  下：hex 收发日志

运行: python gui.py
"""
from __future__ import annotations

import sys
import threading
import time

from PySide6.QtCore import QSettings, Qt, QThread, QTimer, Signal
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QDialog,
    QFormLayout,
    QFrame,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QScrollArea,
    QSpinBox,
    QSlider,
    QSplitter,
    QTabWidget,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

import core.device_manager as dm
from core.battery import BatteryResult, scan_all
from core.descriptor import parse_report_descriptor, summary
from core.dpi import get_dpi_list, read_dpi, set_dpi
from core.diffing import diff_sessions, format_diff
from core.fingerprint import (
    DEFAULT_DB_PATH,
    DeviceStatus,
    fingerprint,
    load_db,
    save_db,
)
from core.probe import (
    feature_report_ids,
    probe_vendor_interfaces,
    suspect_rawhid_interfaces,
)
from core.profiles import (
    DEFAULT_PROFILE_DIR,
    DeviceProfile,
    ProfileFeature,
    list_profiles,
    load_profile,
    save_profile,
)
from core.hidpp import HidppLink, ensure_online
from core.recorder import Recorder, RecorderTarget
from core.report_io import ReportIO, execute_feature
from core.safe_write import safe_write
from core.wizard import build_drafts, build_protocol_block, merge_into_db

APP_VERSION = "v0.6.2"

STATUS_COLOR = {
    DeviceStatus.KNOWN: QColor("#2ecc71"),
    DeviceStatus.SDK: QColor("#3498db"),
    DeviceStatus.UNKNOWN: QColor("#95a5a6"),
}
STATUS_TEXT = {
    DeviceStatus.KNOWN: "OK 已知(可直通)",
    DeviceStatus.SDK: "SDK 有官方SDK",
    DeviceStatus.UNKNOWN: "??? 未知(可学习)",
}
STATUS_ICON = {
    DeviceStatus.KNOWN: "✅",
    DeviceStatus.SDK: "🔌",
    DeviceStatus.UNKNOWN: "❓",
}


# ---------------------------------------------------------------------------
# 工作线程
# ---------------------------------------------------------------------------

class HotplugWatcher(QThread):
    """轮询差分热插拔（任务 1.1 的 GUI 版）。"""

    event = Signal(object)          # DeviceEvent

    def __init__(self, parent=None):
        super().__init__(parent)
        self._stop = threading.Event()

    def run(self):
        try:
            dm.watch_devices(self.event.emit, interval=1.0, stop=self._stop)
        except Exception:
            pass  # 退出阶段的枚举异常忽略

    def stop_watching(self):
        self._stop.set()


class Worker(QThread):
    """一次性工作线程：fn() 在后台跑，结果/异常经信号回 GUI 线程。"""

    done = Signal(object)           # ("ok", result) | ("err", text)

    def __init__(self, fn, parent=None):
        super().__init__(parent)
        self._fn = fn

    def run(self):
        try:
            self.done.emit(("ok", self._fn()))
        except Exception as exc:
            self.done.emit(("err", f"{type(exc).__name__}: {exc}"))


# ---------------------------------------------------------------------------
# 主窗口
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 用户客户端（v0.6.1 重设计）：单页设备卡片墙，专注 电量 + DPI
# ---------------------------------------------------------------------------

STYLE_SHEET = """
QMainWindow, QDialog { background: #0D1117; }
#appTitle { font-size: 22px; font-weight: 800; color: #F0F4FA; }
#appSubtitle { font-size: 12px; color: #8A94A7; }
#card { background: #161B27; border: 1px solid #232B3B; border-radius: 16px; }
#card:hover { border: 1px solid #4F8CFF; }
#cardTitle { font-size: 16px; font-weight: 700; color: #F0F4FA; }
#cardSub { font-size: 12px; color: #8A94A7; }
#batteryPct { font-size: 24px; font-weight: 800; }
QProgressBar { background: #232B3B; border: none; border-radius: 4px; }
QPushButton { background: qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #4F8CFF, stop:1 #6C5CE7);
              color: white; border: none; border-radius: 9px; padding: 8px 16px;
              font-size: 13px; font-weight: 600; }
QPushButton:hover { background: #5B97FF; }
QPushButton:disabled { background: #2A3547; color: #5A6579; }
QPushButton#ghostBtn { background: #1C2333; color: #C9D2E3; border: 1px solid #2A3547; }
QPushButton#ghostBtn:hover { background: #232B3B; }
QSlider::groove:horizontal { height: 6px; background: #232B3B; border-radius: 3px; }
QSlider::handle:horizontal { width: 18px; margin: -7px 0; border-radius: 9px; background: #4F8CFF; }
QSlider::sub-page:horizontal { background: #4F8CFF66; border-radius: 3px; }
QStatusBar { background: transparent; color: #8A94A7; }
QScrollArea { background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }
QToolTip { background: #1C2333; color: #F0F4FA; border: 1px solid #2A3547; }
"""

_TYPE_ICONS = {"鼠标": "\U0001F5B1\uFE0F", "键盘": "\u2328\uFE0F", "耳机": "\U0001F3A7",
               "接收器": "\U0001F4E1", "音频设备": "\U0001F3A7", "外设": "\U0001F50C"}


def _type_info(key: str, ifaces: list, entry: dict | None) -> tuple[str, str]:
    """(emoji, 类型名)：协议库 type 字段优先，否则按 usage 启发式。"""
    t = (entry or {}).get("type")
    if t in _TYPE_ICONS:
        return _TYPE_ICONS[t], t
    usages = {d.usage for d in ifaces}
    if 0x0002 in usages:
        return _TYPE_ICONS["鼠标"], "鼠标"
    if 0x0006 in usages:
        return _TYPE_ICONS["键盘"], "键盘"
    if 0x000C in usages or 0x000B in usages:
        return _TYPE_ICONS["耳机"], "音频设备"
    return _TYPE_ICONS["外设"], "外设"


class DpiDialog(QDialog):
    """简洁 DPI 调节弹窗：读取 → 滑条 → 应用（安全写）。"""

    def __init__(self, parent, key: str, ifaces: list, product: str):
        super().__init__(parent)
        self.setWindowTitle(f"调节鼠标 DPI — {product}")
        self.setStyleSheet(STYLE_SHEET)
        self.setMinimumWidth(420)
        self._ifaces = ifaces

        layout = QVBoxLayout(self)
        layout.setSpacing(14)
        self._status = QLabel("连接中…（设备睡眠时请动一动鼠标，HID++ 流量会唤醒它）")
        self._status.setWordWrap(True)
        layout.addWidget(self._status)

        row = QHBoxLayout()
        self._spin = QSpinBox()
        self._spin.setRange(50, 32000)
        self._spin.setSingleStep(50)
        self._spin.setValue(800)
        self._slider = QSlider(Qt.Horizontal)
        self._slider.setRange(50, 32000)
        self._slider.setValue(800)
        self._spin.valueChanged.connect(self._slider.setValue)
        self._slider.valueChanged.connect(self._spin.setValue)
        row.addWidget(self._slider, 1)
        row.addWidget(self._spin)
        layout.addLayout(row)

        self._info = QLabel("档位表：读取中…")
        self._info.setStyleSheet("color:#6B7280; font-size:12px;")
        layout.addWidget(self._info)

        btns = QHBoxLayout()
        apply_btn = QPushButton("应用 DPI")
        apply_btn.clicked.connect(self._apply)
        close_btn = QPushButton("关闭")
        close_btn.setObjectName("ghostBtn")
        close_btn.clicked.connect(self.reject)
        btns.addWidget(apply_btn, 1)
        btns.addWidget(close_btn)
        layout.addLayout(btns)

        self._read()

    def _build_link(self):
        short = next((d for d in self._ifaces
                      if f"{d.usage_page:04X}" == "FF00" and f"{d.usage:04X}" == "0001"), None)
        long_if = next((d for d in self._ifaces
                        if f"{d.usage_page:04X}" == "FF00" and f"{d.usage:04X}" == "0002"), None)
        if short is None and long_if is None:
            return None
        return HidppLink(short.path if short else None, long_if.path if long_if else None)

    def _slot(self) -> int:
        """接收器配对设备走槽位 1；无长通道的直连设备用 0xFF。"""
        has_long = any(f"{d.usage_page:04X}" == "FF00" and f"{d.usage:04X}" == "0002"
                       for d in self._ifaces)
        return 1 if has_long else 0xFF

    def _track(self, worker: Worker):
        parent = self.parent()
        if hasattr(parent, "_workers"):
            parent._workers.append(worker)

    def _read(self):
        slot = self._slot()

        def job():
            link = self._build_link()
            if link is None:
                return None, "未找到 HID++ 接口"
            with link:
                if ensure_online(link, slot, attempts=10) is None:
                    return None, "设备不在线（请动一动鼠标再试）"
                dpi, dpi_y, lod, default = read_dpi(link, slot)
                lst = get_dpi_list(link, slot)
            return (dpi, dpi_y, lod, default, lst), None

        worker = Worker(job, self)

        def done(payload):
            status, value = payload
            if status != "ok" or value[1]:
                self._status.setText(value if status != "ok" else value[1])
                return
            dpi, dpi_y, lod, default, lst = value[0]
            if dpi is None:
                self._status.setText("读取失败（设备可能睡眠中，动一动鼠标再试）")
                return
            self._spin.setValue(dpi)
            if lst:
                self._slider.setRange(lst[0], lst[-1])
                self._spin.setRange(lst[0], lst[-1])
                self._info.setText(f"支持 {len(lst)} 档（{lst[0]}–{lst[-1]}）")
            extra = f" · Y轴 {dpi_y}" if dpi_y else ""
            extra += f" · LOD {lod}" if lod is not None else ""
            self._status.setText(f"✅ 当前 {dpi} DPI{extra}")

        worker.done.connect(done)
        self._track(worker)
        worker.start()

    def _apply(self):
        slot = self._slot()
        target = self._spin.value()
        self._status.setText(f"应用 {target} DPI 中…（读旧→写→读回验证）")

        def job():
            link = self._build_link()
            if link is None:
                return "未找到 HID++ 接口"
            with link:
                if ensure_online(link, slot, attempts=6) is None:
                    return "设备不在线（请动一动鼠标再试）"
                outcome = set_dpi(link, slot, target)
            return "\n".join(outcome.log)

        worker = Worker(job, self)

        def done(payload):
            status, value = payload
            text = value if status == "ok" else f"出错: {value}"
            if "验证: 通过" in text:
                self._status.setText(f"✅ DPI 已设为 {target}")
            else:
                self._status.setText(text)

        worker.done.connect(done)
        self._track(worker)
        worker.start()


class MainWindow(QMainWindow):
    """用户客户端主窗口：单页卡片墙（电量 + DPI），高级功能收进「高级模式」。"""

    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"外设万能调试器 {APP_VERSION}")
        self.resize(1000, 680)
        self.setStyleSheet(STYLE_SHEET)

        self.db = load_db(DEFAULT_DB_PATH)
        self._devices: dict[str, list[dm.DeviceInfo]] = {}
        self._battery_cache: dict[str, BatteryResult] = {}
        self._workers: list[Worker] = []
        self._watcher: HotplugWatcher | None = None
        self._advanced: AdvancedWindow | None = None
        self._settings = QSettings("AvR0raa", "peripheral-tool")

        self._build_ui()
        self.refresh_devices()
        self._on_battery_all()          # 开机自动读一轮电量
        self._start_watcher()

    def _build_ui(self):
        central = QWidget()
        root = QVBoxLayout(central)
        root.setContentsMargins(24, 20, 24, 16)
        root.setSpacing(14)

        header = QHBoxLayout()
        title_box = QVBoxLayout()
        title = QLabel("外设万能调试器")
        title.setObjectName("appTitle")
        subtitle = QLabel("自动识别你的外设 · 电量一目了然 · 罗技鼠标直接调 DPI")
        subtitle.setObjectName("appSubtitle")
        title_box.addWidget(title)
        title_box.addWidget(subtitle)
        header.addLayout(title_box)
        header.addStretch(1)

        adv_btn = QPushButton("高级模式")
        adv_btn.setObjectName("ghostBtn")
        adv_btn.setToolTip("协议开发：学习模式 / 描述符解析 / 批量探测 / 配置档案")
        adv_btn.clicked.connect(self._open_advanced)
        battery_btn = QPushButton("读取全部电量")
        battery_btn.clicked.connect(self._on_battery_all)
        refresh_btn = QPushButton("刷新")
        refresh_btn.setObjectName("ghostBtn")
        refresh_btn.clicked.connect(self.refresh_devices)
        header.addWidget(adv_btn)
        header.addWidget(battery_btn)
        header.addWidget(refresh_btn)
        root.addLayout(header)

        self._grid_host = QWidget()
        self._grid = QGridLayout(self._grid_host)
        self._grid.setSpacing(16)
        self._grid.setContentsMargins(4, 4, 12, 12)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QScrollArea.NoFrame)
        scroll.setWidget(self._grid_host)
        root.addWidget(scroll, 1)
        self.setCentralWidget(central)
        self.statusBar().showMessage("就绪 — 插拔设备会自动刷新")

    # ---- 设备卡片 ----

    def refresh_devices(self):
        try:
            devices = dm.list_devices()
        except dm.HIDError as exc:
            self.statusBar().showMessage(f"枚举失败: {exc}")
            return
        grouped: dict[str, list[dm.DeviceInfo]] = {}
        for d in devices:
            grouped.setdefault(f"{d.vid_hex}:{d.pid_hex}", []).append(d)
        self._devices = grouped
        self._rebuild_cards()

    def _rebuild_cards(self):
        while self._grid.count():
            item = self._grid.takeAt(0)
            w = item.widget()
            if w is not None:
                w.deleteLater()
        if not self._devices:
            empty = QLabel("没有发现 HID 设备 — 插上外设或点「刷新」")
            empty.setStyleSheet("color:#6B7280; font-size:14px;")
            self._grid.addWidget(empty, 0, 0)
            return
        for index, key in enumerate(sorted(self._devices)):
            self._grid.addWidget(self._build_card(key), index // 3, index % 3)
        self.statusBar().showMessage(
            f"{len(self._devices)} 台设备 · 上次刷新 {time.strftime('%H:%M:%S')}")

    def _build_card(self, key: str) -> QFrame:
        ifaces = self._devices[key]
        first = ifaces[0]
        result = fingerprint(first, self.db)
        entry = result.entry or {}
        icon, type_name = _type_info(key, ifaces, entry)
        product = first.product or "(未命名)"

        card = QFrame()
        card.setObjectName("card")
        card.setMinimumSize(280, 190)
        card.setMaximumHeight(230)
        v = QVBoxLayout(card)
        v.setContentsMargins(18, 16, 18, 16)
        v.setSpacing(8)

        head = QHBoxLayout()
        icon_label = QLabel(icon)
        icon_label.setStyleSheet(
            "font-size: 26px; border: none; background: #1E2A3F;"
            "border-radius: 12px; padding: 6px 10px;")
        name = QLabel(product[:20])
        name.setObjectName("cardTitle")
        head.addWidget(icon_label)
        head.addWidget(name, 1)
        chip_text, chip_color = {
            DeviceStatus.KNOWN: ("已识别 · 可控制", "#3DDC97"),
            DeviceStatus.SDK: ("官方 SDK 可用", "#4F8CFF"),
            DeviceStatus.UNKNOWN: ("未识别", "#8A94A7"),
        }[result.status]
        chip = QLabel(chip_text)
        chip.setStyleSheet(
            f"color:{chip_color}; background:{chip_color}22; border:none;"
            "border-radius:9px; padding:3px 10px; font-size:11px; font-weight:600;")
        head.addWidget(chip)
        v.addLayout(head)

        brand = result.brand_zh or result.brand or "未知品牌"
        sub = QLabel(f"{brand} · {type_name} · {len(ifaces)} 接口")
        sub.setObjectName("cardSub")
        v.addWidget(sub)

        battery = self._battery_cache.get(key)
        pct = battery.percent if battery else None
        volt = battery.voltage_mv if battery else None
        if pct is not None:
            color = "#FF5C5C" if pct <= 20 else "#FFB020" if pct <= 50 else "#3DDC97"
            blabel = QLabel(f"🔋 {pct}%")
            blabel.setObjectName("batteryPct")
            blabel.setStyleSheet(f"color:{color}; border:none; background:transparent;")
            v.addWidget(blabel)
            bar = QProgressBar()
            bar.setRange(0, 100)
            bar.setValue(pct)
            bar.setTextVisible(False)
            bar.setFixedHeight(8)
            bar.setStyleSheet(f"QProgressBar::chunk {{ background: {color}; }}")
            v.addWidget(bar)
        elif volt is not None:
            blabel = QLabel(f"🔋 {volt} mV")
            blabel.setObjectName("batteryPct")
            blabel.setStyleSheet("color:#8A94A7; border:none; background:transparent;")
            v.addWidget(blabel)
        elif result.brand == "Logitech":
            blabel = QLabel("🔋 睡眠中 — 动一动鼠标后点「读取全部电量」")
            blabel.setStyleSheet("color:#5A6579; font-size:12px; border:none; background:transparent;")
            blabel.setWordWrap(True)
            v.addWidget(blabel)
        else:
            blabel = QLabel("🔋 电量未知")
            blabel.setStyleSheet("color:#5A6579; font-size:15px; font-weight:600; border:none; background:transparent;")
            v.addWidget(blabel)

        if result.brand == "Logitech" and any(
                f"{d.usage_page:04X}" == "FF00" for d in ifaces):
            dpi_row = QHBoxLayout()
            dpi_btn = QPushButton("调节 DPI")
            dpi_btn.clicked.connect(lambda _, k=key: self._open_dpi(k))
            dpi_row.addWidget(dpi_btn)
            dpi_row.addStretch(1)
            v.addLayout(dpi_row)
        v.addStretch(1)
        return card

    # ---- 动作 ----

    def _open_dpi(self, key: str):
        ifaces = self._devices.get(key, [])
        dlg = DpiDialog(self, key, ifaces, ifaces[0].product or "鼠标")
        dlg.exec()

    def _open_advanced(self):
        self._advanced = AdvancedWindow()
        self._advanced.show()

    def _on_battery_all(self):
        grouped = dict(self._devices)
        if not grouped:
            return
        self.statusBar().showMessage("电量扫描中…（罗技设备睡眠时会先唤醒）")

        def read_descriptor(path):
            import hid
            try:
                dev = hid.device()
                dev.open_path(path.encode("utf-8") if isinstance(path, str) else path)
                try:
                    return bytes(dev.get_report_descriptor())
                finally:
                    dev.close()
            except Exception:
                return None

        def job():
            return scan_all(grouped, read_descriptor)

        worker = Worker(job, self)

        def on_done(payload):
            status, value = payload
            if status != "ok":
                self.statusBar().showMessage(f"电量扫描出错: {value}")
                return
            self._battery_cache = {r.vidpid: r for r in value}
            known = [r for r in value if r.percent is not None]
            self.statusBar().showMessage(
                f"电量扫描完成 — {len(known)}/{len(value)} 台有读数")
            self._rebuild_cards()

        worker.done.connect(on_done)
        self._workers.append(worker)
        worker.start()

    # ---- 热插拔 / 关闭 ----

    def _start_watcher(self):
        self._watcher = HotplugWatcher(self)
        self._watcher.event.connect(self._on_hotplug)
        self._watcher.start()

    def _on_hotplug(self, event):
        d = event.device
        action = "接入" if event.kind == "added" else "移除"
        self.statusBar().showMessage(
            f"设备{action}: {d.vid_hex}:{d.pid_hex} {d.product or ''}")
        self.refresh_devices()

    def closeEvent(self, event):
        self._settings.setValue("window/geometry", self.saveGeometry())
        if self._watcher:
            self._watcher.stop_watching()
            self._watcher.wait(2000)
        super().closeEvent(event)


class AdvancedWindow(QMainWindow):

    log_line = Signal(str)          # hex 日志追加（工作线程 → GUI 线程）

    def __init__(self):
        super().__init__()
        self.setWindowTitle(f"高级模式（协议开发） — 外设万能调试器 {APP_VERSION}")
        self.resize(1080, 720)

        self.db = load_db(DEFAULT_DB_PATH)
        self._devices: dict[str, list[dm.DeviceInfo]] = {}   # "VID:PID" -> 接口列表
        self._workers: list[Worker] = []                     # 防 GC
        self._watcher: HotplugWatcher | None = None
        self._proto_controls: list[tuple[dict, dict]] = []   # (feature, {param: QSpinBox})
        self._settings = QSettings("AvR0raa", "peripheral-tool")
        self._battery_cache: dict[str, BatteryResult] = {}   # vidpid -> 电量结果

        self._build_ui()
        self.log_line.connect(self._append_log)

        # 设置持久化（任务 3.7）：窗口几何
        geometry = self._settings.value("window/geometry")
        if geometry is not None:
            self.restoreGeometry(geometry)

        self.refresh_devices()

        # 恢复上次会话：标签页 + 选中设备
        try:
            tab_index = int(self._settings.value("ui/tab", 1))
        except (TypeError, ValueError):
            tab_index = 1
        if 0 <= tab_index < self._tabs.count():
            self._tabs.setCurrentIndex(tab_index)
        last_key = self._settings.value("ui/selected_device", "")
        if last_key:
            for i in range(self._list.count()):
                if self._list.item(i).data(Qt.UserRole) == last_key:
                    self._list.setCurrentRow(i)
                    break

        self._start_watcher()

    # ---- UI 构建 ----

    def _build_ui(self):
        root = QWidget()
        layout = QVBoxLayout(root)

        splitter = QSplitter(Qt.Vertical)
        top = QSplitter(Qt.Horizontal)

        # 左：设备列表
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(0, 0, 0, 0)
        header = QHBoxLayout()
        title = QLabel("<b>设备列表</b>")
        self._count_label = QLabel("")
        batch_btn = QPushButton("批量探测")
        batch_btn.setToolTip("逐台探测已知品牌设备：有协议的跑 ping，否则只读探测 vendor 配置通道")
        batch_btn.clicked.connect(self._on_batch_probe)
        refresh_btn = QPushButton("刷新")
        refresh_btn.clicked.connect(self.refresh_devices)
        header.addWidget(title)
        header.addStretch(1)
        header.addWidget(self._count_label)
        header.addWidget(batch_btn)
        header.addWidget(refresh_btn)
        left_layout.addLayout(header)

        self._list = QListWidget()
        self._list.itemSelectionChanged.connect(self._on_select)
        left_layout.addWidget(self._list)
        top.addWidget(left)

        # 右：详情
        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.setContentsMargins(0, 0, 0, 0)

        self._detail_title = QLabel("选择左侧设备查看详情")
        self._detail_title.setTextFormat(Qt.RichText)
        right_layout.addWidget(self._detail_title)

        self._iface_table = QTableWidget(0, 3)
        self._iface_table.setHorizontalHeaderLabels(["接口", "UP:USAGE", "说明"])
        self._iface_table.horizontalHeader().setStretchLastSection(True)
        self._iface_table.setEditTriggers(QTableWidget.NoEditTriggers)
        self._iface_table.verticalHeader().setVisible(False)
        right_layout.addWidget(self._iface_table)

        desc_row = QHBoxLayout()
        self._desc_btn = QPushButton("读取并解析报告描述符")
        self._desc_btn.clicked.connect(self._on_read_descriptor)
        self._vendor_probe_btn = QPushButton("Vendor 只读探测")
        self._vendor_probe_btn.setToolTip(
            "对 vendor 自定义接口(usage page ≥ 0xFF00)只读 feature report——多设备测试/逆向起点")
        self._vendor_probe_btn.clicked.connect(self._on_vendor_probe)
        self._desc_status = QLabel("")
        desc_row.addWidget(self._desc_btn)
        desc_row.addWidget(self._vendor_probe_btn)
        desc_row.addWidget(self._desc_status, 1)
        right_layout.addLayout(desc_row)

        self._desc_output = QPlainTextEdit()
        self._desc_output.setReadOnly(True)
        self._desc_output.setFont(QFont("Consolas", 9))
        self._desc_output.setPlaceholderText("描述符解析结果…")
        self._desc_output.setMaximumHeight(160)
        right_layout.addWidget(self._desc_output)

        self._protocol_box = QVBoxLayout()
        right_layout.addLayout(self._protocol_box)

        # 配置档案区（任务 3.5）
        profile_row = QHBoxLayout()
        self._profile_name = QLineEdit()
        self._profile_name.setPlaceholderText("档案名（保存时用）")
        self._profile_save_btn = QPushButton("保存档案")
        self._profile_save_btn.setToolTip("把当前所有功能控件的参数值存为该设备的配置档案")
        self._profile_save_btn.clicked.connect(self._on_profile_save)
        self._profile_combo = QComboBox()
        self._profile_load_btn = QPushButton("加载到控件")
        self._profile_load_btn.clicked.connect(self._on_profile_load)
        self._profile_apply_btn = QPushButton("应用档案（执行全部）")
        self._profile_apply_btn.setToolTip("按档案参数逐个执行协议功能；write 功能走安全写路径")
        self._profile_apply_btn.clicked.connect(self._on_profile_apply)
        profile_row.addWidget(self._profile_name, 1)
        profile_row.addWidget(self._profile_save_btn)
        profile_row.addWidget(self._profile_combo, 1)
        profile_row.addWidget(self._profile_load_btn)
        profile_row.addWidget(self._profile_apply_btn)
        right_layout.addLayout(profile_row)
        right_layout.addStretch(1)
        top.addWidget(right)
        top.setSizes([380, 700])

        # 下：hex 日志
        self._log = QPlainTextEdit()
        self._log.setReadOnly(True)
        self._log.setFont(QFont("Consolas", 9))
        self._log.setPlaceholderText("TX/RX hex 日志…")
        self._log.setMaximumHeight(180)

        splitter.addWidget(top)
        splitter.addWidget(self._log)
        layout.addWidget(splitter)

        # 顶部标签页：总览（3.6）+ 设备页 + 学习模式页
        self._devices_tab = root
        self._tabs = QTabWidget()
        self._tabs.addTab(self._build_dashboard_tab(), "总览")
        self._tabs.addTab(root, "设备")
        self._tabs.addTab(self._build_learn_tab(), "学习模式")
        self.setCentralWidget(self._tabs)

    # ---- 设备列表 ----

    def refresh_devices(self):
        try:
            devices = dm.list_devices()
        except dm.HIDError as exc:
            self._count_label.setText(f"枚举失败: {exc}")
            return
        grouped: dict[str, list[dm.DeviceInfo]] = {}
        for d in devices:
            grouped.setdefault(f"{d.vid_hex}:{d.pid_hex}", []).append(d)
        self._devices = grouped
        self._count_label.setText(f"{len(grouped)} 台设备 / {len(devices)} 接口")

        selected = self._current_key()
        self._list.clear()
        for key, ifaces in grouped.items():
            first = ifaces[0]
            result = fingerprint(first, self.db)
            product = first.product or "(未命名)"
            item = QListWidgetItem(
                f"{STATUS_ICON[result.status]} {key}  {product}  ({len(ifaces)}接口)")
            item.setForeground(STATUS_COLOR[result.status])
            item.setData(Qt.UserRole, key)
            item.setToolTip(STATUS_TEXT[result.status] + "\n" + result.detail)
            self._list.addItem(item)
            if key == selected:
                item.setSelected(True)
        self._refresh_learn_targets()
        self._refresh_dashboard()

    def _current_key(self) -> str | None:
        items = self._list.selectedItems()
        return items[0].data(Qt.UserRole) if items else None

    def _start_watcher(self):
        self._watcher = HotplugWatcher(self)
        self._watcher.event.connect(self._on_hotplug)
        self._watcher.start()

    def _on_hotplug(self, event):
        d = event.device
        action = "＋ 接入" if event.kind == "added" else "－ 移除"
        self.log_line.emit(f"── 热插拔 {action}: {d.vid_hex}:{d.pid_hex} "
                           f"{d.product or '(未命名)'}")
        self.refresh_devices()

    def closeEvent(self, event):
        self._settings.setValue("window/geometry", self.saveGeometry())
        self._settings.setValue("ui/tab", self._tabs.currentIndex())
        self._settings.setValue("ui/selected_device", self._current_key() or "")
        if self._watcher:
            self._watcher.stop_watching()
            self._watcher.wait(2000)
        if self._learn_recorder is not None:
            self._learn_timer.stop()
            self._learn_recorder.stop()
        super().closeEvent(event)

    # ---- 详情页 ----

    def _on_select(self):
        key = self._current_key()
        self._desc_output.clear()
        self._desc_status.setText("")
        self._clear_protocol_panel()
        if key is None:
            self._profile_combo.clear()
            self._detail_title.setText("选择左侧设备查看详情")
            return
        self._reload_profile_combo(key)
        ifaces = self._devices.get(key, [])
        first = ifaces[0]
        result = fingerprint(first, self.db)

        self._detail_title.setText(
            f"<b>{first.product or '(未命名)'}</b> &nbsp; "
            f"<span style='color:{STATUS_COLOR[result.status].name()}'>"
            f"{STATUS_TEXT[result.status]}</span> &nbsp;|&nbsp; "
            f"品牌: {result.brand_zh or result.brand or '?'} &nbsp;|&nbsp; "
            f"序列号: {first.serial_number or '-'}"
        )
        if result.entry:
            self._detail_title.setText(self._detail_title.text() +
                                       f"<br><small>{result.detail}</small>")

        self._iface_table.setRowCount(0)
        for d in ifaces:
            row = self._iface_table.rowCount()
            self._iface_table.insertRow(row)
            vendor_mark = "（厂商自定义）" if d.usage_page >= 0xFF00 else ""
            self._iface_table.setItem(row, 0, QTableWidgetItem(str(d.interface_number)))
            self._iface_table.setItem(
                row, 1, QTableWidgetItem(f"0x{d.usage_page:04X}:0x{d.usage:04X}"))
            note = f"{result.detail if row == 0 else ''}{vendor_mark}"
            self._iface_table.setItem(row, 2, QTableWidgetItem(note))

        self._build_protocol_panel(key, ifaces, result)

    def _clear_protocol_panel(self):
        self._proto_controls = []
        while self._protocol_box.count():
            item = self._protocol_box.takeAt(0)
            widget = item.widget()
            if widget:
                widget.deleteLater()
            elif item.layout():
                self._clear_layout(item.layout())

    def _clear_layout(self, layout):
        while layout.count():
            item = layout.takeAt(0)
            widget = item.widget()
            if widget:
                widget.deleteLater()
            elif item.layout():
                self._clear_layout(item.layout())

    def _build_protocol_panel(self, key, ifaces, result):
        """按协议库动态生成功能控件（任务 3.2）。"""
        entry = result.entry
        if not entry or "protocol" not in entry:
            if result.status is DeviceStatus.UNKNOWN:
                box = QGroupBox("协议功能")
                box.setLayout(QVBoxLayout(QLabel("未知设备：M4 学习模式上线后可逆向协议")))
                self._protocol_box.addWidget(box)
            return

        proto = entry["protocol"]
        match = proto["transport"]["match"]
        target = [
            d for d in ifaces
            if f"{d.usage_page:04X}" == match["usage_page"]
            and f"{d.usage:04X}" == match["usage"]
        ]
        info = QLabel(f"协议: {proto['name']}（接口匹配 UP:{match['usage_page']} "
                      f"U:{match['usage']}{'，已找到' if target else '，⚠ 未枚举到该接口'}）")
        self._protocol_box.addWidget(info)

        for feature in proto.get("features", []):
            box = QGroupBox(feature.get("label", feature["id"]))
            form = QFormLayout(box)
            spinboxes: dict[str, QSpinBox] = {}
            for name, spec in feature.get("params", {}).items():
                low, high = spec.get("min", 0), spec.get("max", 255)
                spin = QSpinBox()
                spin.setRange(low, high)
                spin.setValue(spec.get("default", 0))
                if high - low > 3:                       # 范围大 → 配滑条（任务 3.4）
                    slider = QSlider(Qt.Horizontal)
                    slider.setRange(low, high)
                    slider.setValue(spin.value())
                    spin.valueChanged.connect(slider.setValue)
                    slider.valueChanged.connect(spin.setValue)
                    form.addRow(spec.get("label", name), spin)
                    form.addRow("", slider)
                else:
                    form.addRow(spec.get("label", name), spin)
                spinboxes[name] = spin
            self._proto_controls.append((feature, spinboxes))
            run_btn = QPushButton("执行")
            run_btn.clicked.connect(
                lambda _, k=key, f=feature, s=dict(spinboxes): self._on_run_feature(k, f, s))
            form.addRow(run_btn)
            self._protocol_box.addWidget(box)

        if result.brand == "Logitech":
            self._build_dpi_panel(key, ifaces)

    def _build_dpi_panel(self, key: str, ifaces: list):
        """罗技设备 DPI 控制面板（M2.5b，HID++ 0x2201）。"""
        box = QGroupBox("鼠标 DPI（HID++ Adjustable DPI）")
        form = QFormLayout(box)

        slot_spin = QSpinBox()
        slot_spin.setRange(0, 7)
        slot_spin.setValue(1)
        form.addRow("接收器槽位（1-7，直连选 0=0xFF）", slot_spin)

        dpi_spin = QSpinBox()
        dpi_spin.setRange(50, 32000)
        dpi_spin.setSingleStep(50)
        dpi_spin.setValue(800)
        dpi_slider = QSlider(Qt.Horizontal)
        dpi_slider.setRange(50, 32000)
        dpi_slider.setValue(800)
        dpi_spin.valueChanged.connect(dpi_slider.setValue)
        dpi_slider.valueChanged.connect(dpi_spin.setValue)
        form.addRow("DPI", dpi_spin)
        form.addRow("", dpi_slider)

        info = QLabel("—")
        info.setWordWrap(True)

        def build_link():
            short = next((d for d in ifaces
                          if f"{d.usage_page:04X}" == "FF00" and f"{d.usage:04X}" == "0001"), None)
            long_if = next((d for d in ifaces
                            if f"{d.usage_page:04X}" == "FF00" and f"{d.usage:04X}" == "0002"), None)
            if short is None and long_if is None:
                return None
            return HidppLink(short.path if short else None, long_if.path if long_if else None)

        def on_read():
            dev = 0xFF if slot_spin.value() == 0 else slot_spin.value()
            info.setText("连接中…（设备睡眠时 HID++ 流量会唤醒它，请动一动鼠标）")

            def job():
                link = build_link()
                if link is None:
                    return None, "未找到 FF00 HID++ 接口"
                with link:
                    version = ensure_online(link, dev, attempts=10)
                    if version is None:
                        return None, "设备不在线（请动一动鼠标再试）"
                    dpi, dpi_y, lod, default = read_dpi(link, dev)
                    lst = get_dpi_list(link, dev)
                return (dpi, dpi_y, lod, default, lst), None

            worker = Worker(job, self)

            def done(payload):
                status, value = payload
                if status != "ok":
                    info.setText(f"读取出错: {value}")
                    return
                result, err = value
                if err:
                    info.setText(err)
                    return
                dpi, dpi_y, lod, default, lst = result
                if dpi is None:
                    info.setText("读取失败（设备可能睡眠中，动一动鼠标再试）")
                    return
                range_text = f"档位 {lst[0]}-{lst[-1]}（{len(lst)} 档）" if lst else "档位表未知"
                extra = ""
                if dpi_y is not None:
                    extra += f" · Y={dpi_y}"
                if lod is not None:
                    extra += f" · LOD={lod}"
                info.setText(f"当前 {dpi} DPI{extra} · {range_text}")
                if lst:
                    dpi_spin.setRange(lst[0], lst[-1])
                    dpi_slider.setRange(lst[0], lst[-1])

            worker.done.connect(done)
            self._workers.append(worker)
            worker.start()

        def on_set():
            dev = 0xFF if slot_spin.value() == 0 else slot_spin.value()
            target = dpi_spin.value()
            self.log_line.emit(f"── DPI 设置: 槽位{slot_spin.value()} → {target}")

            def job():
                link = build_link()
                if link is None:
                    return "未找到 FF00 HID++ 接口"
                with link:
                    if ensure_online(link, dev, attempts=6) is None:
                        return "设备不在线（请动一动鼠标再试）"
                    outcome = set_dpi(link, dev, target)
                return "\n".join(outcome.log)

            worker = Worker(job, self)
            worker.done.connect(lambda payload: self.log_line.emit(
                ("DPI 结果\n" + payload[1]) if payload[0] == "ok" else f"DPI 设置出错: {payload[1]}"))
            self._workers.append(worker)
            worker.start()

        read_btn = QPushButton("读取当前 DPI / 档位表")
        read_btn.clicked.connect(on_read)
        set_btn = QPushButton("应用 DPI（读回验证+失败自动回滚）")
        set_btn.clicked.connect(on_set)
        form.addRow(read_btn)
        form.addRow(set_btn)
        form.addRow(info)
        self._protocol_box.addWidget(box)

    # ---- 总览仪表板（任务 3.6）----

    def _build_dashboard_tab(self) -> QWidget:
        page = QWidget()
        outer = QVBoxLayout(page)
        head = QHBoxLayout()
        head.addWidget(QLabel(
            "<b>设备总览</b> — 插拔实时更新，点「查看」进入设备页控制"))
        head.addStretch(1)
        battery_btn = QPushButton("读取全部电量")
        battery_btn.setToolTip("罗技走 HID++ 电量，其余扫标准 HID Battery 页(0x85)；零写入")
        battery_btn.clicked.connect(self._on_battery_all)
        head.addWidget(battery_btn)
        outer.addLayout(head)
        outer.addWidget(QLabel(
            "<small>快速上手：①「设备」页选中设备看详情/控制（罗技设备有 DPI 面板） "
            "② 未知设备去「学习模式」页逆向 ③ 罗技鼠标也可用命令行 cli.py mouse</small>"))
        self._dash_host = QWidget()
        self._dash_grid = QGridLayout(self._dash_host)
        self._dash_grid.setSpacing(8)
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(self._dash_host)
        outer.addWidget(scroll, 1)
        return page

    def _refresh_dashboard(self):
        grid = self._dash_grid
        while grid.count():
            item = grid.takeAt(0)
            widget = item.widget()
            if widget is not None:
                widget.deleteLater()
        if not self._devices:
            grid.addWidget(QLabel("没有发现 HID 设备（插上外设或点刷新）"), 0, 0)
            return
        for index, (key, ifaces) in enumerate(sorted(self._devices.items())):
            first = ifaces[0]
            result = fingerprint(first, self.db)
            card = QGroupBox(first.product or "(未命名)")
            card_layout = QVBoxLayout(card)
            status_label = QLabel(STATUS_TEXT[result.status])
            status_label.setStyleSheet(f"color: {STATUS_COLOR[result.status].name()}; font-weight: bold;")
            feature_count = 0
            if result.entry and "protocol" in result.entry:
                feature_count = len(result.entry["protocol"].get("features", []))
            info = QLabel(f"{key}\n品牌: {result.brand_zh or result.brand or '?'} · "
                          f"{len(ifaces)} 接口 · 协议功能 {feature_count} 个")
            battery_text = ""
            cached = self._battery_cache.get(key)
            if cached is not None:
                battery_text = f"\n电量: {cached.display}"
            info = QLabel(f"{key}\n品牌: {result.brand_zh or result.brand or '?'} · "
                          f"{len(ifaces)} 接口 · 协议功能 {feature_count} 个{battery_text}")
            view_btn = QPushButton("查看")
            view_btn.clicked.connect(lambda _, k=key: self._goto_device(k))
            card_layout.addWidget(status_label)
            card_layout.addWidget(info)
            card_layout.addWidget(view_btn)
            grid.addWidget(card, index // 3, index % 3)

    def _goto_device(self, key: str):
        self._tabs.setCurrentWidget(self._devices_tab)
        for i in range(self._list.count()):
            if self._list.item(i).data(Qt.UserRole) == key:
                self._list.setCurrentRow(i)
                break

    def _on_battery_all(self):
        """全部设备电量扫描（后台线程，零写入路径）。"""
        grouped = dict(self._devices)
        self.log_line.emit(f"── 电量扫描开始（{len(grouped)} 台设备）──")

        def read_descriptor(path):
            import hid
            try:
                dev = hid.device()
                dev.open_path(path.encode("utf-8") if isinstance(path, str) else path)
                try:
                    return bytes(dev.get_report_descriptor())
                finally:
                    dev.close()
            except Exception:
                return None

        def job():
            return scan_all(grouped, read_descriptor)

        worker = Worker(job, self)

        def on_done(payload):
            status, value = payload
            if status != "ok":
                self.log_line.emit(f"电量扫描出错: {value}")
                return
            self._battery_cache = {r.vidpid: r for r in value}
            for r in value:
                self.log_line.emit(f"[{r.vidpid}] {r.product}: 电量 {r.display} — {r.detail}")
            self.log_line.emit("── 电量扫描完成 ──")
            self._refresh_dashboard()

        worker.done.connect(on_done)
        self._workers.append(worker)
        worker.start()

    # ---- 学习模式页（M4.4）----

    def _build_learn_tab(self) -> QWidget:
        page = QWidget()
        layout = QVBoxLayout(page)

        tip = QLabel(
            "逆向未知设备配置协议：① 勾选要录制的接口 → ② 开始录制 → ③ 点「标记起点」"
            "→ ④ 去官方驱动软件改一次设置（如 DPI 800→1600）→ ⑤ 停止并分析 → ⑥ 填说明后入库（unverified）")
        tip.setWordWrap(True)
        layout.addWidget(tip)

        self._learn_targets = QListWidget()
        self._learn_targets.setToolTip("vendor 自定义页(≥0xFF00)与 rawhid(usage=0) 接口")
        layout.addWidget(self._learn_targets, 1)

        btn_row = QHBoxLayout()
        self._learn_start_btn = QPushButton("开始录制")
        self._learn_mark_btn = QPushButton("标记起点")
        self._learn_stop_btn = QPushButton("停止并分析")
        self._learn_commit_btn = QPushButton("生成草稿入库（unverified）")
        for b in (self._learn_mark_btn, self._learn_stop_btn, self._learn_commit_btn):
            b.setEnabled(False)
        self._learn_start_btn.clicked.connect(self._on_learn_start)
        self._learn_mark_btn.clicked.connect(self._on_learn_mark)
        self._learn_stop_btn.clicked.connect(self._on_learn_stop)
        self._learn_commit_btn.clicked.connect(self._on_learn_commit)
        btn_row.addWidget(self._learn_start_btn)
        btn_row.addWidget(self._learn_mark_btn)
        btn_row.addWidget(self._learn_stop_btn)
        btn_row.addStretch(1)
        btn_row.addWidget(self._learn_commit_btn)
        layout.addLayout(btn_row)

        self._learn_note = QLineEdit()
        self._learn_note.setPlaceholderText("描述你刚做了什么（例: DPI 800→1600 / RGB 改红色 / 关闭呼吸灯）")
        layout.addWidget(self._learn_note)

        self._learn_status = QLabel("空闲")
        layout.addWidget(self._learn_status)

        self._learn_output = QPlainTextEdit()
        self._learn_output.setReadOnly(True)
        self._learn_output.setFont(QFont("Consolas", 9))
        self._learn_output.setPlaceholderText("差分分析结果…")
        layout.addWidget(self._learn_output, 1)

        # 运行态
        self._learn_recorder: Recorder | None = None
        self._learn_mark_ts: float | None = None
        self._learn_diffs: list = []
        self._learn_label_meta: dict = {}
        self._learn_timer = QTimer(self)
        self._learn_timer.setInterval(500)
        self._learn_timer.timeout.connect(self._learn_tick)
        return page

    def _refresh_learn_targets(self):
        """从当前设备列表填充可录制接口（保留勾选状态）。"""
        prev_checked = set()
        for idx in range(self._learn_targets.count()):
            item = self._learn_targets.item(idx)
            if item.checkState() == Qt.Checked:
                prev_checked.add(item.text())
        self._learn_targets.clear()
        for key, ifaces in self._devices.items():
            product = (ifaces[0].product or "(未命名)")[:20]
            for d in ifaces:
                is_vendor = d.usage_page >= 0xFF00
                is_rawhid = d.usage_page == 0x0001 and d.usage == 0x0000
                if not (is_vendor or is_rawhid):
                    continue
                label = f"{key}/UP{d.usage_page:04X}-U{d.usage:04X}"
                item = QListWidgetItem(f"{label}  {product}")
                item.setFlags(item.flags() | Qt.ItemIsUserCheckable)
                item.setCheckState(Qt.Checked if label in prev_checked else Qt.Unchecked)
                item.setData(Qt.UserRole, {
                    "label": label, "vidpid": key, "path": d.path,
                    "usage_page": d.usage_page, "usage": d.usage,
                })
                self._learn_targets.addItem(item)

    def _learn_tick(self):
        if self._learn_recorder is not None:
            self._learn_status.setText(
                f"录制中… 已捕获 {len(self._learn_recorder.entries())} 条")

    def _on_learn_start(self):
        targets: list[RecorderTarget] = []
        meta: dict = {}
        for idx in range(self._learn_targets.count()):
            item = self._learn_targets.item(idx)
            if item.checkState() != Qt.Checked:
                continue
            info = item.data(Qt.UserRole)
            feature_ids: tuple = ()
            if info["usage_page"] >= 0xFF00:
                raw = self._read_descriptor(info["path"])
                if raw:
                    feature_ids = tuple(
                        i for i in feature_report_ids(parse_report_descriptor(raw)) if i)
            targets.append(RecorderTarget(
                label=info["label"], path=info["path"], feature_ids=feature_ids))
            meta[info["label"]] = info
        if not targets:
            self._learn_status.setText("没有勾选任何接口")
            return
        self._learn_label_meta = meta
        self._learn_recorder = Recorder(targets)
        self._learn_recorder.start()
        self._learn_mark_ts = None
        self._learn_diffs = []
        self._learn_output.clear()
        self._learn_start_btn.setEnabled(False)
        self._learn_mark_btn.setEnabled(True)
        self._learn_stop_btn.setEnabled(True)
        self._learn_commit_btn.setEnabled(False)
        self._learn_timer.start()
        self._learn_status.setText("录制中… 先点「标记起点」，再去官方软件改设置")

    def _on_learn_mark(self):
        if self._learn_recorder is None:
            return
        self._learn_mark_ts = time.time()
        self._learn_status.setText("起点已标记 —— 现在去官方软件改设置，改完回来点「停止并分析」")

    def _on_learn_stop(self):
        recorder = self._learn_recorder
        if recorder is None:
            return
        self._learn_timer.stop()
        recorder.stop()
        entries = recorder.entries()
        self._learn_recorder = None
        mark = self._learn_mark_ts
        if mark is None:
            before, after = [], entries
            self._learn_status.setText("⚠ 未标记起点：全部按「新报文」处理")
        else:
            before = [e for e in entries if e.ts < mark]
            after = [e for e in entries if e.ts >= mark]
            self._learn_status.setText(f"分析完成（起点前 {len(before)} 条 / 后 {len(after)} 条）")
        self._learn_diffs = diff_sessions(before, after)
        lines: list[str] = []
        for d in self._learn_diffs:
            lines.extend(format_diff(d))
        if not lines:
            lines = ["没有检测到差异（确认录制期间在官方软件里改了设置？）"]
        self._learn_output.setPlainText("\n".join(lines))
        self._learn_start_btn.setEnabled(True)
        self._learn_mark_btn.setEnabled(False)
        self._learn_stop_btn.setEnabled(False)
        self._learn_commit_btn.setEnabled(any(not d.volatile for d in self._learn_diffs))

    def _on_learn_commit(self):
        if not self._learn_diffs:
            return
        note = self._learn_note.text().strip()
        label_match = {
            label: {"usage_page": f"{info['usage_page']:04X}",
                    "usage": f"{info['usage']:04X}"}
            for label, info in self._learn_label_meta.items()
        }
        drafts = build_drafts(self._learn_diffs, note, label_match)
        if not drafts:
            self._learn_status.setText("没有可入库的草稿（只有噪声流？）")
            return
        by_device: dict[str, list] = {}
        for draft in drafts:
            info = self._learn_label_meta.get(draft["learned"]["source"], {})
            vidpid = info.get("vidpid")
            if vidpid:
                by_device.setdefault(vidpid, []).append(draft)
        try:
            for vidpid, device_drafts in by_device.items():
                block = build_protocol_block(device_drafts[0].get("match"), device_drafts)
                merge_into_db(self.db, vidpid, block, note=note or None)
            save_db(self.db, DEFAULT_DB_PATH)
        except Exception as exc:
            self._learn_status.setText(f"入库失败: {exc}")
            return
        self.log_line.emit(
            f"── 学习模式入库: {len(drafts)} 条草稿 → {', '.join(by_device)}（unverified）")
        self._learn_status.setText(
            f"✅ 已入库 {len(drafts)} 条草稿（unverified）。设备页切到该设备即可看到新功能按钮")
        self.db = load_db(DEFAULT_DB_PATH)
        self.refresh_devices()
        self._learn_commit_btn.setEnabled(False)

    # ---- 动作 ----

    def _on_read_descriptor(self):
        key = self._current_key()
        if key is None:
            return
        ifaces = self._devices.get(key, [])
        self._desc_btn.setEnabled(False)
        self._desc_status.setText("读取中…")

        def job():
            parts = []
            for d in ifaces:
                raw = self._read_descriptor(d.path)
                header = f"── 接口 {d.interface_number} UP:0x{d.usage_page:04X} U:0x{d.usage:04X}"
                if raw is None:
                    parts.append(f"{header}\n   (读取失败：设备被系统占用或接口不可读)")
                    continue
                info = parse_report_descriptor(bytes(raw))
                lines = [header] + [f"   {line}" for line in summary(info)]
                parts.append("\n".join(lines))
            return "\n\n".join(parts)

        worker = Worker(job, self)

        def on_done(payload):
            self._desc_btn.setEnabled(True)
            status, value = payload
            self._desc_status.setText("")
            self._desc_output.setPlainText(value if status == "ok" else f"错误: {value}")

        worker.done.connect(on_done)
        self._workers.append(worker)
        worker.start()

    @staticmethod
    def _read_descriptor(path) -> bytes | None:
        import hid
        try:
            dev = hid.device()
            dev.open_path(path.encode("utf-8") if isinstance(path, str) else path)
            try:
                return bytes(dev.get_report_descriptor())
            finally:
                dev.close()
        except Exception:
            return None

    def _on_vendor_probe(self):
        """单设备 vendor 配置通道只读探测（任务: 多设备测试）。"""
        key = self._current_key()
        if key is None:
            return
        ifaces = self._devices.get(key, [])
        self._vendor_probe_btn.setEnabled(False)
        self._desc_status.setText("只读探测中…")

        def job():
            results = probe_vendor_interfaces(ifaces, self._read_descriptor)
            rawhids = suspect_rawhid_interfaces(ifaces, self._read_descriptor)
            if not results and not rawhids:
                return (f"{key}: 没有发现 vendor 自定义接口或 rawhid 配置通道\n"
                        "说明: 配置通道可能藏在标准接口里，先看描述符解析结果")
            lines = []
            for r in results:
                head = f"UP:0x{r.usage_page:04X} U:0x{r.usage:04X}"
                if r.error:
                    lines.append(f"{head}  {r.error}")
                    continue
                for report_id, data in r.reads.items():
                    body = data.hex(" ") if data else "<读取失败>"
                    lines.append(f"{head}  feature id={report_id}: {body}")
            for r in rawhids:
                data = r.reads.get("input_passive")
                body = data.hex(" ") if data else "<200ms 内无主动推送>"
                lines.append(f"UP:0x{r.usage_page:04X} U:0x{r.usage:04X}  [rawhid] {r.note}\n"
                             f"    被动监听: {body}")
            return "\n".join(lines)

        worker = Worker(job, self)

        def on_done(payload):
            self._vendor_probe_btn.setEnabled(True)
            status, value = payload
            self._desc_status.setText("")
            self.log_line.emit(("Vendor 只读探测 ── " + value) if status == "ok"
                               else f"Vendor 只读探测出错: {value}")

        worker.done.connect(on_done)
        self._workers.append(worker)
        worker.start()

    def _on_batch_probe(self):
        """批量探测：多设备测试入口。已知品牌逐台探测并汇总在线状态。"""
        keys = list(self._devices.keys())
        self.log_line.emit(f"── 批量探测开始（{len(keys)} 台设备）──")

        def probe_one(key, ifaces, log):
            first = ifaces[0]
            result = fingerprint(first, self.db)
            if result.brand is None:
                log(f"[{key}] 跳过（未知品牌，非外设厂商）")
                return None
            label = f"{key} {result.brand_zh or result.brand} {first.product or ''}".strip()

            # 1) 有协议 + ping → ping 探测
            entry = result.entry or {}
            proto = entry.get("protocol")
            ping = next((f for f in (proto or {}).get("features", [])
                         if f.get("id") == "ping"), None)
            if proto and ping:
                match = proto["transport"]["match"]
                target = next((d for d in ifaces
                               if f"{d.usage_page:04X}" == match["usage_page"]
                               and f"{d.usage:04X}" == match["usage"]), None)
                if target is None:
                    log(f"[{label}] 协议接口未枚举到（UP:{match['usage_page']} U:{match['usage']}）")
                    return (key, "无接口", label)
                io = ReportIO(target.path)
                io.on_history = lambda e: self.log_line.emit(
                    f"{'TX>' if e.direction == 'tx' else 'RX<'} {e.data.hex(' ')}")
                try:
                    with io:
                        r = execute_feature(io, ping, None, timeout_ms=400)
                finally:
                    io.on_history = None
                if r.response is None:
                    log(f"[{label}] ping 无响应")
                    return (key, "不在线", label)
                if r.ok:
                    log(f"[{label}] ping ✅ HID++ {r.decoded.get('major')}."
                        f"{r.decoded.get('minor')}")
                else:
                    log(f"[{label}] ping 应答: {r.branch} "
                        f"error=0x{r.decoded.get('error_code', 0):02X}")
                return (key, "在线", label)

            # 2) 无协议 → vendor 只读探测 + rawhid 通道识别
            results = probe_vendor_interfaces(ifaces, self._read_descriptor)
            rawhids = suspect_rawhid_interfaces(ifaces, self._read_descriptor)
            if not results and not rawhids:
                log(f"[{label}] 无 vendor 自定义接口/rawhid 配置通道")
                return (key, "无配置通道", label)
            channel_count = sum(len(r.reads) for r in results)
            for r in results:
                for report_id, data in r.reads.items():
                    body = data.hex(" ") if data else "<读取失败>"
                    log(f"[{label}] UP:0x{r.usage_page:04X} feature id={report_id}: {body}")
            for r in rawhids:
                log(f"[{label}] rawhid 配置通道: UP:0x{r.usage_page:04X} "
                    f"U:0x{r.usage:04X}（写入能力待 M4 验证）")
            return (key, f"探测到 {channel_count + len(rawhids)} 个配置通道", label)

        def job():
            summary = []
            for key in keys:
                ifaces = self._devices.get(key, [])
                outcome = probe_one(key, ifaces, lambda s: self.log_line.emit(s))
                if outcome:
                    summary.append(outcome)
            return summary

        worker = Worker(job, self)

        def on_done(payload):
            status, value = payload
            if status != "ok":
                self.log_line.emit(f"批量探测出错: {value}")
                return
            online = [s for s in value if s[1] == "在线"]
            self.log_line.emit(
                f"── 批量探测完成: 已知品牌 {len(value)} 台，在线 {len(online)} 台 ──")

        worker.done.connect(on_done)
        self._workers.append(worker)
        worker.start()

    def _on_run_feature(self, key: str, feature: dict, spinboxes: dict):
        ifaces = self._devices.get(key, [])
        entry = fingerprint(ifaces[0], self.db).entry
        proto = entry["protocol"]
        match = feature.get("match") or proto["transport"]["match"]   # 功能级 match 优先
        target = next((
            d for d in ifaces
            if f"{d.usage_page:04X}" == match["usage_page"]
            and f"{d.usage:04X}" == match["usage"]), None)
        params = {name: spin.value() for name, spin in spinboxes.items()}
        self.log_line.emit(f"── 执行 {feature['id']} 参数 {params}")

        def job():
            if target is None:
                return "找不到协议要求的接口（UP:{usage_page} U:{usage}）".format(**match)
            io = ReportIO(target.path)
            io.on_history = lambda e: self.log_line.emit(
                f"{'TX>' if e.direction == 'tx' else 'RX<'} {e.data.hex(' ')}")
            try:
                with io:
                    result = execute_feature(
                        io, feature, params,
                        timeout_ms=entry["protocol"]["transport"].get("response_timeout_ms", 600))
            finally:
                io.on_history = None
            if result.response is None:
                return "无响应（超时）"
            lines = [f"响应 {len(result.response)} 字节: {result.response.hex(' ')}"]
            if result.ok:
                lines.append("✅ 主应答: " +
                             (", ".join(f"{k}=0x{v:02X}" if isinstance(v, int) else f"{k}={v}"
                                        for k, v in result.decoded.items()) or "匹配"))
            elif result.branch:
                lines.append(f"ℹ 备用应答（{result.branch}）: " +
                             ", ".join(f"{k}=0x{v:02X}" for k, v in result.decoded.items()
                                       if isinstance(v, int)))
            return "\n".join(lines)

        worker = Worker(job, self)
        worker.done.connect(lambda payload: self.log_line.emit(
            ("执行完成\n" + payload[1]) if payload[0] == "ok" else f"执行出错: {payload[1]}"))
        self._workers.append(worker)
        worker.start()

    # ---- 配置档案（任务 3.5）----

    def _reload_profile_combo(self, key: str):
        self._profile_combo.clear()
        for path in list_profiles(key, directory=DEFAULT_PROFILE_DIR):
            self._profile_combo.addItem(path.stem, str(path))

    def _on_profile_save(self):
        key = self._current_key()
        if key is None:
            return
        name = self._profile_name.text().strip() or time.strftime("profile_%Y%m%d_%H%M%S")
        profile = DeviceProfile(
            vidpid=key,
            name=name,
            features=[
                ProfileFeature(feature["id"], {n: s.value() for n, s in spins.items()})
                for feature, spins in self._proto_controls
            ],
        )
        try:
            path = save_profile(profile, directory=DEFAULT_PROFILE_DIR)
        except Exception as exc:
            self.log_line.emit(f"档案保存失败: {exc}")
            return
        self._profile_name.clear()
        self._reload_profile_combo(key)
        self._profile_combo.setCurrentText(path.stem)
        self.log_line.emit(f"── 档案已保存: {path.name}（{key}，{len(profile.features)} 个功能）")

    def _on_profile_load(self):
        data = self._profile_combo.currentData()
        if not data:
            return
        profile = load_profile(__import__("pathlib").Path(data))
        restored = 0
        for pf in profile.features:
            for feature, spins in self._proto_controls:
                if feature["id"] != pf.feature_id:
                    continue
                for name, value in pf.params.items():
                    if name in spins:
                        spins[name].setValue(int(value))
                        restored += 1
        self.log_line.emit(f"── 档案「{profile.name}」已加载到控件（{restored} 个参数）")

    def _on_profile_apply(self):
        key = self._current_key()
        data = self._profile_combo.currentData()
        if key is None or not data:
            return
        profile = load_profile(__import__("pathlib").Path(data))
        ifaces = self._devices.get(key, [])
        entry = fingerprint(ifaces[0], self.db).entry
        if not entry or "protocol" not in entry:
            self.log_line.emit(f"[{key}] 无协议库条目，无法应用档案")
            return
        proto = entry["protocol"]
        features_by_id = {f["id"]: f for f in proto.get("features", [])}
        timeout_ms = proto["transport"].get("response_timeout_ms", 600)
        self.log_line.emit(f"── 应用档案「{profile.name}」到 {key}（{len(profile.features)} 个功能）")

        def job():
            lines = []
            for pf in profile.features:
                feature = features_by_id.get(pf.feature_id)
                if feature is None:
                    lines.append(f"[{pf.feature_id}] 协议库中已不存在，跳过")
                    continue
                match = feature.get("match") or proto["transport"]["match"]
                target = next((d for d in ifaces
                               if f"{d.usage_page:04X}" == match["usage_page"]
                               and f"{d.usage:04X}" == match["usage"]), None)
                if target is None:
                    lines.append(f"[{feature['id']}] 接口 UP:{match['usage_page']} 未枚举到，跳过")
                    continue
                io = ReportIO(target.path)
                io.on_history = lambda e: self.log_line.emit(
                    f"{'TX>' if e.direction == 'tx' else 'RX<'} {e.data.hex(' ')}")
                try:
                    with io:
                        if feature.get("kind") == "write":
                            read_feature = features_by_id.get(feature.get("read_from"))
                            outcome = safe_write(io, feature, read_feature=read_feature,
                                                 new_params=dict(pf.params), timeout_ms=timeout_ms)
                            lines.append(f"[{feature['id']}] write "
                                         f"{'✅' if outcome.ok else '❌'} "
                                         f"verified={outcome.verified} rolled_back={outcome.rolled_back}")
                        else:
                            result = execute_feature(io, feature, dict(pf.params),
                                                     timeout_ms=timeout_ms)
                            if result.response is None:
                                lines.append(f"[{feature['id']}] 无响应（超时）")
                            elif result.ok:
                                lines.append(f"[{feature['id']}] ✅ 主应答")
                            else:
                                branch_text = result.branch or "未匹配"
                                lines.append(f"[{feature['id']}] ℹ {branch_text}")
                finally:
                    io.on_history = None
            return "\n".join(lines) or "没有可应用的功能"

        worker = Worker(job, self)
        worker.done.connect(lambda payload: self.log_line.emit(
            ("应用完成\n" + payload[1]) if payload[0] == "ok" else f"应用出错: {payload[1]}"))
        self._workers.append(worker)
        worker.start()

    # ---- 日志 ----

    def _append_log(self, line: str):
        self._log.appendPlainText(line)


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())

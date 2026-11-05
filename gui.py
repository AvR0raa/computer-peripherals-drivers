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

from PySide6.QtCore import Qt, QThread, Signal
from PySide6.QtGui import QColor, QFont
from PySide6.QtWidgets import (
    QApplication,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QMainWindow,
    QPlainTextEdit,
    QPushButton,
    QSpinBox,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

import core.device_manager as dm
from core.descriptor import parse_report_descriptor, summary
from core.fingerprint import (
    DEFAULT_DB_PATH,
    DeviceStatus,
    fingerprint,
    load_db,
)
from core.probe import probe_vendor_interfaces, suspect_rawhid_interfaces
from core.report_io import ReportIO, execute_feature

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

class MainWindow(QMainWindow):

    log_line = Signal(str)          # hex 日志追加（工作线程 → GUI 线程）

    def __init__(self):
        super().__init__()
        self.setWindowTitle("外设调试工具 M3 — 计算机外设驱动合集")
        self.resize(1080, 720)

        self.db = load_db(DEFAULT_DB_PATH)
        self._devices: dict[str, list[dm.DeviceInfo]] = {}   # "VID:PID" -> 接口列表
        self._workers: list[Worker] = []                     # 防 GC
        self._watcher: HotplugWatcher | None = None

        self._build_ui()
        self.log_line.connect(self._append_log)
        self.refresh_devices()
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
        self.setCentralWidget(root)

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
        if self._watcher:
            self._watcher.stop_watching()
            self._watcher.wait(2000)
        super().closeEvent(event)

    # ---- 详情页 ----

    def _on_select(self):
        key = self._current_key()
        self._desc_output.clear()
        self._desc_status.setText("")
        self._clear_protocol_panel()
        if key is None:
            self._detail_title.setText("选择左侧设备查看详情")
            return
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
                spin = QSpinBox()
                spin.setRange(spec.get("min", 0), spec.get("max", 255))
                spin.setValue(spec.get("default", 0))
                form.addRow(spec.get("label", name), spin)
                spinboxes[name] = spin
            run_btn = QPushButton("执行")
            run_btn.clicked.connect(
                lambda _, k=key, f=feature, s=dict(spinboxes): self._on_run_feature(k, f, s))
            form.addRow(run_btn)
            self._protocol_box.addWidget(box)

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
        match = entry["protocol"]["transport"]["match"]
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

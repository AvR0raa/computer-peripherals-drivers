"""用户客户端（v0.6.1 单页卡片墙）测试：离屏模式，全部 monkeypatch 隔离。"""
import os

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

import core.report_io as rio
import core.device_manager as dm
import gui as gui_mod
from tests.test_device_manager import fake_raw
from tests.test_report_io import FakeDev
from core.battery import BatteryResult


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance() or QApplication([])
    yield app


class _MemSettings:
    """内存版 QSettings 替身，防止测试写真实注册表/互相污染。"""

    _store = {}

    def __init__(self, *a, **k):
        pass

    def value(self, key, default=None, **k):
        return self._store.get(key, default)

    def setValue(self, key, value):
        self._store[key] = value

    def clear(self):
        self._store.clear()


@pytest.fixture(autouse=True)
def isolated_settings(monkeypatch):
    _MemSettings._store.clear()
    monkeypatch.setattr(gui_mod, "QSettings", _MemSettings)


@pytest.fixture
def window(qapp, monkeypatch):
    monkeypatch.setattr(dm, "list_devices", lambda: [
        dm.DeviceInfo.from_hid_dict(fake_raw("a")),                     # 046D:C54D 罗技
        dm.DeviceInfo.from_hid_dict(fake_raw("a2", usage_page=0xFF00, usage=0x0001)),  # 罗技 HID++ 接口
        dm.DeviceInfo.from_hid_dict(fake_raw("b", vid=0x291D, pid=0x385D,
                                             product="MCHOSE V9 PRO",
                                             usage_page=0xFF21)),
        dm.DeviceInfo.from_hid_dict(fake_raw("c", vid=0xDEAD, pid=0xBEEF,
                                             product="Mystery Box")),
    ])
    monkeypatch.setattr(gui_mod, "scan_all",
                        lambda grouped, rd: [BatteryResult(vidpid="046D:C54D", percent=92)])
    win = gui_mod.MainWindow()
    yield win
    win.close()


def test_cards_built_for_all_devices(window):
    assert window._grid.count() == 3


def test_logitech_card_has_dpi_button(window):
    # 卡片里罗技那张有「调节 DPI」按钮
    texts = []
    for i in range(window._grid.count()):
        w = window._grid.itemAt(i).widget()
        if w is not None:
            for b in w.findChildren(type(window._grid)):
                pass
            texts.append(w)
    dpi_cards = [w for w in texts
                 if any(btn.text() == "调节 DPI" for btn in w.findChildren(__import__("PySide6.QtWidgets", fromlist=["QPushButton"]).QPushButton))]
    assert len(dpi_cards) == 1                                      # 只有罗技接收器


def test_battery_button_updates_cards(window, qapp):
    window._on_battery_all()
    window._workers[-1].wait(3000)
    for _ in range(10):
        qapp.processEvents()
    # 罗技卡应显示 92%
    from PySide6.QtWidgets import QLabel
    found = False
    for i in range(window._grid.count()):
        w = window._grid.itemAt(i).widget()
        if w is None:
            continue
        for lbl in w.findChildren(QLabel):
            if "92%" in lbl.text():
                found = True
    assert found, "电量 92% 应出现在罗技卡片上"


def test_unknown_device_card_shows_hint(window):
    from PySide6.QtWidgets import QLabel
    hit = False
    for i in range(window._grid.count()):
        w = window._grid.itemAt(i).widget()
        if w is None:
            continue
        for lbl in w.findChildren(QLabel):
            if "未识别" in lbl.text():
                hit = True
    assert hit                                                      # Mystery Box 是未知品牌


def test_open_dpi_dialog_reads(window, qapp, monkeypatch):
    monkeypatch.setattr(gui_mod, "ensure_online", lambda link, dev, attempts=8, **kw: 4.2)
    monkeypatch.setattr(gui_mod, "read_dpi", lambda link, dev: (1600, None, None, 800))
    monkeypatch.setattr(gui_mod, "get_dpi_list", lambda link, dev: list(range(100, 32001, 50)))
    monkeypatch.setattr(rio, "_open_device", lambda path: FakeDev())
    ifaces = window._devices["046D:C54D"]
    dlg = gui_mod.DpiDialog(window, "046D:C54D", ifaces, "USB Receiver")
    for _ in range(10):
        qapp.processEvents()
    window._workers[-1].wait(3000)
    for _ in range(20):
        qapp.processEvents()
    assert "1600" in dlg._status.text()
    assert "32000" in dlg._info.text()                              # 档位表范围
    dlg.reject()
    dlg.deleteLater()


def test_advanced_button_opens_developer_window(window):
    window._open_advanced()
    assert window._advanced is not None
    assert "高级模式" in window._advanced.windowTitle()
    window._advanced.close()


# ---------------------------------------------------------------------------
# 内部/系统设备过滤 + 手动隐藏（v0.6.3）
# ---------------------------------------------------------------------------

def test_internal_and_i2c_devices_filtered(window, monkeypatch):
    from core.battery import BatteryResult
    monkeypatch.setattr(gui_mod, "scan_all", lambda g, rd: [])
    # ITE(内部) + I2C 触控板 + 正常外设
    monkeypatch.setattr(dm, "list_devices", lambda: [
        dm.DeviceInfo.from_hid_dict(fake_raw("ite", vid=0x048D, pid=0xC103,
                                             product="ITE Device(8910)")),
        dm.DeviceInfo.from_hid_dict(fake_raw("tp", vid=0x04F3, pid=0x327E,
                                             product="HIDI2C Device", bus_type=3)),
        dm.DeviceInfo.from_hid_dict(fake_raw("ok", vid=0x1234, pid=0x5678,
                                             product="My Mouse")),
    ])
    window.refresh_devices()
    assert set(window._devices.keys()) == {"1234:5678"}


def test_hide_device_persists(window, qapp, monkeypatch, tmp_path):
    monkeypatch.setattr(dm, "list_devices", lambda: [
        dm.DeviceInfo.from_hid_dict(fake_raw("x", vid=0x1234, pid=0x5678,
                                             product="Annoying")),
    ])
    window.refresh_devices()
    assert window._grid.count() == 1
    window._hide_device("1234:5678")
    assert window._grid.count() == 1        # 空状态提示占位
    assert "1234:5678" in window._hidden_list()
    # 重新枚举仍然被隐藏
    window.refresh_devices()
    assert "1234:5678" not in window._devices

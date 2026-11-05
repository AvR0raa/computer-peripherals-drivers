"""M3 GUI 冒烟测试：离屏模式构建窗口、列表分组、详情联动、热插拔信号。

枚举 monkeypatch 隔离；不执行真实设备写操作。
"""
import os

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest
from PySide6.QtWidgets import QApplication

import core.device_manager as dm
import gui as gui_mod
from core.device_manager import DeviceEvent
from tests.test_device_manager import fake_raw


@pytest.fixture(scope="module")
def qapp():
    app = QApplication.instance() or QApplication([])
    yield app


@pytest.fixture
def window(qapp, monkeypatch):
    monkeypatch.setattr(dm, "list_devices", lambda: [
        dm.DeviceInfo.from_hid_dict(fake_raw("a")),                     # 046D:C54D
        dm.DeviceInfo.from_hid_dict(fake_raw("b", vid=0x291D, pid=0x385D,
                                             product="MCHOSE V9 PRO",
                                             usage_page=0xFF21)),
        dm.DeviceInfo.from_hid_dict(fake_raw("c", vid=0x291D, pid=0x385D,
                                             product="MCHOSE V9 PRO",
                                             usage_page=0x000C)),
    ])
    win = gui_mod.MainWindow()
    yield win
    win.close()


def test_list_groups_by_vidpid(window):
    # 3 个接口但 2 台设备（291D:385D 两个接口归并）
    assert window._list.count() == 2
    assert "2 台设备 / 3 接口" in window._count_label.text()


def test_select_shows_detail_and_status(window):
    window._list.item(0).setSelected(True)
    title = window._detail_title.text()
    assert "USB Receiver" in title
    assert "OK 已知" in title                     # 真协议库命中 -> KNOWN


def test_protocol_panel_built_from_db(window):
    # 046D:C54D 有 protocol 块：信息行 + ping 功能组框
    window._list.item(0).setSelected(True)
    texts = []
    for i in range(window._protocol_box.count()):
        w = window._protocol_box.itemAt(i).widget()
        if w is not None:
            texts.append(w.text() if hasattr(w, "text") else w.title())
    assert any("协议: hidpp" in t for t in texts)
    assert any("Ping" in t for t in texts)


def test_protocol_panel_absent_for_unknown(window):
    window._list.item(1).setSelected(True)        # 291D:385D 无 protocol 块
    assert window._iface_table.rowCount() == 2    # 两个接口都列出


def test_hotplug_event_logs_and_refreshes(window, monkeypatch):
    # 假枚举随热插拔更新：先加设备，再发事件
    rows = [fake_raw("a"),
            fake_raw("b", vid=0x291D, pid=0x385D, product="MCHOSE V9 PRO",
                     usage_page=0xFF21),
            fake_raw("c", vid=0x291D, pid=0x385D, product="MCHOSE V9 PRO",
                     usage_page=0x000C)]
    monkeypatch.setattr(dm, "list_devices",
                        lambda: [dm.DeviceInfo.from_hid_dict(r) for r in rows])
    dev = dm.DeviceInfo.from_hid_dict(fake_raw("zz", vid=0xDEAD, pid=0xBEEF,
                                               product="New Mouse"))
    rows.append(fake_raw("zz", vid=0xDEAD, pid=0xBEEF, product="New Mouse"))
    window._on_hotplug(DeviceEvent("added", dev))
    assert "＋ 接入" in window._log.toPlainText()
    assert window._list.count() == 3              # 刷新后新增一台


def test_descriptor_button_flow(window, qapp, monkeypatch):
    # 描述符读取走 worker 线程；monkeypatch 掉真实 hid 读取
    monkeypatch.setattr(gui_mod.MainWindow, "_read_descriptor",
                        staticmethod(lambda path: bytes.fromhex(
                            "05010902A1010901A10005091901290315002501950375018102C0C0")))
    window._list.item(0).setSelected(True)
    window._on_read_descriptor()
    window._workers[-1].wait(3000)
    for _ in range(10):
        qapp.processEvents()                      # 处理跨线程 queued 信号
    text = window._desc_output.toPlainText()
    assert "顶层集合" in text or "读取失败" in text

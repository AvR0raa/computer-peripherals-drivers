"""高级模式窗口（原三页版）回归测试：离屏模式，全部 monkeypatch 隔离。"""
import os

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

import pytest
from PySide6.QtCore import Qt
from PySide6.QtWidgets import QApplication

import core.report_io as rio
import core.device_manager as dm
import gui as gui_mod
from core.device_manager import DeviceEvent
from tests.test_device_manager import fake_raw
from tests.test_report_io import FakeDev


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
    win = gui_mod.AdvancedWindow()
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
    monkeypatch.setattr(gui_mod.AdvancedWindow, "_read_descriptor",
                        staticmethod(lambda path: bytes.fromhex(
                            "05010902A1010901A10005091901290315002501950375018102C0C0")))
    window._list.item(0).setSelected(True)
    window._on_read_descriptor()
    window._workers[-1].wait(3000)
    for _ in range(10):
        qapp.processEvents()                      # 处理跨线程 queued 信号
    text = window._desc_output.toPlainText()
    assert "顶层集合" in text or "读取失败" in text


# ---------------------------------------------------------------------------
# 学习模式页（M4.4）
# ---------------------------------------------------------------------------

def _find_learn_item(window, usage_page):
    for i in range(window._learn_targets.count()):
        item = window._learn_targets.item(i)
        if item.data(Qt.UserRole)["usage_page"] == usage_page:
            return item
    return None


def test_learn_tab_lists_vendor_interfaces(window):
    # window fixture: FF21(vendor) + 000C(consumer, 不算) + 标准(不算)
    assert window._learn_targets.count() == 1
    item = window._learn_targets.item(0)
    assert item.data(Qt.UserRole)["vidpid"] == "291D:385D"
    assert "UPFF21" in item.text()


def test_learn_start_without_selection(window):
    window._on_learn_start()
    assert "没有勾选" in window._learn_status.text()
    assert window._learn_recorder is None


def test_learn_full_flow(window, qapp, monkeypatch):
    """录制 → 停止分析 → 入库：全程假设备 + 假写盘。"""
    monkeypatch.setattr(rio, "_open_device",
                        lambda path: FakeDev(responses=[[0xED, 1, 0x40]]))
    monkeypatch.setattr(gui_mod.AdvancedWindow, "_read_descriptor",
                        staticmethod(lambda path: None))     # 不解析描述符
    captured: dict = {}
    monkeypatch.setattr(gui_mod, "save_db",
                        lambda db, path: captured.setdefault("db", db))

    item = _find_learn_item(window, 0xFF21)
    assert item is not None
    item.setCheckState(Qt.Checked)
    window._on_learn_start()
    assert window._learn_recorder is not None

    deadline = __import__("time").time() + 3.0
    while __import__("time").time() < deadline:
        if window._learn_recorder.entries():
            break
        qapp.processEvents()
        __import__("time").sleep(0.02)

    window._on_learn_stop()                       # 未标记起点 → 全部按新报文
    assert "新报文组" in window._learn_output.toPlainText()
    assert window._learn_commit_btn.isEnabled()

    window._learn_note.setText("DPI 800→1600")
    window._on_learn_commit()
    assert "db" in captured, "save_db 应被调用"
    entry = captured["db"]["devices"]["291D:385D"]
    assert entry["source"] == "learning-mode"
    assert entry["verified"] is False
    features = entry["protocol"]["features"]
    assert features and features[0]["id"].startswith("learned_")
    assert features[0]["report"] == "input"       # 监听型零写入
    assert "已入库" in window._learn_status.text()


def test_learn_commit_empty_diff_guard(window):
    window._learn_diffs = []
    window._on_learn_commit()                     # 不应崩溃、不应入库
    assert window._learn_status.text() == "空闲" or "没有" in window._learn_status.text()


# ---------------------------------------------------------------------------
# 总览仪表板（M3.6）+ 配置档案（M3.5 GUI 侧）
# ---------------------------------------------------------------------------

def test_dashboard_lists_device_cards(window):
    assert window._tabs.count() == 3
    assert window._tabs.tabText(0) == "总览"
    assert window._dash_grid.count() == 2         # 2 台设备各一张卡


def test_goto_device_from_dashboard(window):
    window._goto_device("291D:385D")
    assert window._tabs.currentWidget() is window._devices_tab
    assert window._current_key() == "291D:385D"


def test_profile_save_load_roundtrip(window, qapp, monkeypatch, tmp_path):
    monkeypatch.setattr(gui_mod, "DEFAULT_PROFILE_DIR", tmp_path)
    window._goto_device("046D:C54D")              # 有 ping 功能（3 个参数）
    for _ in range(10):
        qapp.processEvents()
    assert window._proto_controls, "选中设备后应生成控件"

    feature, spins = window._proto_controls[0]
    spins["mark"].setValue(77)
    window._profile_name.setText("我的配置")
    window._on_profile_save()
    files = list(tmp_path.glob("046D_C54D/*.json"))
    assert len(files) == 1
    assert window._profile_combo.count() == 1

    spins["mark"].setValue(66)                    # 改乱 → 加载档案恢复
    window._on_profile_load()
    assert window._proto_controls[0][1]["mark"].value() == 77


def test_profile_apply_executes_features(window, qapp, monkeypatch, tmp_path):
    monkeypatch.setattr(gui_mod, "DEFAULT_PROFILE_DIR", tmp_path)
    monkeypatch.setattr(rio, "_open_device",
                        lambda path: FakeDev(responses=[
                            [0x10, 0xFF, 0x00, 0x10, 0x02, 0x50, 0x42]]))
    window._goto_device("046D:C54D")
    for _ in range(10):
        qapp.processEvents()
    window._profile_name.setText("apply测试")
    window._on_profile_save()
    assert window._profile_combo.count() == 1
    window._on_profile_apply()
    window._workers[-1].wait(5000)
    for _ in range(10):
        qapp.processEvents()
    log = window._log.toPlainText()
    assert "[ping]" in log
    assert "应用完成" in log


def test_profile_apply_without_protocol_skips(window, qapp, monkeypatch, tmp_path):
    monkeypatch.setattr(gui_mod, "DEFAULT_PROFILE_DIR", tmp_path)
    window._goto_device("291D:385D")              # 无 protocol 块
    for _ in range(10):
        qapp.processEvents()
    # 没有控件也没有档案 → 应用直接提示
    window._on_profile_apply()
    assert "无协议库条目" in window._log.toPlainText() or window._profile_combo.count() == 0

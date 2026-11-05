"""M5 多设备测试支持：core/probe.py 单元测试。"""
import core.report_io as rio
from core.descriptor import parse_report_descriptor
from core.probe import (
    ProbeResult,
    feature_report_ids,
    probe_interface,
    probe_vendor_interfaces,
    suspect_rawhid_interfaces,
)
from tests.test_device_manager import fake_raw
from tests.test_report_io import FakeDev


# KBD_VENDOR_DESC 同 test_descriptor：含 feature report id=3 的厂商接口
KBD_VENDOR_DESC = bytes.fromhex(
    "05 01 09 06 A1 01"
    "85 01"
    "05 07 19 E0 29 E7 15 00 25 01 75 01 95 08 81 02"
    "95 01 75 08 81 03"
    "05 08 19 01 29 05 15 00 25 01 75 01 95 05 91 02"
    "95 01 75 03 91 01"
    "C0"
    "05 0C 09 01 A1 01"
    "85 02"
    "0A 23 02 15 00 25 01 75 01 95 01 81 02"
    "95 07 75 01 81 03"
    "C0"
    "06 00 FF 09 01 A1 01"
    "85 03"
    "09 01 15 00 26 FF 00 75 08 95 08 B1 02"
    "C0"
)


def test_feature_report_ids():
    info = parse_report_descriptor(KBD_VENDOR_DESC)
    assert feature_report_ids(info) == [3]


def test_feature_report_ids_falls_back_to_zero():
    mouse = bytes.fromhex(
        "05 01 09 02 A1 01 09 01 A1 00 05 09 19 01 29 03 15 00 25 01"
        "95 03 75 01 81 02 95 01 75 05 81 01 05 01 09 30 09 31 15 81 25 7F"
        "75 08 95 02 81 06 C0 C0")
    assert feature_report_ids(parse_report_descriptor(mouse)) == [0]


def test_probe_interface_reads_features(monkeypatch):
    dev = FakeDev()
    dev.feature_reads = [
        [0x03] + list(range(0x10, 0x20)) + [0x00] * 49,   # id=3 → 64 字节
    ]
    monkeypatch.setattr(rio, "_open_device", lambda path: dev)
    probe = probe_interface(b"\\fake", 0xFF00, 0x0001,
                            lambda path: KBD_VENDOR_DESC)
    assert probe.descriptor_len == len(KBD_VENDOR_DESC)
    assert probe.feature_ids == [3]
    assert probe.reads[3] is not None
    assert probe.reads[3][0] == 0x03
    assert probe.error is None


def test_probe_interface_descriptor_failure(monkeypatch):
    monkeypatch.setattr(rio, "_open_device", lambda path: FakeDev())
    probe = probe_interface(b"\\fake", 0xFF21, 0x0001, lambda path: None)
    assert probe.error is not None
    assert probe.feature_ids == []


def test_probe_vendor_interfaces_filters_standard_pages(monkeypatch):
    called = []

    def fake_read(path):
        called.append(path)
        return KBD_VENDOR_DESC

    monkeypatch.setattr(rio, "_open_device", lambda path: FakeDev())
    ifaces = [
        dm_from_raw(fake_raw("a", usage_page=0x0001, usage=0x0006)),   # 标准键盘接口 → 跳过
        dm_from_raw(fake_raw("b", usage_page=0xFF21, usage=0x0001)),   # vendor → 探测
    ]
    results = probe_vendor_interfaces(ifaces, fake_read)
    assert len(results) == 1
    assert results[0].usage_page == 0xFF21
    assert called == ["b"]                    # DeviceInfo.path 已解码为 str


def dm_from_raw(raw):
    import core.device_manager as dm
    return dm.DeviceInfo.from_hid_dict(raw)


# rawhid 风格描述符：usage 0 + 64 字节 input + 64 字节 output（37 字节，Ace68-II IF1 同款）
RAWHID_DESC = bytes.fromhex(
    "05 01 09 00 A1 01"
    "09 01 15 00 26 FF 00 75 08 95 40 81 02"
    "09 01 15 00 26 FF 00 75 08 95 40 91 02"
    "C0"
)


def test_suspect_rawhid_interfaces(monkeypatch):
    monkeypatch.setattr(rio, "_open_device", lambda path: FakeDev())
    ifaces = [
        dm_from_raw(fake_raw("kbd", usage_page=0x0001, usage=0x0006)),   # 标准键盘 → 跳过
        dm_from_raw(fake_raw("raw", usage_page=0x0001, usage=0x0000)),   # rawhid → 命中
        dm_from_raw(fake_raw("ven", usage_page=0xFF21, usage=0x0001)),   # vendor 页 → 非 rawhid
    ]
    results = suspect_rawhid_interfaces(ifaces, lambda path: RAWHID_DESC)
    assert len(results) == 1
    assert results[0].usage == 0x0000
    assert "rawhid" in (results[0].note or "")
    assert "input_passive" in results[0].reads


def test_suspect_rawhid_rejects_wrong_shape(monkeypatch):
    monkeypatch.setattr(rio, "_open_device", lambda path: FakeDev())
    # usage 0 但描述符是 8 字节小报文 → 不算 rawhid
    small = bytes.fromhex(
        "05 01 09 00 A1 01 09 01 15 00 26 FF 00 75 08 95 02 81 02 C0")
    ifaces = [dm_from_raw(fake_raw("raw", usage_page=0x0001, usage=0x0000))]
    assert suspect_rawhid_interfaces(ifaces, lambda path: small) == []


def test_probe_result_defaults():
    r = ProbeResult(usage_page=0xFF00, usage=1)
    assert r.reads == {} and r.error is None

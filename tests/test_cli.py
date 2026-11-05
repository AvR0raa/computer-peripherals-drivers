"""CLI 组合层测试：参数解析与 list --json 输出（枚举 monkeypatch 隔离）。"""
import json

import pytest

import cli
import core.device_manager as dm
from tests.test_device_manager import fake_raw


def test_parse_vidpid_formats():
    assert cli.parse_vidpid("046D:C54D") == (0x046D, 0xC54D)
    assert cli.parse_vidpid("0x291d:0x385d") == (0x291D, 0x385D)
    with pytest.raises(ValueError):
        cli.parse_vidpid("046D")
    with pytest.raises(ValueError):
        cli.parse_vidpid("zzzz:1234")


def test_usage_short_marks_vendor():
    assert cli.usage_short(0x0001, 0x0080) == "0001:0080 "
    assert cli.usage_short(0xFF21, 0x0001) == "FF21:0001V"


def test_list_json(monkeypatch, capsys):
    monkeypatch.setattr(dm.hid, "enumerate", lambda: [
        fake_raw("a"),                       # 046D:C54D 在真协议库中 -> known
        fake_raw("b", vid=0xDEAD, pid=0xBEEF, product="Mystery Box"),
    ])
    code = cli.main(["list", "--json"])
    assert code == 0
    payload = json.loads(capsys.readouterr().out)
    assert payload["count"] == 2
    by_vid = {d["vid"]: d for d in payload["devices"]}
    assert by_vid["046D"]["status"] == "known"
    assert by_vid["DEAD"]["status"] == "unknown"
    assert by_vid["DEAD"]["product"] == "Mystery Box"


def test_list_table(monkeypatch, capsys):
    monkeypatch.setattr(dm.hid, "enumerate", lambda: [fake_raw("a")])
    code = cli.main(["list"])
    assert code == 0
    out = capsys.readouterr().out
    assert "046D:C54D" in out
    assert "USB Receiver" in out


def test_info_unknown_device_exit_code(monkeypatch, capsys):
    monkeypatch.setattr(dm.hid, "enumerate", lambda: [fake_raw("a")])
    code = cli.main(["info", "1234:5678"])
    assert code == 1
    assert "未找到" in capsys.readouterr().err


def test_info_bad_vidpid_exit_code(capsys):
    assert cli.main(["info", "garbage"]) == 1
    assert "VID:PID" in capsys.readouterr().err

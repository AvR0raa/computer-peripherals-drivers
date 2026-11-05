"""指纹匹配：VID/PID + 品牌表 → 设备状态标注（任务 1.3）。

状态三分类（对应 plan.md 验收标准）：
  KNOWN   —— 协议库有该 VID:PID 条目，M2 可走直通引擎
  SDK     —— 品牌 VID 已知且品牌有官方 SDK，走 SDK 桥路线
  UNKNOWN —— 可进入学习模式（品牌已知时会附带品牌名）
"""
from __future__ import annotations

import json
import re
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

DEFAULT_DB_PATH = Path(__file__).resolve().parent.parent / "protocol_db" / "db.json"


class ProtocolDBError(RuntimeError):
    """协议库文件缺失或结构不合法。"""


_PLACEHOLDER_RE = re.compile(r"^\{[A-Za-z_]\w*\}$")
# 计算型占位符 {#negsum}（校验和）；与 report_io 的定义保持一致（模块间禁止互相 import）
_COMPUTED_RE = re.compile(r"^\{#([a-z_]+)\}$")
_COMPUTED_ALGOS = {"negsum"}
_VALID_KINDS = {"command", "read", "write"}
_VALID_REPORTS = {"output", "feature"}


def _is_byte(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and 0 <= value <= 255


def _validate_protocol(vid_pid: str, protocol) -> None:
    """校验 schema v2 的 protocol 块，非法抛 ProtocolDBError。"""
    where = f"devices[{vid_pid}].protocol"
    if not isinstance(protocol, dict):
        raise ProtocolDBError(f"{where}: 必须是 object")
    for key in ("name", "transport", "features"):
        if key not in protocol:
            raise ProtocolDBError(f"{where}: 缺少 {key}")
    if not isinstance(protocol["name"], str):
        raise ProtocolDBError(f"{where}.name: 必须是字符串")
    features = protocol["features"]
    if not isinstance(features, list) or not features:
        raise ProtocolDBError(f"{where}.features: 必须是非空数组")

    transport = protocol["transport"]
    if not isinstance(transport, dict):
        raise ProtocolDBError(f"{where}.transport: 必须是 object")
    match = transport.get("match")
    if not isinstance(match, dict):
        raise ProtocolDBError(f"{where}.transport.match: 必须是 object")
    for key in ("usage_page", "usage"):
        value = match.get(key, "")
        if not (isinstance(value, str) and re.fullmatch(r"[0-9A-Fa-f]{4}", value)):
            raise ProtocolDBError(f"{where}.transport.match.{key}: 须为 4 位十六进制字符串")

    for index, feature in enumerate(features):
        fwhere = f"{where}.features[{index}]({feature.get('id', '?') if isinstance(feature, dict) else '?'})"
        if not isinstance(feature, dict):
            raise ProtocolDBError(f"{fwhere}: 必须是 object")
        for key in ("id", "label", "kind", "report", "template"):
            if key not in feature:
                raise ProtocolDBError(f"{fwhere}: 缺少 {key}")
        if feature["kind"] not in _VALID_KINDS:
            raise ProtocolDBError(f"{fwhere}.kind: 必须是 {_VALID_KINDS} 之一")
        if feature["report"] not in _VALID_REPORTS:
            raise ProtocolDBError(f"{fwhere}.report: 必须是 {_VALID_REPORTS} 之一")
        template = feature["template"]
        if not isinstance(template, list):
            raise ProtocolDBError(f"{fwhere}.template: 必须是数组")
        for element in template:
            valid = (
                _is_byte(element)
                or (isinstance(element, str) and _PLACEHOLDER_RE.match(element))
                or (isinstance(element, str)
                    and (m := _COMPUTED_RE.match(element)) is not None
                    and m.group(1) in _COMPUTED_ALGOS)
            )
            if not valid:
                raise ProtocolDBError(
                    f"{fwhere}.template: 元素须为 0-255 整数、{{参数}} 或 {{#算法}} 占位符，收到 {element!r}")
        if feature["kind"] == "write" and not feature.get("read_from"):
            raise ProtocolDBError(f"{fwhere}: kind=write 必须提供 read_from（读回验证用）")
        _validate_params(fwhere, feature.get("params"))
        if "response" in feature:
            _validate_response(fwhere, feature["response"])


def _validate_params(where: str, params) -> None:
    if params is None:
        return
    if not isinstance(params, dict):
        raise ProtocolDBError(f"{where}.params: 必须是 object")
    for name, spec in params.items():
        if not isinstance(spec, dict) or spec.get("type") != "int":
            raise ProtocolDBError(f"{where}.params.{name}: 仅支持 {{'type': 'int', ...}}")
        for key in ("default", "min", "max"):
            if key in spec and not isinstance(spec[key], int):
                raise ProtocolDBError(f"{where}.params.{name}.{key}: 必须是整数")
        if {"min", "max"} <= spec.keys() and spec["min"] > spec["max"]:
            raise ProtocolDBError(f"{where}.params.{name}: min > max")


def _validate_response(where: str, response) -> None:
    if not isinstance(response, dict):
        raise ProtocolDBError(f"{where}.response: 必须是 object")
    branches = [("response", response)]
    if "alt" in response:
        branches.append(("response.alt", response["alt"]))
    for path, branch in branches:
        if not isinstance(branch, dict):
            raise ProtocolDBError(f"{where}.{path}: 必须是 object")
        if "check" not in branch:
            raise ProtocolDBError(f"{where}.{path}: 缺少 check")
        for element in branch["check"]:
            ok = (
                element is None
                or _is_byte(element)
                or (isinstance(element, str) and _PLACEHOLDER_RE.match(element))
                or (isinstance(element, list) and all(_is_byte(v) for v in element))
            )
            if not ok:
                raise ProtocolDBError(f"{where}.{path}.check: 非法元素 {element!r}")
        for key, index in branch.get("extract", {}).items():
            if not isinstance(index, int) or isinstance(index, bool) or index < 0:
                raise ProtocolDBError(f"{where}.{path}.extract.{key}: 必须是非负整数索引")


class DeviceStatus(str, Enum):
    KNOWN = "known"      # 已知（可直通）
    SDK = "sdk"          # 有官方 SDK
    UNKNOWN = "unknown"  # 未知（可学习）


@dataclass(frozen=True)
class FingerprintResult:
    status: DeviceStatus
    brand: str | None        # 品牌英文名；未识别为 None
    brand_zh: str | None
    sdk: tuple[str, ...]     # 官方 SDK 名称（无则空）
    entry: dict | None       # db["devices"] 命中的完整条目
    detail: str              # 一句话解释


def load_db(path: Path = DEFAULT_DB_PATH) -> dict:
    """读取并校验协议库。结构非法抛 ProtocolDBError。"""
    if not path.is_file():
        raise ProtocolDBError(f"协议库不存在: {path}")
    try:
        db = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ProtocolDBError(f"协议库 JSON 语法错误: {exc}") from exc

    schema = db.get("$schema", "")
    if not schema.startswith("protocol-db/"):
        raise ProtocolDBError(f"不支持的 schema: {schema!r}")
    for key in ("brands", "devices"):
        if not isinstance(db.get(key), dict):
            raise ProtocolDBError(f"协议库缺少 {key} 表或类型不是 object")
    if db["devices"]:
        bad = [k for k in db["devices"] if ":" not in k]
        if bad:
            raise ProtocolDBError(f"devices 键必须是 VID:PID 格式: {bad[:3]}")
        for vid_pid, entry in db["devices"].items():
            if isinstance(entry, dict) and "protocol" in entry:
                _validate_protocol(vid_pid, entry["protocol"])
    return db


def _brand_info(db: dict, vid_hex: str) -> tuple[str | None, str | None, tuple[str, ...]]:
    """查品牌表，返回 (英文名, 中文名, sdk 元组)。"""
    brand = db["brands"].get(vid_hex)
    if not brand:
        return None, None, ()
    return (
        brand.get("name"),
        brand.get("name_zh"),
        tuple(brand.get("sdk", ())),
    )


def fingerprint(device, db: dict) -> FingerprintResult:
    """对单个 DeviceInfo 标注状态。device 接受任何有 vid_hex/pid_hex 属性的对象。"""
    vid_hex, pid_hex = device.vid_hex, device.pid_hex
    key = f"{vid_hex}:{pid_hex}"

    entry = db["devices"].get(key)
    name, name_zh, sdk = _brand_info(db, vid_hex)

    if entry is not None:
        return FingerprintResult(
            status=DeviceStatus.KNOWN,
            brand=name,
            brand_zh=name_zh,
            sdk=sdk,
            entry=entry,
            detail=f"{vid_hex}:{pid_hex} 在协议库中（可直通，验证状态: "
                   f"{'已验证' if entry.get('verified') else '未验证'}）",
        )

    if name is None:
        return FingerprintResult(
            status=DeviceStatus.UNKNOWN,
            brand=None,
            brand_zh=None,
            sdk=(),
            entry=None,
            detail=f"{vid_hex}:{pid_hex} 完全未知，可进入学习模式",
        )

    if sdk:
        return FingerprintResult(
            status=DeviceStatus.SDK,
            brand=name,
            brand_zh=name_zh,
            sdk=sdk,
            entry=None,
            detail=f"品牌 {name_zh or name} 已知且有官方 SDK（{'/'.join(sdk)}），"
                   f"该具体型号暂无协议条目",
        )

    return FingerprintResult(
        status=DeviceStatus.UNKNOWN,
        brand=name,
        brand_zh=name_zh,
        sdk=(),
        entry=None,
        detail=f"品牌 {name_zh or name} 已知，该型号暂无协议条目（可学习）",
    )

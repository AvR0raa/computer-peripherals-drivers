"""指纹匹配：VID/PID + 品牌表 → 设备状态标注（任务 1.3）。

状态三分类（对应 plan.md 验收标准）：
  KNOWN   —— 协议库有该 VID:PID 条目，M2 可走直通引擎
  SDK     —— 品牌 VID 已知且品牌有官方 SDK，走 SDK 桥路线
  UNKNOWN —— 可进入学习模式（品牌已知时会附带品牌名）
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

DEFAULT_DB_PATH = Path(__file__).resolve().parent.parent / "protocol_db" / "db.json"


class ProtocolDBError(RuntimeError):
    """协议库文件缺失或结构不合法。"""


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

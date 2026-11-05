"""配置档案（任务 3.5）：per-device 保存/加载/应用参数集。

档案 = 某台设备 (VID:PID) 各协议功能参数值的快照（JSON 文件）。
用途：同一型号两台设备互拷配置、改坏了一键恢复。
档案只存"用户想要什么值"，应用时仍走 execute_feature/safe_write 的安全路径。
"""
from __future__ import annotations

import json
import re
import time
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_PROFILE_DIR = Path(__file__).resolve().parent.parent / "profiles"

_SCHEMA = "device-profile/v1"


class ProfileError(ValueError):
    """档案文件不合法。"""


@dataclass
class ProfileFeature:
    feature_id: str
    params: dict[str, int] = field(default_factory=dict)


@dataclass
class DeviceProfile:
    vidpid: str            # "291D:385D"
    name: str
    created_ts: float = field(default_factory=time.time)
    features: list[ProfileFeature] = field(default_factory=list)


def _safe_name(name: str) -> str:
    cleaned = re.sub(r'[\\/:*?"<>|]', "_", name).strip()
    return cleaned or "unnamed"


def profile_dir(vidpid: str, directory: Path = DEFAULT_PROFILE_DIR) -> Path:
    return Path(directory) / vidpid.replace(":", "_")


def save_profile(profile: DeviceProfile, directory: Path = DEFAULT_PROFILE_DIR) -> Path:
    """保存档案 → <dir>/<vidpid>/<name>.json。返回文件路径。"""
    target = profile_dir(profile.vidpid, directory) / f"{_safe_name(profile.name)}.json"
    target.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "$schema": _SCHEMA,
        "vidpid": profile.vidpid,
        "name": profile.name,
        "created_ts": profile.created_ts,
        "features": [
            {"id": f.feature_id, "params": dict(f.params)} for f in profile.features
        ],
    }
    target.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
                      encoding="utf-8")
    return target


def load_profile(path: Path) -> DeviceProfile:
    """读取档案；结构非法抛 ProfileError。"""
    try:
        data = json.loads(Path(path).read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ProfileError(f"档案 JSON 语法错误: {exc}") from exc
    if data.get("$schema") != _SCHEMA:
        raise ProfileError(f"不支持的档案 schema: {data.get('$schema')!r}")
    if not isinstance(data.get("vidpid"), str) or ":" not in data["vidpid"]:
        raise ProfileError("档案缺少合法 vidpid")
    features = []
    for f in data.get("features", []):
        params = {k: int(v) for k, v in (f.get("params") or {}).items()}
        features.append(ProfileFeature(str(f.get("id")), params))
    return DeviceProfile(
        vidpid=data["vidpid"],
        name=str(data.get("name") or Path(path).stem),
        created_ts=float(data.get("created_ts") or 0),
        features=features,
    )


def list_profiles(vidpid: str, directory: Path = DEFAULT_PROFILE_DIR) -> list[Path]:
    """某设备的全部档案文件（按修改时间倒序）。"""
    base = profile_dir(vidpid, directory)
    if not base.is_dir():
        return []
    return sorted(base.glob("*.json"), key=lambda p: p.stat().st_mtime, reverse=True)

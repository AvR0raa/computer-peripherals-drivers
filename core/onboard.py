"""板载配置档管理（0x8100，Solaar OnboardProfiles 平移）。

背景：PRO X2 等新款罗技鼠标不支持主机侧按键重映射（REPROG 0x1B00/0x1B04
不在 feature 表，真机确认）——按键自定义由**板载配置档**承载：每个配置档
包含一套按键映射/DPI/报告率，切换配置档 = 切换整套按键行为。
（G HUB 编辑配置档内容；本模块负责枚举与切换。）

布局（真机实证 PRO X2, 2026-11-14）：
  funct 0x00（长报文）→ [memory, current_profile, macro, count, oob, buttons,
                          sectors, size, ff]（9-16 字节）
  funct 0x50（长报文）→ 分页读 profile headers，每项 4 字节 [sectorHi, sectorLo,
                          enabled, x]；前 4 字节全 0/全 F 表示需切 ROM 区（param[0]=1）
  funct 0x20          → 板载启用标志（0x01=onboard，0x00=host）
  funct 0x10 [01/02]  → 开启板载 / 切 Host 模式（见 hidpp.set_onboard_mode）
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

from core.hidpp import (
    FEATURE_ONBOARD_PROFILES,
    HidppLink,
    _call_feature,
    get_feature_index,
    read_onboard_mode,
)


@dataclass
class ProfileHeader:
    sector: int
    enabled: bool


@dataclass
class OnboardInfo:
    mode: str = "unknown"            # "host" / "onboard" / "unknown"
    current_profile: int | None = None
    macro: int | None = None
    profile_count: int | None = None
    buttons: int | None = None       # 可自定义按键数
    headers: list[ProfileHeader] = field(default_factory=list)
    raw: str = ""

    @property
    def supported(self) -> bool:
        return self.profile_count is not None


def read_onboard_info(link: HidppLink, dev: int, timeout_ms: int = 600) -> OnboardInfo:
    """读取板载配置档全貌：模式 / 当前档 / 档数 / 按键数 / headers。"""
    info = OnboardInfo(mode=read_onboard_mode(link, dev, timeout_ms))
    idx = get_feature_index(link, dev, FEATURE_ONBOARD_PROFILES, timeout_ms)
    if not idx:
        return info
    resp = _call_feature(link, dev, idx, 0x00, long=True, timeout_ms=timeout_ms)
    if resp is None:
        return info
    info.raw = resp.hex(" ")
    if len(resp) >= 3:
        info.current_profile = resp[1]
        info.macro = resp[2]
    if len(resp) >= 6:
        info.profile_count = resp[3]
        info.buttons = resp[5]

    # headers 分页（最多扫 8 项防失控）
    chunk = _call_feature(link, dev, idx, 0x50,
                          bytes([0x00, 0x00, 0x00, 0x00]), timeout_ms, long=True)
    s_flag = 0x00
    if chunk and chunk[0:4] in (b"\x00\x00\x00\x00", b"\xff\xff\xff\xff"):
        chunk = _call_feature(link, dev, idx, 0x50,
                              bytes([0x01, 0x00, 0x00, 0x00]), timeout_ms, long=True)
        s_flag = 0x01
    i = 0
    limit = (info.profile_count or 8) + 2
    while chunk and chunk[0:2] != b"\xff\xff" and i < limit:
        sector, enabled = struct.unpack("!HB", chunk[0:3])
        info.headers.append(ProfileHeader(sector=sector, enabled=bool(enabled)))
        i += 1
        chunk = _call_feature(link, dev, idx, 0x50,
                              bytes([s_flag, 0x00, 0x00, (i * 4) & 0xFF]),
                              timeout_ms, long=True)
    return info

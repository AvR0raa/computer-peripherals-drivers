"""vendor 配置通道只读探测（M5 多设备测试支持）。

安全边界：只发 get_feature_report（读取），绝不写入。
对未知/未适配设备的 vendor 自定义接口，读出描述符里声明的 feature
report 原始内容 —— 这就是配置通道的快照，也是 M4 学习模式的输入。
"""
from __future__ import annotations

from dataclasses import dataclass, field

from core.descriptor import DescriptorInfo, parse_report_descriptor
from core.report_io import ReportIO


def feature_report_ids(info: DescriptorInfo) -> list[int]:
    """描述符里声明的 feature report ID（去重保序；无 feature 字段时返回 [0]）。"""
    ids: list[int] = []
    for f in info.fields:
        if f.report_type == "feature" and not f.is_constant and f.report_id not in ids:
            ids.append(f.report_id)
    return ids or [0]


@dataclass
class ProbeResult:
    usage_page: int
    usage: int
    descriptor_len: int | None = None   # 描述符读取失败时为 None
    feature_ids: list[int] = field(default_factory=list)
    reads: dict = field(default_factory=dict)   # report_id -> bytes | None
    error: str | None = None
    note: str | None = None


def probe_interface(path, usage_page: int, usage: int, read_descriptor) -> ProbeResult:
    """探测单个接口：读描述符 → 找 feature report ID → 逐个读原始内容。"""
    result = ProbeResult(usage_page=usage_page, usage=usage)
    raw = read_descriptor(path)
    if raw is None:
        result.error = "描述符读取失败（设备被系统占用或接口不可读）"
        return result
    result.descriptor_len = len(raw)
    info = parse_report_descriptor(raw)
    result.feature_ids = feature_report_ids(info)

    io = ReportIO(path)
    with io:
        for report_id in result.feature_ids:
            try:
                result.reads[report_id] = io.get_feature(report_id, 64)
            except Exception:
                result.reads[report_id] = None
    return result


def probe_vendor_interfaces(ifaces, read_descriptor) -> list[ProbeResult]:
    """只挑 vendor 自定义页（>=0xFF00）的接口做探测；没有则空列表。"""
    return [
        probe_interface(d.path, d.usage_page, d.usage, read_descriptor)
        for d in ifaces
        if d.usage_page >= 0xFF00
    ]


def suspect_rawhid_interfaces(ifaces, read_descriptor) -> list[ProbeResult]:
    """识别 rawhid 风格配置通道：usage=0x0000 且带 64 字节 input+output。

    国产键盘（ROYUAN/迈从/美嘉獅等 OEM 方案）最常见的配置接口形态。
    只做被动 input 读取（零风险），不写入任何报文。
    """
    results: list[ProbeResult] = []
    for d in ifaces:
        if d.usage_page != 0x0001 or d.usage != 0x0000:
            continue
        raw = read_descriptor(d.path)
        if raw is None:
            continue
        info = parse_report_descriptor(raw)
        has_input = any(
            f.report_type == "input" and not f.is_constant and f.size * f.count >= 512
            for f in info.fields)
        has_output = any(
            f.report_type == "output" and not f.is_constant and f.size * f.count >= 512
            for f in info.fields)
        if not (has_input and has_output):
            continue
        io = ReportIO(d.path)
        with io:
            pushed = io.read_input(200)   # 被动抓 200ms 内设备主动推送
        results.append(ProbeResult(
            usage_page=d.usage_page, usage=d.usage,
            descriptor_len=len(raw),
            reads={"input_passive": pushed},
            note="rawhid 疑似配置通道（64字节 in/out，写入能力待 M4 学习模式验证）",
        ))
    return results

"""写命令安全机制（任务 2.4）。

原则（plan.md 风险表）：全部写命令走"读旧值 → 写新值 → 读回验证"，
验证失败且手上有旧值时立即回滚旧值再读回；全程记录完整 hex 日志。

两条路径：
  report == "feature" : send_feature + get_feature 自读自验（无需 read 功能）
  report == "output"  : 需配对 read 功能，用 execute_feature 读前后值
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field

from core.report_io import ReportIO, execute_feature, render_template


@dataclass
class WriteOutcome:
    ok: bool                    # 写流程完成（回滚成功也算 ok=False？——见下）
    verified: bool              # 读回与 expect 一致；无 expect 时恒为 False（不可验证）
    wrote: bytes | None
    readback: bytes | None
    old_value: bytes | None
    rolled_back: bool
    log: list[str] = field(default_factory=list)


def _log(out: WriteOutcome, message: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {message}"
    out.log.append(line)


def _merged_params(feature: dict, params: dict | None) -> dict:
    """feature params 定义 + 调用者覆盖 → 完整参数（缺省回填 default）。"""
    merged = dict(params or {})
    for name, spec in feature.get("params", {}).items():
        merged.setdefault(name, spec.get("default", 0))
    return merged


def _read_via_feature(io: ReportIO, read_feature: dict, params: dict, out, timeout_ms):
    result = execute_feature(io, read_feature, params, timeout_ms)
    _log(out, f"读({read_feature.get('id')}): {result.response.hex(' ') if result.response else '<无响应>'}")
    return result.response


def _expect_ok(readback: bytes | None, expect: dict) -> bool:
    if readback is None:
        return False
    return all(index < len(readback) and readback[index] == byte
               for index, byte in expect.items())


def safe_write(
    io: ReportIO,
    write_feature: dict,
    read_feature: dict | None = None,
    new_params: dict | None = None,
    expect: dict | None = None,
    rollback_params: dict | None = None,
    timeout_ms: int = 600,
) -> WriteOutcome:
    """执行一次带安全网的写。

    expect: {字节索引: 期望值}，读回比对用；给了 expect 才具备回滚触发条件
    （没有期望值就无法判断写错，只能记录"已写入但未验证"）。
    rollback_params: output 路径回滚时的旧值参数（feature 路径直接回写旧字节流）。
    """
    out = WriteOutcome(ok=False, verified=False, wrote=None,
                       readback=None, old_value=None, rolled_back=False)

    if write_feature.get("kind") != "write":
        _log(out, f"拒绝: feature kind={write_feature.get('kind')!r} 不是 write（安全机制只放行 write）")
        return out
    if write_feature.get("report") != "feature" and read_feature is None:
        _log(out, "拒绝: output 报文写入必须配对 read 功能（无法读回验证）")
        return out

    # ---- 1. 读旧值 ----
    if write_feature["report"] == "feature":
        new_params = _merged_params(write_feature, new_params)
        try:
            data = render_template(write_feature["template"], new_params)
        except Exception as exc:
            _log(out, f"拒绝: 模板渲染失败 {exc}")
            return out
        report_id, size = data[0], len(data)
        out.old_value = io.get_feature(report_id, size)
        _log(out, f"读旧值(feature id={report_id:#04x}): "
                  f"{out.old_value.hex(' ') if out.old_value else '<无响应>'}")
    else:
        read_params = _merged_params(read_feature, {
            k: v for k, v in (new_params or {}).items() if k in read_feature.get("params", {})
        })
        out.old_value = _read_via_feature(io, read_feature, read_params, out, timeout_ms)

    # ---- 2. 写新值 ----
    try:
        data = render_template(write_feature["template"], _merged_params(write_feature, new_params))
    except Exception as exc:
        _log(out, f"中止: 新值模板渲染失败 {exc}")
        return out
    out.wrote = data
    if write_feature["report"] == "feature":
        io.send_feature(data)
        _log(out, f"写(feature): {data.hex(' ')}")
    else:
        io.write_output(data)
        _log(out, f"写(output): {data.hex(' ')}")

    # ---- 3. 读回验证 ----
    if write_feature["report"] == "feature":
        out.readback = io.get_feature(report_id, size)
        _log(out, f"读回(feature id={report_id:#04x}): "
                  f"{out.readback.hex(' ') if out.readback else '<无响应>'}")
    else:
        out.readback = _read_via_feature(io, read_feature, read_params, out, timeout_ms)

    if expect is not None:
        out.verified = _expect_ok(out.readback, expect)
        _log(out, f"验证: {'通过' if out.verified else '失败'} "
                  f"(期望 {({i: f'{b:02X}' for i, b in expect.items()})})")
        if not out.verified and out.old_value is not None:
            # ---- 4. 回滚旧值 ----
            if write_feature["report"] == "feature":
                _log(out, f"回滚: 重写旧值 {out.old_value.hex(' ')}")
                io.send_feature(out.old_value)
                confirm = io.get_feature(report_id, size)
                out.rolled_back = confirm == out.old_value
            elif rollback_params is not None:
                try:
                    old_data = render_template(
                        write_feature["template"], _merged_params(write_feature, rollback_params))
                except Exception as exc:
                    old_data = None
                    _log(out, f"回滚失败: 旧值模板渲染失败 {exc}")
                    confirm = None
                if old_data is not None:
                    _log(out, f"回滚: 重写旧值 {old_data.hex(' ')}")
                    io.write_output(old_data)
                    confirm = _read_via_feature(io, read_feature, read_params, out, timeout_ms)
                    out.rolled_back = confirm is not None
            else:
                confirm = None
                _log(out, "回滚失败: output 路径未提供 rollback_params（无法构造旧值报文）")
            _log(out, f"回滚{'成功' if out.rolled_back else '失败'}: "
                      f"读回 {confirm.hex(' ') if confirm else '<无响应>'}")
        elif not out.verified:
            _log(out, "无旧值可回滚（读旧值阶段未取得响应）")
    else:
        _log(out, "未提供 expect：已写入但无法自动验证（M2 只读原则下属正常调试流程）")

    out.ok = out.readback is not None and (expect is None or out.verified)
    return out

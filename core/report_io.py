"""HID report 模板渲染 + 读写器（任务 2.2）。

纯函数 render_template / match_response 可离线测试；
ReportIO 包装 hidapi，测试通过 monkeypatch _open_device 注入假设备。
"""
from __future__ import annotations

import re
import time
from dataclasses import dataclass, field

import hid


class RenderError(ValueError):
    """模板或参数不合法。"""


class ReportError(RuntimeError):
    """hidapi 打开/读写失败。"""


_PLACEHOLDER_RE = re.compile(r"^\{([A-Za-z_]\w*)\}$")


# ---------------------------------------------------------------------------
# 纯函数
# ---------------------------------------------------------------------------

def _resolve_byte(element, params: dict, what: str) -> int:
    """模板元素 → 一个字节。int 直接用；"{param}" 从 params 解析。"""
    if isinstance(element, str):
        match = _PLACEHOLDER_RE.match(element)
        if not match:
            raise RenderError(f"{what}: 非法占位符 {element!r}")
        name = match.group(1)
        if name not in params:
            raise RenderError(f"{what}: 参数 {name!r} 未提供")
        value = params[name]
    else:
        value = element
    if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= 255:
        raise RenderError(f"{what}: {value!r} 不是合法字节 (0-255)")
    return value


def render_template(template: list, params: dict) -> bytes:
    """模板 + 参数 → 字节流。例如 [16,"{dev}"] + {"dev":255} -> b'\\x10\\xff'。"""
    return bytes(
        _resolve_byte(element, params, f"template[{i}]")
        for i, element in enumerate(template)
    )


def match_response(response: bytes, check: list, params: dict | None = None) -> bool:
    """响应字节流是否满足 check：int 精确 | None 通配 | "{param}" | [v1,v2..] 任一。"""
    params = params or {}
    if len(response) < len(check):
        return False
    for i, want in enumerate(check):
        if want is None:
            continue
        if isinstance(want, str):
            want = _resolve_byte(want, params, f"check[{i}]")
        actual = response[i]
        if isinstance(want, list):
            if actual not in want:
                return False
        elif actual != want:
            return False
    return True


def extract_fields(response: bytes, extract: dict) -> dict:
    """按 extract 索引表从响应取字段: {"major": 4} -> {"major": response[4]}。"""
    result = {}
    for key, index in extract.items():
        result[key] = response[index] if index < len(response) else None
    return result


# ---------------------------------------------------------------------------
# ReportIO
# ---------------------------------------------------------------------------

@dataclass
class HistoryEntry:
    ts: float
    direction: str      # "tx" | "rx"
    data: bytes


@dataclass
class ReportIO:
    """单个 HID 接口的读写句柄。惰性打开，用完 close()。"""

    path: bytes | str
    history: list[HistoryEntry] = field(default_factory=list, init=False)
    on_history: "callable | None" = None   # GUI 订阅: fn(HistoryEntry)
    _dev: object = field(default=None, init=False, repr=False)

    def open(self) -> None:
        if self._dev is not None:
            return
        path = self.path.encode("utf-8") if isinstance(self.path, str) else self.path
        try:
            self._dev = _open_device(path)
        except Exception as exc:
            raise ReportError(f"打开设备失败 ({path!r}): {exc}") from exc

    def close(self) -> None:
        if self._dev is not None:
            try:
                self._dev.close()
            finally:
                self._dev = None

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *exc):
        self.close()

    # ---- 日志 ----

    def _log(self, direction: str, data: bytes) -> None:
        entry = HistoryEntry(time.time(), direction, bytes(data))
        self.history.append(entry)
        if self.on_history is not None:
            try:
                self.on_history(entry)
            except Exception:
                pass  # 订阅方异常不影响设备通信

    # ---- 基础读写 ----

    def write_output(self, data: bytes) -> int:
        self.open()
        written = self._dev.write(list(data))
        if written < 0:
            raise ReportError(f"write 失败: {written}")
        self._log("tx", data)
        return written

    def read_input(self, timeout_ms: int = 500, size: int = 64) -> bytes | None:
        self.open()
        raw = self._dev.read(size, timeout_ms)
        if not raw:
            return None
        data = bytes(raw)
        self._log("rx", data)
        return data

    def send_feature(self, data: bytes) -> None:
        self.open()
        result = self._dev.send_feature_report(list(data))
        if result < 0:
            raise ReportError(f"send_feature_report 失败: {result}")
        self._log("tx", data)

    def get_feature(self, report_id: int = 0, size: int = 64) -> bytes | None:
        self.open()
        raw = self._dev.get_feature_report(report_id, size)
        if not raw:
            return None
        data = bytes(raw)
        self._log("rx", data)
        return data

    # ---- 组合：写并等响应 ----

    def call(
        self,
        data: bytes,
        check: list | None = None,
        params: dict | None = None,
        timeout_ms: int = 500,
    ) -> bytes | None:
        """写 output report，然后轮询读 input 直到出现匹配 check 的响应。

        返回第一个匹配的响应；check=None 时返回第一个收到的响应；
        超时返回 None。设备主动推送的其他报文（如鼠标移动）被忽略并留在日志里。
        """
        params = params or {}
        self.write_output(data)
        deadline = time.monotonic() + timeout_ms / 1000
        while True:
            remaining_ms = int((deadline - time.monotonic()) * 1000)
            if remaining_ms <= 0:
                return None
            response = self.read_input(timeout_ms=min(remaining_ms, 50))
            if response is not None and (check is None or match_response(response, check, params)):
                return response


def _open_device(path: bytes):
    """打开 hidapi 设备。测试用 monkeypatch 替换本函数注入假设备。"""
    dev = hid.device()
    dev.open_path(path)
    return dev

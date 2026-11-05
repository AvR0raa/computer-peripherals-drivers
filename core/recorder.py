"""学习模式录制器（任务 4.1）。

对一个或多个 vendor/rawhid 接口做只读录制：
- 连续 input 报文监听（Windows HID input 对所有打开句柄广播，
  后台录制不影响设备正常使用）
- feature report 快照（录制开始时自动拍一次，改配置前后可手动再拍）

会话文件格式（jsonl，每行一个 JSON 对象）：
  {"ts": 1699999999.123, "source": "FF21", "kind": "input",
   "report_id": 237, "data": "0e ed ..."}

安全边界：只读（read_input / get_feature），绝不写入。
"""
from __future__ import annotations

import json
import threading
import time
from dataclasses import dataclass
from pathlib import Path

from core.report_io import ReportIO

MAX_ENTRIES = 200_000    # 防失控内存增长；超出后录制自动停止并标记 overflowed


@dataclass(frozen=True)
class RecorderTarget:
    """一个被录制的接口。"""

    label: str                          # 人读标签（如 "FF21" / "rawhid"）
    path: str                           # 设备路径（DeviceInfo.path）
    feature_ids: tuple[int, ...] = ()   # 需要做快照的 feature report ID


@dataclass(frozen=True)
class CaptureEntry:
    ts: float
    source: str          # target.label
    kind: str            # "input" | "feature_read" | "error"
    report_id: int       # input 的最佳估计 = data[0]；无 ID 报文即首字节
    data: str            # hex（空格分隔）；error 时为人话文本；无响应快照为 ""


def _hex(data: bytes | None) -> str:
    return data.hex(" ") if data else ""


class Recorder:
    """多接口只读录制器。start() 起线程，stop() 收尾，entries()/save_jsonl() 取数据。"""

    def __init__(
        self,
        targets: list[RecorderTarget],
        read_timeout_ms: int = 50,
        max_entries: int = MAX_ENTRIES,
    ):
        self._targets = list(targets)
        self._read_timeout_ms = read_timeout_ms
        self._max_entries = max_entries
        self._entries: list[CaptureEntry] = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._threads: list[threading.Thread] = []
        self._overflowed = False

    # ---- 生命周期 ----

    def start(self) -> None:
        if self._threads:
            raise RuntimeError("录制已在进行中")
        self._stop.clear()
        for target in self._targets:
            thread = threading.Thread(
                target=self._run_target, args=(target,), daemon=True)
            thread.start()
            self._threads.append(thread)

    def stop(self, timeout: float = 3.0) -> None:
        self._stop.set()
        for thread in self._threads:
            thread.join(timeout)
        self._threads.clear()

    @property
    def is_recording(self) -> bool:
        return bool(self._threads) and not self._stop.is_set()

    @property
    def overflowed(self) -> bool:
        return self._overflowed

    # ---- 数据访问 ----

    def entries(self) -> list[CaptureEntry]:
        with self._lock:
            return list(self._entries)

    def _add(self, entry: CaptureEntry) -> bool:
        """追加一条记录；超出上限时置溢出并请求停止。返回是否写入成功。"""
        with self._lock:
            if len(self._entries) >= self._max_entries:
                self._overflowed = True
                self._stop.set()
                return False
            self._entries.append(entry)
            return True

    # ---- 线程体 ----

    def _run_target(self, target: RecorderTarget) -> None:
        try:
            io = ReportIO(target.path)
            with io:
                for fid in target.feature_ids:
                    try:
                        data = io.get_feature(fid, 64)
                    except Exception as exc:
                        if not self._add(CaptureEntry(
                                time.time(), target.label, "error", fid,
                                f"feature 读取异常: {exc}")):
                            return
                        continue
                    if not self._add(CaptureEntry(
                            time.time(), target.label, "feature_read", fid, _hex(data))):
                        return
                while not self._stop.is_set():
                    try:
                        data = io.read_input(self._read_timeout_ms)
                    except Exception as exc:
                        self._add(CaptureEntry(
                            time.time(), target.label, "error", 0,
                            f"input 读取异常（目标终止）: {exc}"))
                        return   # 读取异常视为该目标死亡，终止此线程
                    if data is not None:
                        if not self._add(CaptureEntry(
                                time.time(), target.label, "input", data[0], _hex(data))):
                            return
        except Exception as exc:
            self._add(CaptureEntry(
                time.time(), target.label, "error", 0, f"打开失败: {exc}"))

    # ---- 手动快照（改配置前后各拍一次，供差分） ----

    def snapshot_features(self) -> int:
        """对所有目标的 feature_ids 各读一次并记录。返回实际记录条数。"""
        count = 0
        for target in self._targets:
            if not target.feature_ids:
                continue
            try:
                io = ReportIO(target.path)
                with io:
                    for fid in target.feature_ids:
                        try:
                            data = io.get_feature(fid, 64)
                        except Exception as exc:
                            if self._add(CaptureEntry(
                                    time.time(), target.label, "error", fid,
                                    f"feature 读取异常: {exc}")):
                                count += 1
                            continue
                        if self._add(CaptureEntry(
                                time.time(), target.label, "feature_read", fid, _hex(data))):
                            count += 1
            except Exception as exc:
                if self._add(CaptureEntry(
                        time.time(), target.label, "error", 0, f"打开失败: {exc}")):
                    count += 1
        return count

    # ---- 会话文件 ----

    def save_jsonl(self, path: str | Path) -> int:
        """全部记录写 jsonl。返回写入条数。"""
        with self._lock:
            entries = list(self._entries)
        with open(path, "w", encoding="utf-8") as fh:
            for e in entries:
                fh.write(json.dumps({
                    "ts": e.ts, "source": e.source, "kind": e.kind,
                    "report_id": e.report_id, "data": e.data,
                }) + "\n")
        return len(entries)

    @staticmethod
    def load_jsonl(path: str | Path) -> list[CaptureEntry]:
        entries: list[CaptureEntry] = []
        with open(path, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                obj = json.loads(line)
                entries.append(CaptureEntry(
                    obj["ts"], obj["source"], obj["kind"],
                    obj["report_id"], obj["data"]))
        return entries

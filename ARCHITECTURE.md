# ARCHITECTURE.md — M1 设备识别器（冻结基准）

> 本文件是 M1 的架构契约。之后所有开发会话第一件事：读本文件，不得偏离。
> 修改本文件需要明确声明"架构变更"并单独 commit。

## 1. 分层

```
┌─────────────────────────────┐
│  cli.py                     │  命令行入口（list / info / watch）
├─────────────────────────────┤
│  core/                      │
│   ├── device_manager.py     │  设备枚举 + 快照差分热插拔（依赖 hidapi）
│   ├── descriptor.py         │  HID 报告描述符解析（纯逻辑，无 I/O）
│   └── fingerprint.py        │  VID/PID 指纹 → protocol_db 匹配
├─────────────────────────────┤
│  protocol_db/db.json        │  协议库（数据，非代码）
└─────────────────────────────┘
```

依赖方向：`cli → core → hidapi/json`。core 模块之间**禁止互相 import**
（descriptor、device_manager、fingerprint 彼此独立；组合发生在 cli 层）。

## 2. 模块职责与公开接口（冻结）

### core/device_manager.py — 设备枚举与热插拔

```python
@dataclass(frozen=True)
class DeviceInfo:
    path: str              # 平台设备路径（唯一键）
    vendor_id: int         # VID
    product_id: int        # PID
    serial_number: str
    release_number: int    # BCD 版本
    manufacturer: str
    product: str
    usage_page: int
    usage: int
    interface_number: int
    bus_type: int

    # 派生属性: vid_hex / pid_hex -> "046D" 大写十六进制

def list_devices() -> list[DeviceInfo]
    # 包装 hid.enumerate()；hidapi 异常时抛 HIDError

def find_devices(vendor_id: int | None = None,
                 product_id: int | None = None,
                 devices: list[DeviceInfo] | None = None) -> list[DeviceInfo]

@dataclass(frozen=True)
class DeviceEvent:
    kind: str              # "added" | "removed"
    device: DeviceInfo

def diff_snapshots(old: dict[str, DeviceInfo],
                   new: dict[str, DeviceInfo]) -> list[DeviceEvent]
    # 纯函数：老快照有新没有 = removed；反之 = added；按 path 排序

def snapshot() -> dict[str, DeviceInfo]
    # list_devices() 转成 {path: DeviceInfo}

def watch_devices(on_event: Callable[[DeviceEvent], None],
                  interval: float = 1.0,
                  stop: threading.Event | None = None) -> None
    # 阻塞式轮询快照差分（跨平台，不引原生依赖）
```

### core/descriptor.py — 报告描述符解析（纯逻辑，零 I/O，零风险）

```python
@dataclass
class ReportField:
    report_type: str       # "input" | "output" | "feature"
    report_id: int         # 0 = 未使用 Report ID
    offset: int            # 报文体（不含 ID 字节）内 bit 偏移
    size: int              # 每个 item 的 bit 数
    count: int             # item 重复次数（Report Count）
    usage: int | None      # 解析后的 32 位 usage（page<<16|index）；常量字段为 None
    usage_min: int | None  # 数组型字段范围
    usage_max: int | None
    flags: int             # 主条目原始 flags（bit0 Data/Const, bit1 Array/Var, bit2 Abs/Rel）
    is_constant: bool      # True = 填充/常量位

@dataclass
class CollectionNode:
    kind: str              # "Application"/"Physical"/"Logical"/...（未知值保留原始数字字符串）
    usage: int | None      # 32 位 usage
    children: list         # CollectionNode | ReportField

@dataclass
class DescriptorInfo:
    usage_pages: set[int]          # 出现过的 Usage Page
    application_usages: list[int]  # 顶层（Application）集合 usage 列表
    collections: list[CollectionNode]
    fields: list[ReportField]      # 全部字段（含常量）
    has_vendor_page: bool          # usage_pages 含 0xFF00..0xFFFF 厂商自定义页

def parse_report_descriptor(data: bytes) -> DescriptorInfo
    # 解析原始描述符字节；支持短条目（0/1/2/4 字节数据）与长条目（0xFE，跳过）
    # 语法错误：抛 DescriptorError（带出错偏移）

def summary(info: DescriptorInfo) -> list[str]
    # 人读摘要行，如 "Application usage 0x00010002 (Mouse), 3 fields"
```

### core/fingerprint.py — 指纹匹配

```python
class DeviceStatus(str, Enum):
    KNOWN   = "known"    # 协议库有该 VID/PID 条目 → 可直通
    SDK     = "sdk"      # 品牌 VID 已知且品牌有官方 SDK → SDK 桥路线
    UNKNOWN = "unknown"  # 可进入学习模式

@dataclass(frozen=True)
class FingerprintResult:
    status: DeviceStatus
    brand: str | None        # 品牌英文名，未识别为 None
    brand_zh: str | None     # 品牌中文名
    sdk: tuple[str, ...]     # 官方 SDK 名称
    entry: dict | None       # db.devices 命中的完整条目
    detail: str              # 一句话解释

def load_db(path: Path = DEFAULT_DB_PATH) -> dict
    # 读 db.json；缺 schema 键时抛 ProtocolDBError

def fingerprint(device, db: dict) -> FingerprintResult
    # 先精确匹配 devices[f"{vid:04X}:{pid:04X}"] → KNOWN
    # 再查 brands[vid_hex] → 有 sdk 字段则 SDK，否则 KNOWN 品牌但无协议
    # → 实现为：品牌命中但无条目时，有 sdk=SDK，无 sdk=UNKNOWN（brand 仍填）
```

### cli.py — 命令行

```
python cli.py list [--json]            # 全部 HID 接口表 + 指纹状态
python cli.py info <VID:PID> [--desc]  # 单设备各接口详情，--desc 尝试读描述符并解析
python cli.py watch [--interval 1.0]   # 热插拔监视（Ctrl+C 退出）
```

## 3. protocol_db/db.json schema（v1）

```jsonc
{
  "$schema": "protocol-db/v1",
  "brands": {                       // VID 级品牌表
    "046D": { "name": "Logitech", "name_zh": "罗技",
              "sdk": ["GHSDK", "LGS"] }   // sdk 可省略 = 无官方 SDK 路线
  },
  "devices": {                      // 精确 VID:PID 条目（M1 只做“已知存在”，M2 扩协议）
    "046D:C54D": {
      "brand": "Logitech",
      "product": "USB Receiver",
      "note": "Unifying 接收器",
      "source": "hidapi-observed",  // 信息来源
      "verified": false             // 是否真机验证过协议
    }
  }
}
```

规则：键一律大写 4 位十六进制 `VID` / `VID:PID`；只录有依据的条目，
不确定的信息宁可不录；每个设备条目必须带 `source`。

## 4. 约定

- Python 3.11+，仅标准库 + hidapi（M1 不引 GUI/其他依赖）
- 测试：`tests/` 目录，pytest；descriptor / device_manager 差分 / fingerprint
  全部纯逻辑可离线测试；涉及真 hidapi 的函数用 monkeypatch 隔离
- 错误：模块自定义异常 `HIDError` / `DescriptorError` / `ProtocolDBError`，
  cli 层捕获转人话 + 非零退出码
- 输出编码：cli 顶层 `sys.stdout.reconfigure(encoding="utf-8", errors="replace")`
- M1 明确不做：任何写操作、打开标准键鼠接口、GUI、打包（1.4 打包为后续任务）

## 5. 真机验收对照（plan.md）

| 任务 | 验收 | 状态 |
|---|---|---|
| 1.1 | 插拔鼠标列表实时更新 | watch 命令轮询差分 |
| 1.2 | 显示"接口1=键盘，接口2=厂商自定义" | info --desc 输出 application usage + vendor page 标记 |
| 1.3 | 输出 已知(直通)/有官方SDK/未知(可学习) | list 命令状态列 |
| 1.4 | 单文件 exe | 独立任务，M1 验收后进行 |

---

# 架构变更记录（M2 / M3 附加节）

> 以下为 M2（协议库+直通引擎）与 M3（GUI）的附加契约，M1 的 1-5 节继续有效。

## 6. M2 附加架构

### core/report_io.py — 模板渲染 + report 读写器

```python
def render_template(template: list, params: dict) -> bytes
    # 纯函数。元素: int(0-255) 或 "{param}"；未知占位符/越界抛 RenderError

def match_response(response: bytes, check: list, params: dict) -> bool
    # 纯函数。check 元素: int 精确 | None 通配 | "{param}" 引用 | [v1,v2..] 任一；
    # 长度不足 response 直接 False

@dataclass
class HistoryEntry:
    ts: float; direction: str  # "tx" | "rx" | "err"
    data: bytes                # UI 层负责转 hex

class ReportIO:
    def __init__(self, path: bytes | str)   # 惰性打开
    def open() -> None                      # 失败抛 ReportError
    def close() -> None
    def write_output(data: bytes) -> int
    def read_input(timeout_ms: int) -> bytes | None
    def call(data: bytes, check: list | None, params: dict,
             timeout_ms: int = 500) -> bytes | None
        # 写 output + 循环读 input 直到匹配 check 或超时
    def send_feature(data: bytes) -> None
    def get_feature(report_id: int, size: int) -> bytes | None
    history: list[HistoryEntry]             # 每次收发追加
    on_history: Callable | None = None      # 实时订阅（GUI 用）

class ReportError(RuntimeError); class RenderError(ValueError)
```

### db.json schema v2 —— devices.* 可选 protocol 块

```jsonc
"protocol": {
  "name": "hidpp",
  "transport": { "match": {"usage_page": "FF00", "usage": "0001"},
                  "short_len": 7, "long_len": 20, "response_timeout_ms": 500 },
  "features": [{
    "id": "ping", "label": "Ping",
    "kind": "command",              // command=写并等响应 | read=只读 | write=写(必须带 read_from)
    "report": "output",             // output | feature
    "template": [16, "{device_index}", 0, "{request_id}", 0, 0, "{mark}"],
    "params": { "device_index": {"type":"int","default":255,"min":0,"max":255,"label":"设备索引"} },
    "response": { "min_len": 7,
                  "check":  [16, "{device_index}", [0,143], "{request_id}", null, null, "{mark}"],
                  "extract": {"major": 4, "minor": 5} }   // 索引 → 结果键名
  }]
}
```

- load_db 校验 v1/v2：v2 protocol 块结构非法抛 ProtocolDBError
- kind=write 必须含 read_from（指向同协议 read 功能 id），M2.4 起强制

### core/safe_write.py — 写安全机制（M2.4）

```python
@dataclass
class WriteOutcome:
    ok: bool; wrote: bytes | None; readback: bytes | None
    rolled_back: bool; log: list[str]        # 每步一行，含完整 hex

def safe_write(io, write_feature: dict, read_feature: dict | None,
               new_params: dict, timeout_ms=500) -> WriteOutcome
    # 序列: (有 read 时)读旧值 → 写新值 → 读回比对 → 不匹配回滚旧值再读回
    # 全程写 log（完整 hex），io.history 同步记录
```

## 7. M3 附加架构（gui.py，单文件 PySide6 应用）

- MainWindow：左设备列表（QListWidget，状态着色 KNOWN=绿/SDK=蓝/UNKNOWN=灰）
  + 右详情页（设备信息 / 描述符摘要 / 按 db.protocol.features 动态生成的操作区）
  + 底部 hex 日志（QPlainTextEdit，等宽字体，随 on_history 实时追加）
- HotplugWatcher(QThread)：复用 diff_snapshots 轮询 → Qt 信号刷新列表（任务1.1 GUI 版）
- 控件映射：params int+min/max → QSpinBox；其余占位文本；command 功能 → "执行"按钮
- 全部设备 I/O 在工作线程（QThread + 信号回传），UI 不阻塞；M1 只读原则不变
- 入口：`python gui.py`

---

# 架构变更记录（M5 附加节：设备扩容 + 多设备测试）

## 8. core/probe.py — vendor 配置通道只读探测

```python
def feature_report_ids(info: DescriptorInfo) -> list[int]   # 纯函数
def probe_interface(path, usage_page, usage, read_descriptor) -> ProbeResult
def probe_vendor_interfaces(ifaces, read_descriptor) -> list[ProbeResult]
def suspect_rawhid_interfaces(ifaces, read_descriptor) -> list[ProbeResult]
    # usage=0x0000 + 64字节 in/out 的 rawhid 风格配置通道识别（国产键盘常见形态）
    # 只做被动 input 读取，零写入

@dataclass ProbeResult: usage_page, usage, descriptor_len, feature_ids, reads, error, note
```

依赖方向：probe → descriptor + report_io（与 safe_write 同级，单向不循环）。

## 9. 引擎扩展（core/report_io.py，接口追加不改既有签名）

- `render_template` 元素支持 `{#negsum}`：校验和 = 0xFF-(前面字节之和&0xFF)
  （ROYUAN/前行者方案）
- `execute_feature` 支持 `report=feature` 命令路径：send_feature 后
  get_feature 同 ID 读回并按 checks 分支匹配（无超时轮询）

## 10. db.json v2 追加（向前兼容）

- 顶层可选 `pending_brands`: `[{"name","name_zh","note"}]` —— 待实测 VID 的品牌
- brands/devices 条目可选 `note` 自由文本（协议线索、来源、危险提示）

## 11. GUI 多设备测试

- 设备列表头部「批量探测」：已知品牌逐台路由——有 protocol+ping 的跑 ping，
  否则 vendor 只读探测 + rawhid 识别
- 详情页「Vendor 只读探测」：当前设备的 vendor feature 读取 + rawhid 被动监听

---

# 架构变更记录（M4 附加节：学习模式）

## 12. core/recorder.py — 录制器（任务 4.1）

```python
@dataclass(frozen=True) RecorderTarget: label, path, feature_ids=()
@dataclass(frozen=True) CaptureEntry: ts, source, kind("input"|"feature_read"|"error"),
                                       report_id(input 取 data[0] 最佳估计), data(hex)

class Recorder:
    def __init__(targets, read_timeout_ms=50, max_entries=200_000)
    def start() -> None                       # 每目标一个 daemon 线程
    def stop(timeout=3.0) -> None
    is_recording: bool;  overflowed: bool
    def entries() -> list[CaptureEntry]       # 线程安全快照
    def snapshot_features() -> int            # 手动快照（改配置前后各拍）
    def save_jsonl(path) -> int
    @staticmethod load_jsonl(path) -> list[CaptureEntry]
```

- 只读（read_input/get_feature），零写入；Windows input 广播 → 后台录制不影响使用
- 会话 jsonl 每行：{"ts","source","kind","report_id","data"}
- input 读取异常视为该目标死亡：记 error 条目后线程退出
- 超过 max_entries 置 overflowed 并自动停录

## 13. core/diffing.py — 差分分析（任务 4.2）

```python
@dataclass(frozen=True) ByteChange: offset, before(int|None), after(int|None)
@dataclass(frozen=True) GroupDiff: source, kind, report_id, before_len, after_len,
                                    changes, is_new, volatile

def diff_sessions(before, after) -> list[GroupDiff]
    # 按 (source, kind, report_id) 分组取最后一条；error 条目跳过；
    # 无 before 的组 = is_new；逐字节对比只留变化字节
def volatile_groups(entries, threshold=8) -> set    # 疑似数据流噪声标记
def format_diff(GroupDiff) -> list[str]             # 人读行（单组最多 16 变化字节）
```

依赖方向：recorder/diffing → report_io（与 probe 同级）。GUI 集成在任务 4.4。

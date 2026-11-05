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

---

# 架构变更记录（M4.3/M4.4 附加节：引导入库 + GUI 集成）

## 14. core/wizard.py — 引导入库（任务 4.3）

```python
LEARNED_PREFIX = "learned_"          # 学习产物功能 id 前缀

def build_drafts(diffs, note="", label_match=None, max_changes=64) -> list[dict]
    # GroupDiff → 协议功能草稿（kind=read）
    #   feature_read → report="feature", template=[report_id]
    #   input        → report="input",  template=[]（监听型零写入）
    #   volatile 组跳过；response.check = [None]*after_len（草稿通配）
    #   带 learned 佐证块（source/changes/is_new）+ feature 级 match

def build_protocol_block(match, drafts) -> dict
def merge_into_db(db, vid_pid, protocol_block, note=None) -> dict
    # 新设备创建条目(source=learning-mode, verified=False)；
    # 已有 protocol 按 feature id 合并（同 id 覆盖），不动 name、不降级 verified=True
```

## 15. 引擎/协议库配套扩展

- `ReportIO.read_until(checks, params, timeout_ms)`：只读轮询（call_multi 复用）
- `execute_feature` 支持 `report="input"` 监听型（零写入）
- schema: `_VALID_REPORTS += "input"`；feature 可选 `match`（多接口设备按功能选接口，
  GUI/exec 调用方解析目标接口时 feature.match 优先于 transport.match）
- `validate_db(db)` 公开；`save_db(db, path)` 校验 + 原子写回（tmp + os.replace）

## 16. GUI 学习模式标签页（任务 4.4）

- 主窗口改为 QTabWidget：设备页 + 学习模式页
- 学习页流程：勾选 vendor/rawhid 接口 → 开始录制 → 标记起点 →
  （用户去官方软件改设置）→ 停止并分析（diff 起点 ts 前后两组）→
  填说明 → 生成草稿入库（unverified）
- 入库后自动 reload db + 刷新设备页（新功能按钮立即可见）
- 关窗时自动停录

---

# 架构变更记录（M3+ 附加节：多设备调节软件成型）

## 17. GUI 升级（任务 3.4-3.7）

- 主窗口三标签页：**总览**（3.6 设备卡片栅格 + 查看跳转）| **设备** | **学习模式**
- 控制页（3.4）：int 参数范围 >3 自动配 QSlider+QSpinBox 双向联动；
  `self._proto_controls: [(feature, {param: QSpinBox})]` 供档案读写
- 配置档案（3.5，core/profiles.py）：
  ```python
  ProfileFeature(feature_id, params) / DeviceProfile(vidpid, name, created_ts, features)
  save_profile(profile, directory=DEFAULT_PROFILE_DIR) -> Path   # profiles/<vidpid>_<name>.json
  load_profile(path) -> DeviceProfile    # 校验 $schema=device-profile/v1，坏档抛 ProfileError
  list_profiles(vidpid, directory) -> list[Path]                 # mtime 倒序
  ```
  GUI 侧：保存（读控件）/ 加载（写回控件）/ 应用（逐功能执行：
  command/read→execute_feature，write→safe_write 走安全路径，write 的
  read_from 引用功能解析 read_feature）
- 设置持久化（3.7）：QSettings("AvR0raa","peripheral-tool")——窗口几何、
  当前标签页、选中设备，关闭时保存、启动时恢复
- feature 级 match 生效：控制页/档案应用解析接口时 feature.match 优先于 transport.match

---

# 架构变更记录（M2.5a 附加节：全设备电量）

## 18. core/hidpp.py — HID++ 2.0 最小客户端

```python
SW_ID = 5;  FEATURE_ROOT=0x0000;  FEATURE_BATTERY_STATUS=0x1000;
FEATURE_BATTERY_VOLTAGE=0x1001;  FEATURE_UNIFIED_BATTERY=0x1004

def _call_feature(io, dev, feature_index, funct, params, timeout_ms) -> bytes | None
    # 线材: [0x10, dev, featIdx, (funct<<4)|SW_ID, p0-p2]；应答 payload = resp[4:7]
    # 0x8F 错误应答/超时 → None（call_multi 双分支路由）
def get_feature_index(io, dev, feature_id) -> int | None    # Root.GetFeature
def ping(io, dev, mark) -> ("2.0", ver) | ("1.0", 1.0) | ("empty-slot", None) | ("offline", None)
def read_battery(io, dev=0xFF) -> HidppBattery | None
    # 回退链: Unified(0x1004 funct1) → Status(0x1000 funct0) → Voltage(0x1001)
```

来源标注：Solaar lib/logitech_receiver/base.py + hidpp20_constants.py（FEATURE_SET 等
常量逐一核对）。

## 19. core/battery.py — 全设备电量编排

```python
BATTERY_USAGE_PAGE = 0x85            # HID Battery System
def find_battery_fields(descriptor) -> list[ReportField]
def read_standard_battery(io, fields) -> (raw | None, detail)   # feature 直读 / input 200ms 推送
def scan_all(grouped, read_descriptor, hidpp_vids={"046D"}) -> list[BatteryResult]
    # 顺序: 罗技→HID++(接收器槽位1-7 + 直连0xFF) → 标准电量页 → 无源标记(学习模式提示)
BatteryResult: vidpid/product/percent/voltage_mv/source/display
```

依赖：battery → hidpp + descriptor + report_io。GUI 总览「读取全部电量」+ 卡片
电量行（`_battery_cache`）；CLI `python cli.py battery`。
已知边界：写触发型电量（部分国产设备）读不到 → 标记"需学习模式"，协议入库后自动接上。

---

# 架构变更记录（M2.5b 附加节：鼠标 DPI + HID++ 双通道）

## 20. HidppLink 双通道（core/hidpp.py）

- C54D 实测：短报文(0x10) 走 FF00:0001，长报文(0x11) 走 FF00:0002
- `HidppLink(short_path, long_path)`：call_multi 按报文 ID 路由发送，
  **双通道轮询收应答**（应答长度与请求无关，短请求的长应答出现在长通道）
- `_call_feature` check 兼容两种 rid（[[0x10,0x11], ...]）；payload 取 resp[4:20]
- `ping`：有长通道先试长报文（PRO X2 只应答长报文，实测 HID++ 4.2）；
  0x08(UNKNOWN_DEVICE) 换通道重试；**应答不回显 mark**（PRO X2 实测），校验只到 reqLo

## 21. core/dpi.py — Adjustable DPI 0x2201（M2.5b）

```python
def get_dpi_list(link, dev) -> list[int]   # funct1 分页列表, 步进编码 0b111 展开
def read_dpi(link, dev) -> (dpi, default)  # funct2; dpi==0 → 用默认值(Solaar 语义)
def set_dpi(link, dev, new_dpi) -> DpiOutcome   # 读旧→写(funct3)→读回→失败回滚
DpiOutcome: ok/wrote/old/readback/rolled_back/log
```

布局来源：Solaar settings_templates.AdjustableDpi（read_fnid 0x20/write_fnid 0x30/
write_prefix 0x00）+ produce_dpi_list（ignore=1、0xE000|step 编码）。
安全范围 50-32000。GUI：罗技设备详情页自动出现 DPI 面板（槽位/滑条/读/写）。

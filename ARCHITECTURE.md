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

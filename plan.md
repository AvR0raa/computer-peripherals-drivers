# 外设万能调试器（通用驱动）— 开发计划 v4

> AI 辅助开发版 · v1 2026-11-05 · v2 聚焦多设备 · v3 通用驱动愿景 · v4 2026-11-11（技术栈迁移：C++20 原生化）
> 版本纪律：保持 0.x，直到"通用驱动目标"达成验收才考虑 1.0

## 0. 项目定义（v3）

**一句话**：一个通用驱动，用户不用再下载一堆厂商驱动，就能查看和调节所有外设
（包括但不限于鼠标、键盘、耳机、手柄、麦克风……）。

**三条核心行为**：

1. **插入即识别**：外设插入 → 自动识别品牌/型号/类型/能力 → 记录到本机设备档案
   （识别结果 + 能力快照 + 上次电量，形成"我的设备"清单）
2. **能力调节**：识别出的设备按其能力开放调节项（DPI/报告率/RGB/音量/宏……），
   每个调节项都走"读旧 → 写 → 读回验证 → 失败回滚"安全路径
3. **配置迁移**：用户在某台外设上调好的 config（档案），换同类型新外设时
   由算法自动映射迁移（见 §6）

## 0.5 技术栈迁移（v4 核心：C++20 原生化，最低资源占用）

**目标形态（用户指定）**：

- C++20/23 · Native Windows Application · **最低资源占用**
- Win32 API + Windows HID API（SetupDi/HidD_*）+ 设备通知（WM_DEVICECHANGE +
  RegisterDeviceNotification）——热插拔**事件驱动**，不再轮询
- 系统托盘常驻（Shell_NotifyIcon），可做低占用后台服务
- 无 Electron / 无 Python 运行时 / 无大型 UI 框架

**为什么可行（现状盘点）**：

| 资产 | 形态 | 可移植性 |
|---|---|---|
| 协议知识（HID++ 双通道/DPI/报告率/电量三路、ATK、MCHOSE、杰理） | 字节级实现 + 来源注释 | ✅ 机械翻译（字节布局不变） |
| 安全写模式（读旧→写→读回→回滚） | 逻辑 | ✅ 直接移植 |
| 测试 216 个（协议应答脚本） | pytest | ✅ 翻译成断言用例 = 规格书 |
| 学习模式/描述符解析/差分 | Python UI | ⏸ 保留 Python 作为**协议实验室**（研发工具，不随产品分发） |

**迁移策略（双形态分工，不推倒重来）**：

- **C++ 形态 = 交付产品**：托盘常驻 + 电量 + 鼠标设置（DPI/报告率）+ 配置迁移；
  日常使用的低占用常驻客户端
- **Python 形态 = 协议实验室**：学习模式逆向、描述符解析、新协议试验；
  逆向验证过的协议 → 毕业移植到 C++
- 协议层接口一比一对应（Python core/ 模块 → C++ 同名模块），字节布局以
  Python 版注释+测试为准

**C++ 工程结构（对应 Python core/，一比一移植地图）**：

```
native/
├── CMakeLists.txt
├── src/
│   ├── main.cpp            # 入口：托盘 + 消息循环
│   ├── hid_enum.cpp/.h     # ← core/device_manager.py（SetupDi 枚举 + 通知）
│   ├── hid_io.cpp/.h       # ← core/report_io.py（CreateFile/ReadFile/WriteFile/HidD_GetFeature）
│   ├── descriptor.cpp/.h   # ← core/descriptor.py（HidP_GetCaps/ValueCaps 替代自解析）
│   ├── hidpp.cpp/.h        # ← core/hidpp.py（双通道 0x10/0x11 + feature 调用 + sw_id 轮换）
│   ├── dpi.cpp/.h          # ← core/dpi.py（0x2201/0x2202）
│   ├── pollrate.cpp/.h     # ← 0x8060/0x8061
│   ├── battery.cpp/.h      # ← core/battery.py + vendor_battery.py（ATK/MCHOSE/杰理）
│   ├── mouse_api.cpp/.h    # ← core/mouse_api.py（后端注册模板）
│   ├── safe_write.cpp/.h   # ← core/safe_write.py
│   ├── config.cpp/.h       # 配置/设备档案（%APPDATA%/PeripheralTool/，手写极简 JSON）
│   └── tray.cpp/.h         # 托盘图标/菜单/电量环
└── tests/                  # 每个协议一个断言组（从 pytest 翻译）
```

**资源预算（目标）**：内存 < 10MB 常驻（现 PySide6 形态 ~150MB），
exe < 3MB，托盘热插拔事件即时响应，无后台轮询。

**工具链**：CMake + MSVC Build Tools（winget，~3GB）或 MinGW-w64 GCC
（~200MB，Win32 兼容够用）——本机目前无编译器，C1 首任务先装。

## 1. 厂商适配优先级（用户指定）

### 第一梯队（核心，逐家做深）

| # | 厂商 | VID | 已有基础 | 优先方向 |
|---|---|---|---|---|
| 1 | 罗技 Logitech | 046D | ✅ 最深：HID++ 双通道/DPI/报告率/电量 | 全品类（键鼠耳机）+ SDK 桥 |
| 2 | 雷蛇 Razer | 1532 | 识别 14 机型 | OpenRazer 协议移植（DPI/RGB/电量） |
| 3 | 华硕 ROG | 0B05 | 品牌识别 | Aura SDK 桥 + 识别扩容 |
| 4 | 赛睿 SteelSeries | 1038 | Arctis 7 识别 | GG SDK 桥 + 耳机电量 |
| 5 | 海盗船 Corsair | 1B1C | 6 机型识别 | iCUE SDK 桥 + OpenRGB 线索移植 |
| 6 | 达尔优 DAREU | 25A7 | 品牌识别 | 学习模式逆向（EM915 等热门型号） |
| 7 | 雷柏 Rapoo | 待实测 | — | 插机实测 VID + 学习模式 |
| 8 | 狼蛛 AULA | 待实测 | — | 同上 |
| 9 | 狼途 LANGTU | 待实测 | — | 同上 |
| 10 | VGN / VXE | 待实测 | — | 同上（VGN 已在 pending_brands） |
| 11 | 迈从 MCHOSE | 291D/41E4/3837/1837/3654 | ✅ 多线识别 + V9 Turbo 电量协议 | V9 PRO 专用协议逆向 + 全品类 |

### 第二梯队（识别先行，功能跟进）

| # | 厂商 | VID | 现状 |
|---|---|---|---|
| 19 | 双飞燕/血手幽灵 A4TECH/Bloody | 09DA | 品牌识别；Bloody 子品牌待实测 |
| 20 | 极度未知 HyperX | 0951/03F0 | 双线识别 + Cloud 系电量（HeadsetControl 移植） |
| 21 | 联想拯救者 Lenovo Legion | 待实测 | 插机实测 |
| 22 | 卓威 ZOWIE GEAR | 待实测 | 插机实测 |
| 27 | 樱桃 CHERRY | 046A | 品牌识别 |

### 通用兜底（任意厂商）

- 标准 HID 电量页（0x85 + EZ2.4G usage 对，含逻辑范围缩放）✅ 已实现
- 杰理 JL_RCSP 芯片方案探测（TLV + CRC16）✅ 已实现
- 学习模式（差分逆向）✅ 已实现 → 未识别设备的最终路径

## 2. 驱动应该有什么（能力矩阵 = 识别记录的目标模板）

外设插入时按此矩阵探测并**记录能力快照**（写入本机设备档案）：

| 能力 | 鼠标 | 键盘 | 耳机 | 数据来源 |
|---|---|---|---|---|
| 电量 | ✅ | ✅ | ✅ | HID++ / 厂商协议 / 标准页 / 学习模式 |
| DPI/灵敏度 | ✅ | — | — | HID++ 0x2201/0x2202 / 厂商协议 |
| 报告率 | ✅ | ✅(键盘轮询) | — | HID++ 0x8060/0x8061 |
| RGB/灯光 | ✅ | ✅ | ✅ | OpenRGB 移植 / 厂商协议 / 学习模式 |
| 按键/滚轮 | ✅ | ✅ | — | HID++ REPROG / 学习模式 |
| 音量/EQ/麦 | — | — | ✅ | HeadsetControl 移植 / 厂商协议 |
| 配置档案 | ✅ | ✅ | ✅ | profiles/（已实现） |

## 3. 仓库结构（v3 整理后）

```
computer-peripherals-drivers/
├── core/                    # 引擎（协议无关层）
│   ├── device_manager.py    # 枚举 + 热插拔
│   ├── descriptor.py        # HID 描述符解析
│   ├── fingerprint.py       # 识别 + 协议库加载（目录模式：一品牌一文件）
│   ├── report_io.py         # report 读写 + 模板渲染
│   ├── hidpp.py             # 罗技 HID++ 双通道客户端
│   ├── dpi.py               # 罗技 DPI (0x2201/0x2202)
│   ├── battery.py           # 电量编排（HID++→厂商→标准→学习）
│   ├── vendor_battery.py    # 国产协议（ATK前行者/MCHOSE/杰理）
│   ├── mouse_api.py         # 鼠标设置统一 API（模板层）
│   ├── safe_write.py        # 写安全（读回+回滚）
│   ├── recorder.py/diffing.py/wizard.py  # 学习模式
│   └── profiles.py          # 配置档案
├── protocol_db/
│   ├── brands/              # ★ 一品牌一文件（v3 整理）
│   │   ├── logitech.json
│   │   ├── razer.json
│   │   ├── mchose.json
│   │   ├── atk.json / attackshark.json
│   │   ├── internal-*.json  # 笔记本内部设备（过滤用）
│   │   └── ...（26 文件）
│   └── db.json.legacy       # 旧单文件（留档）
├── gui.py / cli.py          # 用户客户端 + 高级模式
├── installer/ scripts/      # 打包
└── plan.md ARCHITECTURE.md README.md
```

## 4. 当前进度（v3 时点，2026-11-11）

| 模块 | 状态 |
|---|---|
| 设备识别（32 品牌 VID / 60 设备 / 一品牌一文件） | ✅ |
| 罗技深度支持（DPI 双 feature / 报告率 / 电量三路 / 双通道） | ✅ |
| 全设备电量编排（HID++→厂商→标准→学习提示） | ✅ |
| 有线/无线自动探测（0xFF 直连 vs 接收器槽位） | ✅ |
| 国产电量协议（ATK前行者/MCHOSE dongle/杰理兜底） | ✅ |
| 鼠标设置统一 API 模板（mouse_api.py 后端注册制） | ✅ |
| 学习模式（录制/差分/引导入库/GUI） | ✅ |
| 用户客户端（卡片墙 UI / 安装包） | ✅ |
| 写安全（读回+回滚+hex日志） | ✅ |
| 213 测试 | ✅ |
| **雷蛇 OpenRazer 协议移植** | ⬜ 下一优先 |
| **键盘/耳机厂商协议（按第一梯队逐家）** | ⬜ |
| **插入识别记录（本机设备档案持久化）** | ⬜ |
| **配置迁移算法** | ⬜ §6 |

## 5. 里程碑（v4 重排：Python 实验室与 C++ 产品线并行）

### C++ 产品线（主交付）

| 里程碑 | 内容 | 验收 |
|---|---|---|
| **M-C1 脚手架** | 装 MSVC/MinGW + CMake 工程 + Win32 托盘窗口 + HID 枚举（SetupDi）+ 设备通知 | 托盘常驻 exe（<5MB），插拔即时打印设备列表，内存 <10MB |
| **M-C2 HID++ 移植** | hidpp.cpp 双通道 + ping/电量三路（字节布局按 Python 版） | PRO X2 在线 + 电量读数（与 Python 版一致） |
| **M-C3 设置移植** | dpi.cpp + pollrate.cpp + safe_write.cpp | 真机读/写 DPI+报告率（读回验证） |
| **M-C4 托盘 UI** | 每设备托盘电量 + 右键菜单（DPI/报告率）+ 设备档案持久化 | 日常使用零 Python |
| **M-C5 厂商协议** | vendor_battery（ATK/MCHOSE/杰理）+ 标准电量页 + 内部设备过滤 | 全设备电量面板 |
| **M-C6 配置迁移** | §6 intent 翻译算法（C++） | 同类设备档案互迁 |
| **M-C7 分发** | 静态链接单 exe + Inno Setup | 双击即用、卸载器齐全 |

### Python 实验室（研发工具，继续演进）

| 里程碑 | 内容 |
|---|---|
| M-V1~V6（v3 原计划） | 雷蛇移植/达尔优深挖/二梯队识别/耳机动能/配置迁移——**在 Python 实验室完成协议验证**，逐个毕业进 C++ |
| 学习模式 | 保持 Python（逆向工具，面向开发者） |

## 6. 配置迁移算法（用户指定的新需求）

**场景**：用户在旧鼠标上调好 config（DPI 1600 / 报告率 1000Hz / RGB 红色），
换了新鼠标（甚至换品牌），希望自动把"意图"迁移过去。

**映射规则（按优先级）**：

1. **能力交集迁移**：取旧档案 ∩ 新设备 capabilities：
   - DPI：旧值若在新设备档位表中 → 直接用；否则**就近取档**（欧氏距离最近档）
   - 报告率：同样就近取档
   - RGB：颜色直接迁移；灯效名不同时映射到新设备最接近效果（静态色保底）
2. **类型守卫**：迁移只发生在同类型设备间（鼠标→鼠标）；跨类型仅迁移电量等通用项
3. **回滚保底**：迁移前记录新设备当前值（safe_write 路径天然带回滚）
4. **人工确认**：迁移预览面板（旧值 → 新设备将写入的值），用户点确认才执行

**数据结构**：档案 schema v2 增加 `intent` 层（设备无关的意图描述），
迁移 = intent 翻译，不做原始字节搬运。

## 7. 执行纪律（继承 v2，全部有效）

1. 每任务带测试（当前 213 个）
2. ARCHITECTURE.md 冻结；改动单独声明
3. 每里程碑 tag + push
4. 真机验证分工：只读 AI 自验 / 写操作用户点按钮验证
5. 协议库铁律：只录有据条目（source 必填）；危险命令永不盲发
6. 版本 0.x —— 直到 §5 M-V7 + M-C7 全部验收才 1.0
7. C++ 移植纪律：每个协议移植前先抄 Python 版的测试用例作规格书；移植后
   同一真机场景 Python/C++ 结果必须一致（对照验收）
8. **更新日志纪律**：每次打 tag / 发 Release，必须在 UPDATELOG.md 顶部按
   「新增/修复/变更/已知问题」四类追加条目（格式见该文件头部说明）

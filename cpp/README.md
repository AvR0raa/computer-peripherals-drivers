# PeripheralToolCpp — Python 版的 C++17 重构

> `core/*.py` + `gui.py` + `cli.py` 的 1:1 平移（2026-11-11 **全部完成**）。
> 接口契约见仓库根 `ARCHITECTURE.md`（Python 版），本目录模块名/语义与其一一对应；
> 协议库与档案数据文件两版**共享**。

## 环境要求

- Windows 10/11 + Visual Studio 2022/2026 Build Tools（含 MSVC + Windows SDK）
- CMake（VS 自带即可）+ Ninja（VS 自带）
- GUI 另需 Qt 6.8+（`aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --outputdir C:\Qt`）
- 第三方已就位（`third_party/`）：hidapi 0.14 源码、nlohmann/json 单头、doctest 单头

## 构建

```bat
cmd /c cpp\build.bat              rem Debug 构建（core+cli+tests；Qt 在则连 gui）
cmd /c cpp\build.bat test         rem 构建并跑全部测试
cmd /c cpp\build-release.bat      rem Release 构建（分发用）
cmd /c cpp\build-release.bat test
```

产物（build/ 或 build-release/）：`pt_core.lib`（静态库）、`cli.exe`、
`PeripheralTool.exe`（GUI，仅 Qt 找到时）、`pt_tests.exe`。
core/cli 静态链接 CRT（/MT），零运行时 DLL 依赖。

## 打包分发

```bat
cmd /c cpp\package.bat      rem 组装 dist\PeripheralTool（windeployqt + db.json）并压 zip
ISCC.exe installer\peripheraltool-cpp.iss   rem Inno Setup 安装包
```

产物：`PeripheralTool-v0.9.0-cpp-win64.zip`（13.7MB）与
`dist/PeripheralTool-Setup-0.9.0-cpp.exe`（11.6MB）。
分发布局：exe 同级放 `protocol_db/db.json`（`PERIPHERAL_TOOL_DB` 环境变量可覆盖）；
`profiles/` 目录首次保存档案时自动创建。

## 目录结构（与 Python 版对照）

```
cpp/
├── CMakeLists.txt            # pt_core / cli / gui / pt_tests 四个目标（Qt 缺席自动跳过 gui）
├── build.bat / build-release.bat / package.bat
├── src/
│   ├── core/                 # ←→ Python core/*.py（一一对应）
│   │   ├── common.hpp        # Bytes/hex/占位符工具 + exe_dir（分发路径定位）
│   │   ├── json_util.hpp     # Json(ordered)/Params/十六进制校验
│   │   ├── descriptor.* / device_manager.* / hid_api.* / fingerprint.*
│   │   ├── report_io.* / safe_write.* / hidpp.* / dpi.*
│   │   ├── battery.* / vendor_battery.* / probe.*
│   │   ├── recorder.* / diffing.* / wizard.* / profiles.*
│   │   └── mouse_api.*       # 统一鼠标 API（Backend 模板层）
│   ├── app/main_cli.cpp      # ←→ cli.py（list/info/watch/battery/mouse）
│   ├── app/main_gui.cpp      # ←→ gui.py main（metatype 注册）
│   └── gui/                  # ←→ gui.py（按窗口拆分）
│       ├── workers.*         # HotplugWatcher / Worker（QThread，信号回 GUI 线程）
│       ├── gui_helpers.*     # 状态着色/类型图标/样式表/qhex
│       ├── gui_payloads.hpp  # Worker 载荷类型（shared_ptr<void> + 约定类型）
│       ├── dpi_dialog.*      # DPI 调节弹窗（滑条+报告率，安全写）
│       ├── main_window.*     # 用户端：单页设备卡片墙（电量+DPI+隐藏设备）
│       └── advanced_window.* # 高级端：总览/设备/学习模式 三标签页
│                            #   （动态协议控件/DPI面板/档案/批量探测/学习入库全保留）
├── tests/                    # ←→ tests/test_*.py（doctest，180 用例 1070 断言）
└── third_party/              # hidapi 源码 / nlohmann-json / doctest
```

## 与 Python 版的关键设计差异

| 事项 | Python 版 | C++ 版 |
|---|---|---|
| 假设备注入 | monkeypatch `_open_device` | `DevFactory` 工厂注入（ReportIO/HidppLink/scan_all/probe 均收工厂参数） |
| duck typing | ReportIO / HidppLink 互换传参 | `HidppChannel` 抽象基类，二者都实现它 |
| 动态协议数据 | dict 直传 | `nlohmann::ordered_json`（键序与 Python 一致，save_db 往返不重排） |
| sw_id 轮换 | 模块级 `_sw_state` | 文件级计数 + `hidpp_reset_sw_id()`（测试对齐用） |
| 后台任务 | `Worker(QThread)` + 动态值信号 | 同名 QThread；载荷 `shared_ptr<void>` + 约定类型（gui_payloads.hpp） |
| db.json / profiles/ | 独占 | **共享同一份数据文件**（优先 exe 同级 → 源码树；`PERIPHERAL_TOOL_DB` 可覆盖） |
| 双通道测试 | 单一假设备全局共享 | `FakeDev(shared_queue)`：每通道一个设备对象、共享同一脚本队列 |

## 内存安全纪律（重构要求 #2）

- 零裸 owning 指针：设备句柄 `unique_ptr` + 自定义删除器；集合树 `variant<ReportField, unique_ptr<CollectionNode>>`
- 所有缓冲 `std::vector`/`std::string`，边界检查后访问；无手写 new/delete（测试假设备经工厂移交唯一所有权）
- ReportIO/Recorder 拷贝已删除、移动语义显式默认；Recorder 析构自动 stop+join；窗口关闭等待工作线程收尾
- MSVC `/W4 /permissive-` 零告警（第三方头 SYSTEM include 屏蔽）

## 已验证（2026-11-11，全部完成）

- `pt_tests.exe`：**180 用例 / 1070 断言全绿**（Debug 与 Release 双配置；含真实 `protocol_db/db.json` 完整性与命中测试）
- 真机（Windows 11，26 个 HID 接口）：`list`/`list --json`/`info --desc`/`battery` 正常；杰理协议在 3654:6901 实测读到 90% 电量
- GUI：Debug/Release 均编译零告警；干净 PATH 下独立启动烟测通过（卡片墙 + 设备枚举 + 热插拔线程）
- 分发：zip（13.7MB）与 Inno Setup 安装包（11.6MB）均生成并验证；cli.exe 纯系统依赖可直接运行

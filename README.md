# easyDebug

面向设备调试的 Qt Widgets 工具，计划支持串口、TCP 服务器、TCP 客户端和 UDP，并提供实时收发显示、端口间转发、自定义发送。

**当前阶段：M0 设计基线 + 主页面初版界面。** 当前版本已经提供可运行的 Qt Widgets 工作台原型：端口列表与参数页、接收监视区、发送区、快捷命令和转发规则草稿。通信按钮仍是预览状态，真实串口/TCP/UDP 后端和性能验收按后续里程碑实施。

## 设计文档

| 文档 | 内容 |
| --- | --- |
| [PLAN.md](PLAN.md) | 需求范围、阶段安排、交付清单与实施顺序 |
| [参考工程分析](docs/REFERENCE_REVIEW.md) | 两个参考工程的实际结构、复用点和需改进的问题 |
| [架构设计](docs/ARCHITECTURE.md) | 模块边界、接口契约、线程、转发、缓冲与配置 |
| [界面与交互](docs/UI_DESIGN.md) | 端口选择控件扩展、监视区、路由和自定义发送 |
| [验收与性能计划](docs/TEST_PLAN.md) | 功能矩阵、异常测试、性能目标与测量方法 |
| [参考源码指纹](docs/reference-snapshot.csv) | 本次阅读涉及文件的 SHA-256，定位工作区版本 |

## 技术基线

采用 C++17、Qt 6.10.1、Qt Widgets、qmake，主验收环境为 Windows 10/11 x64、MSVC 2022。这是根据现有 `easyDebug.pro` 和本机 Qt Creator Kit 作出的首版选择；MinGW 在发布阶段验证。

可用 Qt Creator 打开 `easyDebug.pro`，选择 Qt 6.10.1 Kit 构建。当前界面原型使用 Qt Widgets；通信实现阶段再加入 Qt Network、SerialPort。Qt Test 用于独立测试工程。示例数据只进入界面模型，不会发送到真实设备。

Windows 运行构建结果需要 Qt 运行库和平台插件。构建后可执行：

```powershell
pwsh -File scripts/deploy_qt_windows.ps1 -Binary build/release/release/easyDebug.exe
pwsh -File scripts/deploy_qt_windows.ps1 -Binary build/ui-tests-run/release/easyDebug_ui_tests.exe -IncludeOffscreen
```

运行 UI 测试时使用 `QT_QPA_PLATFORM=offscreen`；脚本会把插件复制到 exe 旁边，避免“没有 Qt 平台插件”启动错误。

## 核心设计

沿用参考工程的“类型下拉框＋参数页”交互，将其扩展为可重复使用的 `PortSelectorWidget`。通信后端独立为 `AbstractTransport`，支持多个端口实例同时打开。

数据在专用 I/O 线程中直接进入转发引擎；监视显示和文件记录使用独立的有界旁路。转发保留原始字节，文本与 HEX 只是显示/发送编码选项。TCP 客户端身份、UDP 目标及数据报边界都显式建模。

参考工程位置：

- `D:/AWork/myself/QTProject/debugTool`
- `D:/AWork/myself/QTProject/YmodemTool`

界面原型对应 [PLAN.md](PLAN.md) 的 M0/M2/M3 交互准备工作；后续实现从 M1 通信契约开始。每个阶段完成后更新任务状态、测试记录和设计偏差。

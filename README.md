# easyDebug

面向设备调试的 Qt Widgets 工具，计划支持串口、TCP 服务器、TCP 客户端和 UDP，并提供实时收发显示、端口间转发、自定义发送。

**当前阶段：M0 设计基线 + 固定双端点工作台。** 当前版本已经提供可运行的 Qt Widgets 工作台原型：顶部 A/B 两行横向端点条、桥接行、接收监视区和单页发送区（目标 A/B/AB + 正文 + 格式/编码/末尾 + 字节预览）。快捷命令列表暂时撤下，后续以右侧可展开面板回归。通信按钮仍是预览状态，真实串口/TCP/UDP 后端和性能验收按后续里程碑实施。

## 设计文档

| 文档 | 内容 |
| --- | --- |
| [PLAN.md](PLAN.md) | 需求范围、阶段安排、交付清单与实施顺序 |
| [参考工程索引](docs/REFERENCES.md) | 三个参考工程的入口、借鉴维度与按需求查表 |
| [参考工程分析](docs/REFERENCE_REVIEW.md) | debugTool/YmodemTool 两个本地工程的结构、复用点和需改进的问题 |
| [BLE 调试工具参考](docs/REFERENCE_BLE_TOOL.md) | 同作者 iOS BLE 调试工具：消息模型、过滤、显示设置、接收组帧的对照 |
| [架构评审](docs/ARCHITECTURE_REVIEW.md) | 对比参考工程的架构评审结论、分级问题清单与修复记录 |
| [架构设计](docs/ARCHITECTURE.md) | 模块边界、接口契约、线程、转发、缓冲与配置 |
| [界面与交互](docs/UI_DESIGN.md) | 端口选择控件扩展、监视区、路由和自定义发送 |
| [验收与性能计划](docs/TEST_PLAN.md) | 功能矩阵、异常测试、性能目标与测量方法 |
| [版本管理](docs/VERSIONING.md) | 版本源文件、自动构建标识和发布版本递增 |
| [参考源码指纹](docs/reference-snapshot.csv) | 本次阅读涉及文件的 SHA-256，定位工作区版本 |

## 技术基线

采用 C++17、Qt 6.10.1、Qt Widgets、qmake，主验收环境为 Windows 10/11 x64、MSVC 2022。这是根据现有 `easyDebug.pro` 和本机 Qt Creator Kit 作出的首版选择；MinGW 在发布阶段验证。

可用 Qt Creator 打开 `easyDebug.pro`，选择 Qt 6.10.1 Kit 构建。当前界面使用 Qt Widgets，SerialPort 与 Concurrent 用于后台扫描系统串口；通信实现阶段再加入 Qt Network。Qt Test 用于独立测试工程。示例数据只进入界面模型，不会发送到真实设备。

串口下拉列表显示端口号和友好名称，启动时自动扫描，之后每 2 秒自动刷新一次，设备插拔无需手动操作。TCP/UDP 地址支持历史下拉选择，编辑完成后保存最近 20 条有效地址，重启后仍可使用；本地绑定地址与远端地址分别记录在用户配置目录的 `preferences.ini`，不会自动切换当前端点目标。

qmake 配置 `easyDebug.pro` 时会自动运行版本脚本，生成 `version/version.h`。主窗口标题显示版本和构建时间；版本号的修改方式见 [版本管理](docs/VERSIONING.md)。

Windows 构建链接完成后会自动运行 `scripts/deploy_qt_windows.ps1`，从当前 qmake Kit 复制匹配 Debug/Release 的 Qt 运行库与平台插件到 exe 目录，支持脱离 Qt Creator 启动。旧构建目录或手动部署可执行：

```powershell
pwsh -File scripts/deploy_qt_windows.ps1 -Binary build/release/release/easyDebug.exe
pwsh -File scripts/deploy_qt_windows.ps1 -Binary build/ui-tests/release/easyDebug_ui_tests.exe -IncludeOffscreen
pwsh -File scripts/deploy_qt_windows.ps1 -Binary build/Desktop_Qt_6_10_1_MSVC2022_64bit-Debug/debug/easyDebug.exe -Configuration Debug
```

运行 UI 测试时使用 `QT_QPA_PLATFORM=offscreen`；脚本会把插件复制到 exe 旁边，避免“没有 Qt 平台插件”启动错误。

移动程序时需保留整个运行目录，包括 Qt DLL 和 `platforms` 子目录；不要仅复制 exe。更换 Kit 后重新运行 qmake；手动部署其他 Qt 版本时用 `-QtBin` 指定对应 Kit。

## 核心设计

沿用参考工程的“类型下拉框＋参数页”交互，并采用其单行形态：A、B 各是一个完整的 `PortSelectorWidget` 行，类型、参数、状态和连接动作都在该行内，两个端点的参数同时可编辑，不需要选中步骤。低频参数（名称、数据位/校验/停止位/流控、连接超时、客户端上限）在该行下方就地展开。通信后端独立为 `AbstractTransport`，桥接只连接这两个端点。

数据在专用 I/O 线程中直接进入转发引擎；监视显示和文件记录使用独立的有界旁路。转发保留原始字节，文本与 HEX 只是显示/发送编码选项。TCP 客户端身份、UDP 目标及数据报边界都显式建模。

参考工程位置（总索引与按需求查表见 [docs/REFERENCES.md](docs/REFERENCES.md)）：

- `D:/AWork/myself/QTProject/debugTool`
- `D:/AWork/myself/QTProject/YmodemTool`
- https://gitee.com/qinjmin/ble-debug-tool-ios.git（BLE 调试工具，功能与交互参考）

界面原型对应 [PLAN.md](PLAN.md) 的 M0/M2/M3 交互准备工作；后续实现从 M1 通信契约开始。每个阶段完成后更新任务状态、测试记录和设计偏差。

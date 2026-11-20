# QQCom-plus 开发与验证

## 工具链与构建

主要验证环境为 Windows x64、Qt 6.10.1 MSVC 2022、C++17 和 qmake。Qt 需安装 Widgets、Network、SerialPort、Concurrent、Core5Compat 和 Test。其他平台/MinGW 尚未完成发布验证。

仓库根目录执行 `build.bat`，或用 Qt Creator 打开 `QQCom.pro`。脚本默认使用当前开发机路径，可通过环境变量 `QT_BIN` 和 `VCVARS64` 指定其他匹配的 Kit。构建结果在 `build/release/release/QQCom-plus.exe`，生成物不会进入 Git。

统一使用 `build/` 下的独立构建目录，不在源码根目录执行 qmake。Qt Creator 的构建目录也应设在 `build/` 下；`.qtcreator/QQCom.pro.user` 是本机 Kit 和运行配置，不提交。根目录不需要 `debug/`、`release/` 或 `.qmake.stash`，它们是早期在源码目录配置工程留下的产物，可以删除。

| 路径 | 内容与清理方式 |
| --- | --- |
| `build/release/` | 主程序 Makefile、中间文件及 `release/QQCom-plus.exe`；可重新构建 |
| `build/core-tests/`、`build/ui-tests/`、`build/update-tests/` | 当前测试构建；可重新构建 |
| `build/release-runtime/<version>/` | 完整打包输入，仅保留仍需使用的版本 |
| `build/releases/` | 最终 ZIP 与定稿更新记录；不放解压目录、日志或清单 |
| `build/release-verification/<version>/` | 校验报告、线上 Release/清单快照及测试日志、截图 |
| `build/ota/` | 打包与更新演练暂存；发布核验后可清理，先检查是否有程序正在运行 |

临时性能探针、对照源码和一次性诊断目录验证结束后删除，测量结论只在仍有效时并入架构或测试说明。源码不依赖这些构建产物。

以下命令在已加载 MSVC 环境、Qt bin 已加入 PATH 的 **cmd.exe** 中执行，起点为仓库根目录：

```bat
mkdir build\core-tests
cd build\core-tests
qmake ..\..\tests\core\core_tests.pro -spec win32-msvc "CONFIG+=release" "CONFIG-=debug"
nmake release
release\QQCom_core_tests.exe -o core-results.txt,txt
cd ..\..

mkdir build\ui-tests
cd build\ui-tests
qmake ..\..\tests\ui\ui_smoke.pro -spec win32-msvc "CONFIG+=release" "CONFIG-=debug"
nmake release
set QT_QPA_PLATFORM=windows
release\QQCom_ui_tests.exe -o ui-results.txt,txt
cd ..\..
```

UI/traffic 性能工程引用主窗口版本头 `version/version.h`。qmake 后再 nmake，避免运行旧目标。测试返回非零或报告中出现 FAIL 都视为失败。无桌面环境可使用 `offscreen` 平台；字体、DPI、原生窗口布局问题仍需 Windows 平台验证。

Windows 串口控制线回归默认跳过。可在上述 cmd 环境中执行 `set QQCOM_TEST_SERIAL_PORT=COM1`，指定空闲的专用测试串口（建议虚拟串口），再运行 `build\core-tests\release\QQCom_core_tests.exe serialReleasesInheritedControlLines`。用例会暂时设置该端口的 DTR、RTS/CTS 和 DSR 门控，读取真实驱动 DCB，验证默认/无流控、软件流控和硬件流控的控制线初始化及关闭行为，结束时恢复原设置。不发送数据，不使用正在调试的设备端口。

测试程序使用独立应用身份或临时文件，不能把生产配置路径写入测试夹具。截图产物可通过 `QQCOM_UI_ARTIFACT_DIR` 指定到构建目录。压测仅对自有本地/测试设备执行，结果放入 `build/`。

## 验证范围

| 测试 | 覆盖内容 |
| --- | --- |
| `tests/core` | TCP/UDP 回环收发、peer 来源、打开失败、组帧边界/兜底、按 peer 隔离、空闲重计时、关闭排空、指令格式往返、旧配置迁移 |
| `tests/update` | OTA 清单协议、版本比较和发布包大小/SHA-256。更新器覆盖、配置保留和失败日志由 `scripts/test_ota.ps1` 覆盖。真实程序的检查、下载、覆盖和重启由 `scripts/local_update_test.ps1` 覆盖 |
| `tests/ui` | 16 种端口类型组合、完整网卡地址/波特率显示、高级行布局、发送编码、过滤和重绘、历史容量、日志会话/退出、指令编辑与导入导出 |
| `tests/perf` | 实际组帧器、监视面板、指令表格探针；见 [性能说明](../tests/PERFORMANCE_TEST.md) |
| `tests/stress` | TCP/UDP 发压、事件循环延迟分析、占用端口辅助工具 |

修改共享运行时、消息或持久化时执行核心和 UI 全套；窄范围布局修改执行对应 UI 用例并检查实际文本绘制区域。测试数量随用例增长，以当前执行报告为准，历史计数只保留在归档记录。

AI 接入测试包含核心 CLI 参数/JSON 输入校验，以及 UI 中的命名管道发现、多实例选择、自动启动、真实 TCP/UDP 收发和历史查询。CLI 的跨进程 UI 用例默认调用 `build/release/release/QQCom-plus.exe`，需先构建主程序；也可用 `QQCOM_CLI_TEST_EXECUTABLE` 指定待测程序。缺少程序会明确跳过，不能当作集成验证通过。自动启动用例使用测试程序作为短时退出的独立窗口，避免触及生产配置。

人工发布检查仍包括：真实串口设备收发与拔插、非本机 TCP/UDP、防火墙、多个显示缩放比例、长网卡名、持续高负载/慢磁盘、关闭时尾帧和日志完整性。自动回环通过不代表以上场景全部验证。

RAW 回归覆盖未打开端点、部分写入、停止后迟到的旧接受结果、分块和间隔，进度只计本地已接受字节。UDP 容量回归直接驱动真实传输对象的接收信号，注入 1024 个唯一来源，核对列表/定时器上限、提示次数与按来源完整的字节输出；实际 UDP 回环用例继续验证套接字和桥接。32 MiB RAW 跨线程 TCP 发送与 SHA-256 验收入口见 [性能说明](../tests/PERFORMANCE_TEST.md)，不以本地提交成功代替对端校验。

`scripts/local_update_test.ps1` 使用仓库内的 `local_update_server.py`（需 Python 3），不依赖临时目录里预先准备的脚本。它在独立目录以本次构建模拟旧版本号，验证下载、覆盖、配置保留和重启，不修改用户现有实例。

快捷指令 CLI 用例验证分组/指令增删改、稳定 UUID、只更新指定字段、排序、无效修改的原子性、完整内容分页、界面同步和重新加载后的持久化。GUI 编辑器与待发送快捷指令的忙碌保护也有用例覆盖；编辑操作不应触发发送信号。

快捷面板重构用例覆盖编辑后复制/删除/拖动的数据保留、结尾符显示与值对应、元数据更新时保留编辑器和选中范围、切组后延迟动作失效，以及跨组移动和序列化缓存失效。发送调度器在核心测试中覆盖顺序、停止、循环、回调内重启和大批次单定时器约束。快捷发送链路使用真实 UDP 回环验收字节，不以未连接端点的错误记录代替发送成功。

## 版本与发布

版本号改 `version/version.h` 里 `VER MANAGER` 区块的 `QQCOM_VERSION_MAJOR`、`QQCOM_VERSION_MINOR`、`QQCOM_VERSION_PATCH`。qmake 在 Windows 配置时调用 `version/update_version.ps1`，只刷新同一区块的时间戳和 `QQCom-plus v主.次.修订 Build:yyyyMMddHHmm` 标题，不改这三个数字。Qt Creator 的「构建」不会每次重跑 qmake，所以日常构建要把 `version/stamp_version.bat` 加到 Make 之前：项目设置、构建和运行、构建步骤、添加构建步骤、自定义过程步骤。命令填 `%{sourceDir}\version\stamp_version.bat`，工作目录填 `%{sourceDir}`，再用向上箭头把它移到 Make 上面。Make 会因 `version.h` 变新而重编引用它的源文件。

主工程链接后通过 `scripts/deploy_qt_windows.pri` 部署同一 Kit 的运行库。手动部署测试或交付目录：

```powershell
powershell -NoProfile -File scripts/deploy_qt_windows.ps1 -Binary build/ui-tests/release/QQCom_ui_tests.exe -IncludeOffscreen
# 其他 Kit 使用 -QtBin 指定对应 bin；Debug 构建加 -Configuration Debug
```

发布时整理完整运行文件并保留 DLL 和插件目录，准备匹配的 MSVC 运行库，排除编译中间产物。最终交付和更新记录格式按 [AGENTS.md](../AGENTS.md)，打包、线上附件校验和稳定清单推送见 [OTA 发布流程](OTA.md#发布流程)。

### GitHub Actions 自动构建与发布

工作流位于 `.github/workflows/build-release.yml`，在 `windows-2022` 上使用 Qt 6.10.1 MSVC 2022（含 SerialPort、Core5Compat、ImageFormats）和 VS 2022 工具链，通过 `build.bat` 构建，与本地 Kit 保持一致。

| 触发 | 执行内容 |
| --- | --- |
| 推送任意分支、提交 PR | 构建 Release，运行核心、升级和 UI 测试（UI 测试关闭自动检查更新，跨进程 CLI 用例使用本次构建的主程序；失败时上传测试结果），按 `build/release-runtime/<version>/` 整理运行文件（含 MSVC 运行库），用 `scripts/package_release.ps1` 打包并以 `validate_ota_manifest.ps1` 校验，再运行 `test_ota.ps1` 和 `local_update_test.ps1` 做更新器校验与真实升级演练，ZIP 和 `固件更新记录.md` 作为构建产物保留 30 天 |
| 推送 `v<version>` 标签 | 先确认标签等于 `version/version.h` 的版本、`更新记录.md` 含 `# QQCom-plus-<version>-win64` 一节，再执行上述构建；通过后创建同名 GitHub Release，标题为 `QQCom-plus-<version>-win64`，说明取该节内容并附 ZIP 大小和 SHA-256 |
| 手动运行并填写已有标签 | 用当前工作流构建该标签的源码并补发 Release；产品名取自该标签 `QQCom.pro` 的 `TARGET`，改名前的标签仍按 `QQCom-<version>-win64` 发布。同名 Release 已存在时只补传缺少的附件并更新说明；已有同名附件时直接失败，不覆盖已发布附件 |

发新版仍按上文顺序：改版本号和更新记录，提交后创建 `v<version>` 标签，推送到 GitHub 即触发发布。标签与版本头不一致或缺少更新记录时工作流直接失败，不生成 Release。GitHub Release 只提供下载；客户端自动升级仍读取 `ota/stable.json`，是否切换稳定版按 [OTA 发布流程](OTA.md#发布流程) 单独核验。

## 贡献与提交

从 `master` 创建短期分支，提交标题使用 Conventional Commits，例如 `fix: 修复 TCP 服务器关闭时的尾帧处理`。一次提交围绕一个主题，不混入生成物、个人配置或无关格式化。C++ 风格由根目录 `.clang-format` 和 `.editorconfig` 约束；历史文件不做无关的整篇重排。

提交前执行 `git diff --check`、Release 构建及与改动有关的测试。修改服务层或公共控件执行对应 core/UI 回归；修改发布脚本执行 OTA 校验及更新演练。用 `git status --short` 检查完整文件列表，根目录的主窗口文件也要纳入检查。旧配置迁移路径和历史说明中的旧名称是兼容依据。

源码、规则、文档、根目录更新记录、版本头和 `ota/stable.json` 入 Git；构建生成的头文件、二进制、日志、截图及本机配置不入 Git。`version/version.h` 是源码文件，不能随生成头一起删除。版本号使用 `主.次.修订`：破坏兼容性递增主版本，向后兼容的新能力递增次版本，问题修复递增修订版本。发布标签固定构建源码，后续维护提交到 `master`，不移动已有标签。

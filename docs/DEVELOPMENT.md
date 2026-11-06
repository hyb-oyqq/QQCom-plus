# QQCom 开发与验证

## 工具链与构建

主要验证环境为 Windows x64、Qt 6.10.1 MSVC 2022、C++17 和 qmake。Qt 需安装 Widgets、Network、SerialPort、Concurrent、Core5Compat、Test 及翻译工具。其他平台/MinGW 尚未完成发布验证。

仓库根目录执行 `build.bat`，或用 Qt Creator 打开 `QQCom.pro`。脚本默认使用当前开发机路径，可通过环境变量 `QT_BIN` 和 `VCVARS64` 指定其他匹配的 Kit。构建结果在 `build/release/release/QQCom.exe`，生成物不会进入 Git。

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

AI 接入测试包含核心 CLI 参数/JSON 输入校验，以及 UI 中的命名管道发现、多实例选择、自动启动、真实 TCP/UDP 收发和历史查询。CLI 的跨进程 UI 用例默认调用 `build/release/release/QQCom.exe`，需先构建主程序；也可用 `QQCOM_CLI_TEST_EXECUTABLE` 指定待测程序。缺少程序会明确跳过，不能当作集成验证通过。自动启动用例使用测试程序作为短时退出的独立窗口，避免触及生产配置。

人工发布检查仍包括：真实串口设备收发与拔插、非本机 TCP/UDP、防火墙、多个显示缩放比例、长网卡名、持续高负载/慢磁盘、关闭时尾帧和日志完整性。自动回环通过不代表以上场景全部验证。

`scripts/local_update_test.ps1` 使用仓库内的 `local_update_server.py`（需 Python 3），不依赖临时目录里预先准备的脚本。它在独立目录以本次构建模拟旧版本号，验证下载、覆盖、配置保留和重启，不修改用户现有实例。

快捷指令 CLI 用例验证分组/指令增删改、稳定 UUID、只更新指定字段、排序、无效修改的原子性、完整内容分页、界面同步和重新加载后的持久化。GUI 编辑器与待发送快捷指令的忙碌保护也有用例覆盖；编辑操作不应触发发送信号。

快捷面板重构用例覆盖编辑后复制/删除/拖动的数据保留、结尾符显示与值对应、元数据更新时保留编辑器和选中范围、切组后延迟动作失效，以及跨组移动和序列化缓存失效。发送调度器在核心测试中覆盖顺序、停止、循环、回调内重启和大批次单定时器约束。快捷发送链路使用真实 UDP 回环验收字节，不以未连接端点的错误记录代替发送成功。

## 版本与发布

版本号改 `version/version.h` 里 `VER MANAGER` 区块的 `QQCOM_VERSION_MAJOR`、`QQCOM_VERSION_MINOR`、`QQCOM_VERSION_PATCH`。qmake 在 Windows 配置时调用 `version/update_version.ps1`，只刷新同一区块的时间戳和 `QQCom v主.次.修订 Build:yyyyMMddHHmm` 标题，不改这三个数字。Qt Creator 的「构建」不会每次重跑 qmake，所以日常构建要把 `version/stamp_version.bat` 加到 Make 之前：项目设置、构建和运行、构建步骤、添加构建步骤、自定义过程步骤。命令填 `%{sourceDir}\version\stamp_version.bat`，工作目录填 `%{sourceDir}`，再用向上箭头把它移到 Make 上面。Make 会因 `version.h` 变新而重编引用它的源文件。

主工程链接后通过 `scripts/deploy_qt_windows.pri` 部署同一 Kit 的运行库。手动部署测试或交付目录：

```powershell
powershell -NoProfile -File scripts/deploy_qt_windows.ps1 -Binary build/ui-tests/release/QQCom_ui_tests.exe -IncludeOffscreen
# 其他 Kit 使用 -QtBin 指定对应 bin；Debug 构建加 -Configuration Debug
```

发布时整理完整运行文件并保留 DLL 和插件目录，准备匹配的 MSVC 运行库，排除编译中间产物。最终交付按 [AGENTS.md](../AGENTS.md)：`build/releases/` 直接平铺 `QQCom-<version>-win64.zip` 与 `固件更新记录.md`，暂存目录、解压后的程序和 manifest 放在其他构建目录。更新记录最新标题为 `# QQCom-<version>-win64`，与 ZIP 名称一致，历史保留。打包及校验步骤见 [OTA 发布流程](OTA.md#发布流程)。版本清单、源码和文档入 Git，生成头、二进制、日志和截图不入 Git。提交前运行 `git diff --check`，检查文件列表和残留旧品牌；旧配置迁移路径及历史说明中的旧名称属于兼容依据。

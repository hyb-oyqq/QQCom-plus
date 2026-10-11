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

人工发布检查仍包括：真实串口设备收发与拔插、非本机 TCP/UDP、防火墙、多个显示缩放比例、长网卡名、持续高负载/慢磁盘、关闭时尾帧和日志完整性。自动回环通过不代表以上场景全部验证。

## 版本与发布

版本号改 `version/version.h` 里 `VER MANAGER` 区块的 `QQCOM_VERSION_MAJOR`、`QQCOM_VERSION_MINOR`、`QQCOM_VERSION_PATCH`。qmake 在 Windows 配置时调用 `version/update_version.ps1`，只刷新同一区块的时间戳和 `QQCom v主.次.修订 Build:yyyyMMddHHmm` 标题，不改这三个数字。Qt Creator 的「构建」不会每次重跑 qmake，所以日常构建要把 `version/stamp_version.bat` 加到 Make 之前：项目设置、构建和运行、构建步骤、添加构建步骤、自定义过程步骤。命令填 `%{sourceDir}\version\stamp_version.bat`，工作目录填 `%{sourceDir}`，再用向上箭头把它移到 Make 上面。Make 会因 `version.h` 变新而重编引用它的源文件。

主工程链接后通过 `scripts/deploy_qt_windows.pri` 部署同一 Kit 的运行库。手动部署测试或交付目录：

```powershell
powershell -NoProfile -File scripts/deploy_qt_windows.ps1 -Binary build/ui-tests/release/QQCom_ui_tests.exe -IncludeOffscreen
# 其他 Kit 使用 -QtBin 指定对应 bin；Debug 构建加 -Configuration Debug
```

发布时复制整个运行目录并保留 DLL/plugins，不只复制 exe；MSVC 运行库另按目标机器环境准备。交付目录可使用 `release/QQCom.exe`。版本清单、源码和文档入 Git，生成头、二进制、日志和截图不入 Git。提交前运行 `git diff --check`，检查文件列表和残留旧品牌；旧配置迁移路径及历史说明中的旧名称属于兼容依据。

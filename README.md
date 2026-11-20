# QQCom-plus

[![Build & Release](https://github.com/hyb-oyqq/QQCom-plus/actions/workflows/build-release.yml/badge.svg)](https://github.com/hyb-oyqq/QQCom-plus/actions/workflows/build-release.yml)

QQCom-plus 是一个基于 Qt Widgets 的 Windows 通信调试工作台，支持串口、TCP 客户端、TCP 服务器和 UDP，并提供收发监视、组帧、过滤、编码、日志和快捷指令管理。

本项目原名 QQCom。本仓库 QQCom-plus 由 QQCom 原仓库迁移而来，保留完整的提交历史和版本标签。

- 原仓库地址：<https://gitee.com/qinjmin/qqcom>
- 本仓库地址：<https://github.com/hyb-oyqq/QQCom-plus>

## 当前能力

- A/B 双端点真实收发，支持串口、TCP 客户端、TCP 服务器和 UDP。串口、TCP 客户端和 UDP 异常断开后可按 1 秒自动恢复，串口需等同名设备重新出现。TCP 服务器不自动重试监听。
- 串口/TCP/UDP 均支持用户自定义空闲超时和结束符组帧，连接中可修改；UDP 按来源隔离，未启用时保留数据报边界。
- 多种显示与发送编码、关键词/正则过滤、文本日志和 TSV 预览导出。
- 手动/定时发送、快捷指令分组、勾选/选中指令跨组移动、拖动排序和 QQCom/JCom/BLE 文件导入导出。
- 原生 JSON CLI AI 接入：可自动发现/启动 QQCom-plus，查询能力，配置和开关端口、发送数据、读取状态与历史/界面文本，以及编辑快捷指令分组和指令；见 [接入说明](docs/CLI.md)。
- 文件收发面板：RAW 直接字节流收发，兼容 CRC 与经典校验和的 XMODEM 单文件收发，以及兼容常见 BLE 调试助手的 YMODEM 文件收发。
- Windows x64 发布目录自动部署 Qt DLL 和插件，版本在 `version/version.h` 里改。

A/B 桥接把一端收到的字节原样写到另一端。后台日志轮转仍在演进中；Windows 自动升级已经接入第一版检查、下载和覆盖流程。启动后会检查清单，状态栏「检查更新」可以手动再查。协议和发布流程见 [OTA 方案](docs/OTA.md)。

## 快速开始

直接使用请从 [GitHub Releases](https://github.com/hyb-oyqq/QQCom-plus/releases) 下载 Windows x64 ZIP，解压后运行 `QQCom-plus.exe`，保留同目录的 DLL、插件和更新器。端口、显示过滤及 RAW/XMODEM/YMODEM 文件收发的操作见 [使用约定](docs/UI_DESIGN.md)。

### 开发环境

- Windows x64
- Qt 6.10.1 MSVC 2022（Widgets、Network、SerialPort、Concurrent、Core5Compat、Test）
- Visual Studio 2022 C++ 工具链
- PowerShell 5.1 或更高版本

Qt Creator 直接打开 [QQCom.pro](QQCom.pro) 即可。命令行构建：

```powershell
.\build.bat
.\build\release\release\QQCom-plus.exe
```

脚本默认使用 `C:\MyProgram\Qt\6.10.1\msvc2022_64\bin` 和 Visual Studio 2022 Community。安装路径不同可以覆盖：

```powershell
$env:QT_BIN = 'D:\Qt\6.10.1\msvc2022_64\bin'
$env:VCVARS64 = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
.\build.bat
```

### 测试

核心、UI 和性能测试的完整命令见 [开发与验证](docs/DEVELOPMENT.md)。提交前至少执行：

```powershell
git diff --check
.\build.bat
```

## 目录结构

| 目录 | 作用 |
| --- | --- |
| `src/` | domain、transport、app、discovery、persistence、automation、ui、update；职责见 [架构](docs/ARCHITECTURE.md) |
| `resources/` | 图标、样式和 Qt 资源 |
| `tests` | 核心、UI、性能和压力测试 |
| `scripts` | Qt 部署、CLI 入口、OTA 打包、校验与演练工具 |
| `docs` | 架构、使用、开发与贡献、CLI、OTA 说明 |
| `ota/` | 客户端读取的稳定清单与清单模板，需提交 Git |
| `version` | 版本头文件。改 `version.h` 里的主/次/修订号 |
| `build/` | 全部构建与本地验证产物，不提交 Git；最终 ZIP 位于 `build/releases/` |

`main.cpp`、`mainwindow.*` 和 `QQCom.pro` 留在根目录，作为程序与主窗口的组合入口。根目录不需要空的 `debug/`、`release/`；它们与实际输出 `build/release/release/`、交付目录 `build/releases/` 不同。详细构建目录约定见 [开发与验证](docs/DEVELOPMENT.md)。

## 版本和发布

版本号在 `version/version.h` 里手动修改，构建只刷新时间戳。版本记录维护在根目录 [更新记录](更新记录.md)，最终交付为 `build/releases/QQCom-plus-<version>-win64.zip` 和同目录的 `固件更新记录.md`。完整打包、附件校验及稳定版发布流程见 [OTA 方案](docs/OTA.md)，约定见 [项目规则](AGENTS.md)。

GitHub Actions 会在每次推送和 PR 时构建 Release、运行核心测试并上传 ZIP 构建产物；推送与 `version/version.h` 一致的 `v<version>` 标签后，自动创建同名 GitHub Release 并附上 ZIP。触发条件和补发方式见 [开发与验证](docs/DEVELOPMENT.md#github-actions-自动构建与发布)。

## AI 直接操作

在界面点击「CLI → 复制给 AI」，把复制的整段说明粘贴给能执行本机命令的 AI，再告诉它要做什么，例如“配置 A 端口，发送查询指令并分析回复”。AI 会自行读取可用功能并操作，无需用户手动运行命令。说明中已包含程序完整路径和当前窗口标识，多开窗口时也能准确连接。

发布目录附带 `qqcom-plus-cli.cmd` 和 [AI 操作速查](AI_ACCESS.md)。完整命令、实例恢复和结果判断见 [CLI 接入](docs/CLI.md)。

## 文档

- [架构](docs/ARCHITECTURE.md)
- [AI / CLI 接入](docs/CLI.md)
- [界面与使用约定](docs/UI_DESIGN.md)
- [开发、验证与贡献](docs/DEVELOPMENT.md)
- [OTA 自动升级方案](docs/OTA.md)
- [更新记录](更新记录.md)

## 贡献和许可

提交代码前请阅读 [开发与贡献约定](docs/DEVELOPMENT.md#贡献与提交)，并确保 `git diff --check` 和相关测试通过。

当前仓库尚未声明开源许可证。除维护者明确授权的范围外，不应将源码或构建产物用于再分发。

# QQCom

QQCom 是一个基于 Qt Widgets 的 Windows 通信调试工作台，支持串口、TCP 客户端、TCP 服务器和 UDP，并提供收发监视、组帧、过滤、编码、日志和快捷指令管理。

仓库地址：[gitee.com/qinjmin/qqcom](https://gitee.com/qinjmin/qqcom)

## 当前能力

- A/B 双端点真实收发，支持串口、TCP 客户端、TCP 服务器和 UDP。
- 串口/TCP 直通、空闲超时、结束符和 U16 小端长度前缀组帧。
- 多种显示与发送编码、关键词/正则过滤、文本日志和 TSV 预览导出。
- 手动/定时发送、快捷指令分组、拖动排序和 QQCom/JCom/BLE 文件导入导出。
- Windows x64 发布目录自动部署 Qt DLL 和插件，版本由 `version/version.json` 统一管理。

桥接转发和后台日志轮转仍在演进中；Windows 自动升级已经接入第一版检查、下载和覆盖流程，协议和发布流程见 [OTA 方案](docs/OTA.md)。

## 快速开始

### 开发环境

- Windows x64
- Qt 6.10.1 MSVC 2022（Widgets、Network、SerialPort、Concurrent、Core5Compat、Test）
- Visual Studio 2022 C++ 工具链
- PowerShell 5.1 或更高版本

Qt Creator 直接打开 [QQCom.pro](QQCom.pro) 即可。命令行构建：

```powershell
.\build.bat
.\build\release\release\QQCom.exe
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
| `src/domain` | 领域值对象和消息模型 |
| `src/transport` | 串口、TCP、UDP 传输实现 |
| `src/app` | 端点运行时、组帧和快捷指令 |
| `src/ui` | Qt Widgets 面板和公共控件 |
| `src/persistence` | 偏好设置、历史地址和旧版本迁移 |
| `tests` | 核心、UI、性能和压力测试 |
| `scripts` | 构建、版本、Qt 部署和发布工具 |
| `docs` | 架构、开发、界面和 OTA 说明 |
| `version` | 发布版本清单 |

## 版本和发布

版本源文件是 `version/version.json`。普通构建只生成被 Git 忽略的 `version/version.h`，不会改变版本号；发布前显式递增版本：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/update_version.ps1 -Mode patch
```

发布目录必须包含完整的 `QQCom.exe`、Qt DLL 和 `plugins` 子目录。生成 OTA 包和清单：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package_release.ps1 `
  -BinaryDir build/release/release `
  -Version 0.1.0 `
  -BaseUrl https://gitee.com/qinjmin/qqcom/releases/download/v0.1.0
```

发布流程、清单校验、安全要求和更新策略见 [OTA 方案](docs/OTA.md)。

## 文档

- [架构](docs/ARCHITECTURE.md)
- [界面与使用约定](docs/UI_DESIGN.md)
- [开发与验证](docs/DEVELOPMENT.md)
- [OTA 自动升级方案](docs/OTA.md)
- [后续工作](docs/ROADMAP.md)
- [贡献指南](CONTRIBUTING.md)
- [变更记录](CHANGELOG.md)

## 贡献和许可

提交代码前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)，并确保 `git diff --check` 和相关测试通过。

当前仓库尚未声明开源许可证。除维护者明确授权的范围外，不应将源码或构建产物用于再分发。

# QQCom

QQCom 是一个基于 Qt Widgets 的 Windows 通信调试工作台，支持串口、TCP 客户端、TCP 服务器和 UDP，并提供收发监视、组帧、过滤、编码、日志和快捷指令管理。

仓库地址：[gitee.com/qinjmin/qqcom](https://gitee.com/qinjmin/qqcom)

## 当前能力

- A/B 双端点真实收发，支持串口、TCP 客户端、TCP 服务器和 UDP。串口、TCP 客户端和 UDP 异常断开后可按 1 秒自动恢复，串口需等同名设备重新出现。TCP 服务器不自动重试监听。
- 串口/TCP 直通、空闲超时、结束符和 U16 小端长度前缀组帧。
- 多种显示与发送编码、关键词/正则过滤、文本日志和 TSV 预览导出。
- 手动/定时发送、快捷指令分组、拖动排序和 QQCom/JCom/BLE 文件导入导出。
- 文件收发面板：RAW 直接字节流收发，兼容 CRC 与经典校验和的 XMODEM 单文件收发，以及兼容常见 BLE 调试助手的 YMODEM 文件收发。
- Windows x64 发布目录自动部署 Qt DLL 和插件，版本在 `version/version.h` 里改。

A/B 桥接把一端收到的字节原样写到另一端。后台日志轮转仍在演进中；Windows 自动升级已经接入第一版检查、下载和覆盖流程。启动后会检查清单，状态栏「检查更新」可以手动再查。协议和发布流程见 [OTA 方案](docs/OTA.md)。

## 快速开始

### RAW/XMODEM/YMODEM 文件收发

发送区有「手动发送」和「文件收发」两个标签。文件页选择端点 A/B、方向和模式：

- `RAW` 适合双方已经约定好文件边界的场景。发送端选择文件后点击“发送文件”；接收端选择保存路径，收到的字节会持续写入，传输结束后点击“停止”提交文件。RAW 本身没有结束标记，停止动作就是接收完成信号。
- `XMODEM` 适合对端只要 128 字节块、不传文件名的场景。接收端先发 `C`（0x43）要 CRC16；约 3 秒没有数据再发 `NAK`（0x15）。对端若直接发来 132 字节校验和帧，也会立刻按经典校验和接收。发送端看到 `C` 就发 133 字节 CRC 帧，看到 `NAK` 就发 132 字节校验和帧。两边都选本地文件路径。末包不足 128 字节时用 0x1A 填充。协议不传长度，接收文件按 128 字节对齐，末尾的 0x1A 会保留。不支持 1024 字节块。
- `YMODEM` 适合需要文件名、长度、CRC16 和重传的场景。发送端选择文件和 128/1024 字节分包；接收端选择目录，文件名和长度从 YMODEM 文件头取得。协议使用 `C` 启动、SOH/STX 分包、ACK/NAK、双 EOT 和空文件头结束，能与常见串口/BLE 调试助手互通。

文件服务限制单文件 100 MB。XMODEM 和 YMODEM 都使用临时文件提交，传输失败不会留下未完成目标文件。YMODEM 接收还会清理文件名中的路径和 Windows 保留字符。该实现按单文件工作，不包含回滚或多文件批量传输。

文件页可以填写一条前置指令和一条后置指令。前置指令发出后先等待设定的毫秒数，再开始文件字节；文件完成后再等待，然后发送后置指令。等待期间到达的字节不写入文件，也不会被当成 XMODEM/YMODEM 控制字。传输过程中显示速度、进度、重试次数、CRC 错误次数，以及已传输文件载荷的 CRC16 和 CRC32。

两端必须连接到同一个可传输字节流的端点；普通监视区仍会显示通信内容，文件协议使用运行时的原始字节入口，不受组帧、结束符或空闲超时设置影响。

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
| `version` | 版本头文件。改 `version.h` 里的主/次/修订号 |

## 版本和发布

版本号改 `version/version.h` 里的 `QQCOM_VERSION_MAJOR`、`QQCOM_VERSION_MINOR`、`QQCOM_VERSION_PATCH`。构建只刷新同一区块里的时间戳和标题字符串，不会改这三个数字。要在每次点「构建」时刷新构建时间，把 `version/stamp_version.bat` 加到 Qt Creator 的 Make 步骤之前。

发布目录必须包含完整的 `QQCom.exe`、Qt DLL 和 `plugins` 子目录。打包脚本还会写入版本说明文件。生成 OTA 包和清单：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package_release.ps1 `
  -BinaryDir build/release/release `
  -Version 1.0.1 `
  -BaseUrl https://gitee.com/qinjmin/qqcom/releases/download/v1.0.1
```

发布流程、清单校验、安全要求和更新策略见 [OTA 方案](docs/OTA.md)。

## 文档

- [架构](docs/ARCHITECTURE.md)
- [界面与使用约定](docs/UI_DESIGN.md)
- [开发与验证](docs/DEVELOPMENT.md)
- [OTA 自动升级方案](docs/OTA.md)
- [贡献指南](CONTRIBUTING.md)
- [更新记录](更新记录.md)

## 贡献和许可

提交代码前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)，并确保 `git diff --check` 和相关测试通过。

当前仓库尚未声明开源许可证。除维护者明确授权的范围外，不应将源码或构建产物用于再分发。

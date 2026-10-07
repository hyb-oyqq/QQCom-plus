# QQCom

基于 Qt Widgets 的通信调试工具，固定 A/B 双端点，支持串口、TCP 客户端、TCP 服务器和 UDP。

## 当前功能

- 真实收发、端点状态、TCP 客户端身份及 UDP 来源 IP/端口记录。
- TCP 服务器和 UDP 通过网卡名称及 IPv4 地址选择本地绑定地址，支持 `0.0.0.0`；本地端口 `0` 由系统分配。
- 串口/TCP 接收支持直通、空闲超时、结束符、U16 小端长度前缀。空闲默认 10 ms，从最后一次非空数据到达重新计时；长度兜底默认 300 字节，关闭时输出残留缓冲。
- 多种显示/发送编码、分类及关键词过滤、文本日志、TSV 预览导出、手动和定时发送。
- 快捷指令分组、重命名、行内编辑、拖动排序、批量顺序发送；QQCom JSON、JCom、BLE 调试器指令文件导入导出。

桥接/路由目前只有界面预览，尚不转发真实数据。协议插件、OTA、工作区管理和后台轮转日志尚未实现，见 [后续工作](docs/ROADMAP.md)。

## 构建运行

主要验证环境：Windows x64、Qt 6.10.1 MSVC 2022、C++17、qmake。需要 Qt Widgets、Network、SerialPort、Concurrent、Core5Compat；测试另需 Qt Test。

Qt Creator 打开 [QQCom.pro](QQCom.pro)，选择对应 Kit 构建。命令行可在仓库根目录运行：

```powershell
.\build.bat
.\build\release\release\QQCom.exe
```

`build.bat` 支持用 `QT_BIN`、`VCVARS64` 覆盖本机工具链路径，采用独立构建目录，失败返回非零。qmake 自动生成版本头；Windows 链接后自动部署当前 Kit 的 Qt DLL 和插件。搬移程序时保留整个运行目录；目标机器还需匹配的 MSVC 运行库。

## 配置兼容

程序、工程和测试目标统一使用 QQCom。Windows 配置位于 `%LOCALAPPDATA%\QQCom\QQCom\preferences.ini`。首次运行且新配置不存在时，从旧目录 `%LOCALAPPDATA%\easyDebug\easyDebug\preferences.ini` 复制设置和指令，原文件保留；已有 QQCom 配置不会被覆盖。

改名不改变指令 JSON 的字段和版本，旧文件仍可导入。仓库所在磁盘目录名称不影响构建；重新打开 `QQCom.pro` 即可使用新工程名。

## 文档入口

| 文档 | 内容 |
| --- | --- |
| [架构](docs/ARCHITECTURE.md) | 真实模块职责、线程、数据流及容量边界 |
| [界面与使用约定](docs/UI_DESIGN.md) | 端口、UDP 语义、组帧、过滤、发送与日志 |
| [开发与验证](docs/DEVELOPMENT.md) | 构建、测试、版本和发布流程 |
| [性能测试](tests/PERFORMANCE_TEST.md) | 可复现探针、压测步骤及计量限制 |
| [后续工作](docs/ROADMAP.md) | 未实现能力和优化优先级 |
| [参考来源](docs/REFERENCES.md) | 参考工程、版本、借鉴范围与许可 |
| [历史归档](docs/archive/2026-10-07-review.md) | 本轮审查结果、性能证据和文档合并记录 |

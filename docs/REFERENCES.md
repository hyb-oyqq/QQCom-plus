# 参考工程总索引

easyDebug 的设计对照了四个参考工程。本文档是它们的入口索引；细节分析见各自的专项文档。

| 工程 | 平台 | 来源 | 专项文档 | 主要借鉴维度 |
| --- | --- | --- | --- | --- |
| debugTool | Qt Widgets / C++17（qmake） | 本地工作区 `D:/AWork/myself/QTProject/debugTool` | [REFERENCE_REVIEW.md](REFERENCE_REVIEW.md) | 端口抽象与工厂、多类型端口参数页、主从转发、50ms 批量刷新 |
| YmodemTool | Qt Widgets / C++23（qmake） | 本地工作区 `D:/AWork/myself/QTProject/YmodemTool` | [REFERENCE_REVIEW.md](REFERENCE_REVIEW.md) | 协议状态机与链路分离、指令历史、转义发送、Ymodem 移植说明 |
| BLE 调试工具 | iOS / Swift / SwiftUI | https://gitee.com/qinjmin/ble-debug-tool-ios.git | [REFERENCE_BLE_TOOL.md](REFERENCE_BLE_TOOL.md) | 消息模型、过滤分类、显示设置开关、接收组帧、发送历史、日志导出 |
| Serial Studio | Qt 6.9（Widgets + QML）/ C++20（CMake） | https://github.com/Serial-Studio/Serial-Studio | [REFERENCE_SERIAL_STUDIO.md](REFERENCE_SERIAL_STUDIO.md) | 分层治理与构建期校验、驱动抽象、环形缓冲帧提取与计数、控制台文本与终端缓冲、发送预设、记录校验 |

## 按需求找参考

| 要做的事 | 先看 |
| --- | --- |
| 消息字段、日志行显示要素、显示开关 | REFERENCE_BLE_TOOL.md §3–4（模型/开关对照表） |
| 过滤分类与关键词搜索 | REFERENCE_BLE_TOOL.md §4；实现以 `TrafficFilter` 为准 |
| 字节流组帧、分包/粘包 | REFERENCE_BLE_TOOL.md §5；配置结构 `src/domain/receiveframing.h` |
| 端口抽象、参数页布局、转发 | REFERENCE_REVIEW.md §3（可复用思路表） |
| 协议解析、文件传输（YMODEM） | REFERENCE_REVIEW.md §3 + YmodemTool 源码 |
| 发送历史、快捷命令、记录导出 | REFERENCE_BLE_TOOL.md §6（尚未借鉴清单） |
| 分层约束、构建期依赖校验 | REFERENCE_SERIAL_STUDIO.md §3（七层静态库 + layer-verify） |
| 帧提取、溢出/丢帧/校验计数 | REFERENCE_SERIAL_STUDIO.md §6（FrameReader + CircularBuffer） |
| 文本视图性能、行状态机、HEX 渲染 | REFERENCE_SERIAL_STUDIO.md §7（TextFormat、批量刷屏、scrollback） |
| 速率/丢弃/摘要计数、拥塞治理 | REFERENCE_SERIAL_STUDIO.md §8（LinkStats、ParseBudget） |
| 循环发送、命令预设、校验和 | REFERENCE_SERIAL_STUDIO.md §9（SendLibrary） |
| 入站分路、桥接写路径 | REFERENCE_SERIAL_STUDIO.md §10（DeviceIoRouter） |
| 记录格式与完整性校验 | REFERENCE_SERIAL_STUDIO.md §12（Sessions/Verifier） |

## 约定

- 两个本地工程与两个远端仓库都保持原状，不修改、不进入本项目工作区；远端仓库按需重新克隆。
- 迁入任何参考语义或代码时，提交说明记录来源工程、路径与文件指纹；指纹登记在 [reference-snapshot.csv](reference-snapshot.csv)。
- 参考工程是"产品语义与教训来源"，不是代码基座：easyDebug 的分层（domain/transport/ui）、严格编码、有界队列等关键决策不因参考而回退。
- Serial Studio 为 GPL-3.0-or-later / 商业双许可：只借鉴设计结论与验证手段，不复制实现；引入其任何代码前须单独评估许可。

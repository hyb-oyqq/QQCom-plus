# 参考工程：BLE 调试工具（ble-debug-tool-ios）

阅读日期：2026-10-02。与 [REFERENCE_REVIEW.md](REFERENCE_REVIEW.md) 的两个本地 Qt 工程并列作为第三个参考工程；参考工程总索引见 [REFERENCES.md](REFERENCES.md)。

## 1. 版本与阅读范围

| 项 | 值 |
| --- | --- |
| 仓库 | https://gitee.com/qinjmin/ble-debug-tool-ios.git |
| 平台 | iOS / Swift / SwiftUI / CoreBluetooth |
| 阅读时 HEAD | `0af321c536e6d640ab12388ae27f5ad7396f8c62`（2026-09-06） |
| 规模 | 128 个 Swift 文件，约 44 000 行 |
| 阅读方式 | 远程仓库浅克隆（`--depth 1`），逐文件阅读核心模型与过滤引擎 |

逐行阅读了以下文件，指纹见 [reference-snapshot.csv](reference-snapshot.csv) 中 `bleTool` 条目：

| 文件 | 内容 |
| --- | --- |
| `Features/Scan/DataTransfer/Models/DataTransferModels.swift` | TransmissionLog 消息模型、LogType、DataEncoding 六种编解码、EndingDataOption 行尾、SendHistoryItem |
| `Shared/Logging/LogProcessing.swift` | LogFilterable 协议、LogFilterEngine 类别+关键词过滤、JSONL 流式导出 |
| `Models/UserSettings.swift` | FilterSettings / DisplaySettings（显示开关、组帧全部参数）持久化 |
| `BLE/Peripheral/Models/PeripheralTransmissionLog.swift` | 外设模式日志（centralId + characteristicUUID） |

UI 层（LogTableView、PeripheralLogTableView 等）通过用户截图目视确认行结构，未逐行阅读。作者即本项目作者，产品语义（显示设置、精简日志、组帧）按同一作者意图对齐。

## 2. 定位

该 app 是同一作者在 iOS 上实现的 BLE 外设调试工具，与 easyDebug 的共同点是"实时观察原始收发数据 + 自定义发送"，差异是链路层（BLE MTU 报文 vs 串口/TCP 字节流）。**借鉴的是产品功能与交互设计，不是代码**——Swift/SwiftUI 与 Qt/C++ 不存在可迁移的实现。

它先于 easyDebug 遇到了同一批问题：消息该存什么字段、过滤怎么分类、显示哪些信息可开关、字节流怎么组帧。这些答案可以直接映射为 easyDebug 的验收参照。

## 3. 消息模型对照

参考实现 `TransmissionLog`：`id / timestamp / type / message / data / characteristicUUID / descriptorUUID`，类型枚举 `send/receive/error/info/warning`。

easyDebug 的 `Traffic::Message` 是它的超集，映射关系：

| 参考字段 | easyDebug 对应 | 差异 |
| --- | --- | --- |
| `id` | `Message::id` | 一致，均作稳定 key |
| `timestamp` | `timestamp` + `monotonicNs` | 本项目多单调时钟，排序/速率不受系统时间调整影响 |
| `type: send/receive` | `Direction::Receive/Transmit` + `Origin` | 本项目把"方向"与"触发方式"拆开，手动/定时/转发 TX 可区分 |
| `type: info/warning/error` | `Direction::State/Warning/Error` | 2026-10-02 补齐 Warning |
| `message` | `statusText` + `detail` | 本项目拆成结构化说明与自由文本 |
| `data` | `raw` + `rawLength` + `truncated` | 本项目多截断语义（显示真实长度） |
| `characteristicUUID`/`centralId` | `Source`（endpointId/peerId/peerAddress） | BLE 的属性 UUID 在通用调试工具里对应端点与对端身份 |

结论：模型不返工，后续 BLE app 出现新的显示/过滤需求时先查本表确认字段已覆盖。

## 4. 过滤与显示设计对照

参考实现把每条消息投影成四个搜索面（`LogFilterable`）：类别（5 类开关）、文本（message）、元数据（UUID 拼接）、数据（按当前编码渲染后匹配）；`LogFilterEngine` 先按类别短路再跑关键词。

easyDebug 的 `TrafficFilter` 已实现同构且更强的版本：类别开关（端点集合/RX/TX/示例/完整性/最小长度）+ 七面关键词匹配（peerAddress、endpointName、statusText、detail、当前格式渲染、UTF-8、两种 HEX 写法）。**所见即可搜的原则两边一致**。

显示开关对照（`UserSettings.DisplaySettings` ↔ 本项目）：

| 参考开关 | easyDebug | 落点 |
| --- | --- | --- |
| `showTimestamp` | `showTimestamp` | `TrafficDisplayOptions`（2026-10-02 新增） |
| `showDataLength` | `showLength` | 同上 |
| `showCharacteristicUUID` | `showPort` / `showPeer` | 同上 |
| `showHexData`（同时显示 HEX） | `showHexAlongside` | 同上 |
| `isCompactMode` | `compact` | 同上 |
| `autoScroll` | 「滚动到底」按钮 | 已实现 |
| `defaultSendEncoding` / `defaultDisplayEncoding` | 发送区/监视区格式组合框 | 已实现（`PayloadCodec::formats()`） |
| `timedSendInterval`（默认 1000ms） | 定时发送 | 发送区已规划，M5 周期任务 |

参考实现的设置经 `UserSettings`（Codable + UserDefaults）持久化；本项目对应 `PreferencesStore`（QSettings），M5 落地时按上述对照取键名语义。

## 5. 接收组帧对照

参考实现（`enableMergedReceive` 全家）：帧判定 `none/delimiter/lengthU16LE`，空闲超时默认 100ms，缓冲阈值默认 300B，另有四个细节开关：结束符输入类型（HEX 或字符串）、输出是否包含结束符（默认不含）、输出是否包含长度前缀（默认含）、是否输出"合并提示"信息日志。

easyDebug 的 `ReceiveFraming`（2026-10-02 新增）已对齐：`Passthrough/Delimiter/LengthPrefixU16LE` + `idleTimeoutMs=100` + `bufferThresholdBytes=300` + `includeDelimiter=false` + `includeLengthPrefix=true`。"合并提示日志"是运行时行为，实现组帧器（M3/M4）时作为一条 `Direction::Info` 状态消息发出，不进配置结构。

差异提醒：BLE 通知包自带 MTU 报文边界，组帧是"包与包的合并"；串口/TCP 是裸字节流，组帧还要承担"从流里切帧"。语义上本项目的组帧器要更严格，超时与阈值兜底因此是硬要求（防无定界协议数据无限滞留）。

## 6. 尚未借鉴、按需参考的能力

| 参考能力 | 位置 | easyDebug 可能的落点 |
| --- | --- | --- |
| 发送历史（SendHistoryItem：data+encoding+ending） | DataTransferModels.swift | 发送区历史下拉/重发，M3 |
| 快捷发送分组（QuickSendItem + group 分组） | DataTransferModels.swift、QuickSendFullScreenView | 命令列表（原 sendpanel 页签暂缓项），M5 |
| JSONL 流式日志导出/导入（64KiB 分块、进度回调） | LogProcessing.swift（JSONLStreamReader、LogExportFileWriter） | R11 记录与诊断的文件格式与流式读取，M5 |
| "合并输出提示"日志行 | UserSettings.swift + 组帧服务 | 组帧器实现时的状态消息，M3/M4 |
| 外设模式日志（centralId 维度） | PeripheralTransmissionLog.swift | TCP Server 多客户端视角的日志分组参考 |
| LogType → 图标/颜色的统一映射 | PeripheralTransmissionLog.swift | 行渲染的视觉规范参考 |

## 7. 明确不迁移的部分

- CoreBluetooth 全部依赖（characteristic/descriptor/订阅/写入方式 WriteType）——BLE 链路特有。
- SwiftUI 声明式 UI 结构、@AppStorage 绑定——技术栈不可迁移；借鉴交互形态，不借鉴实现。
- ScanSettings / 广播格式化 / 厂商 ID 字典——扫描与广播是 BLE 特有页面。
- 外设模拟器（虚拟 Service/Characteristic/数据生成）——easyDebug 首版不做设备模拟。

## 8. 使用约定

后续开发遇到过滤、显示设置、组帧、发送历史、日志导出类需求时，先查本文档第 4–6 节的对照表；需要细节时按第 1 节的文件清单到参考仓库对应文件取语义，并在迁移提交说明里记录来源路径与指纹。参考仓库不进入本项目工作区，需要时重新克隆。

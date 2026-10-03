# 参考工程：Serial Studio（Serial-Studio/Serial-Studio）

阅读日期：2026-10-03。与 [REFERENCE_REVIEW.md](REFERENCE_REVIEW.md)、[REFERENCE_BLE_TOOL.md](REFERENCE_BLE_TOOL.md) 并列作为第四个参考工程；参考工程总索引见 [REFERENCES.md](REFERENCES.md)。

## 1. 版本与阅读范围

| 项 | 值 |
| --- | --- |
| 仓库 | https://github.com/Serial-Studio/Serial-Studio |
| 定位 | 硬件遥测仪表盘（把设备数据变成实时曲线/仪表），不是收发调试工具 |
| 平台 | Qt 6.9+（Widgets + QML 混合）、C++20、CMake 3.20+，全平台 + Raspberry Pi |
| 许可 | 双许可：GPL-3.0-or-later（源码构建）/ 商业许可（Pro 模块与官方二进制） |
| 阅读时 HEAD | `70aeb505f1001789a182a0c5c82f9c0d1e987637`（2026-10-02） |
| 规模 | 6 450 个受版本控制文件，其中 2 323 个 `.cpp/.h` |
| 阅读方式 | 远端浅克隆（`--depth 1`）到工作区外 `D:/AWork/myself/QTProject/_refs/Serial-Studio`，逐文件阅读 IO、控制台、帧提取与统计相关代码 |

逐行阅读了以下文件，指纹见 [reference-snapshot.csv](reference-snapshot.csv) 中 `serialStudio` 条目：

| 文件 | 内容 |
| --- | --- |
| `core/Core/IO/HAL_Driver.h` | 驱动抽象、`CapturedData` 数据块、`DriverProperty` 属性表与 `visibleWhen`、open 结果闩锁、独占资源声明 |
| `core/Core/IO/LinkStats.h` | 链路计数聚合（bytesIn/droppedFrames/overflowBytes/checksumErrors/framesExtracted） |
| `core/Core/IO/FrameConfig.h` | 帧提取配置聚合（起止序列、校验算法、帧检测模式） |
| `core/Pipeline/IO/FrameReader.h` | 环形缓冲帧提取、KMP 多模式锚点、对象池、诊断计数 |
| `core/Pipeline/IO/StreamWorker.h` | 每源一个工作线程 + 显示环、暂停原子量、有界退出 |
| `core/Core/CircularBuffer.h` | 定容环形缓冲（2 的幂、溢出计数、KMP/短模式/字节三级扫描） |
| `core/Core/Checksum.h` | 校验算法注册表与模板包装 |
| `core/Core/ParseBudget.h` | 公平份额解析负载治理（EWMA + 迟滞 + 抽稀） |
| `core/Ui/Console/TextFormat.h/.cpp` | 行尾归一化与跨 chunk 行状态、控制字符过滤、SIMD hex dump |
| `core/Ui/Console/Handler.h/.cpp` | 控制台文本装配、UI 节拍批量刷屏、scrollback、多设备分路、发送选项 |
| `core/Ui/Console/SendLibrary.h` | 发送历史 + pins + 循环发送（spec 0091） |
| `core/Devices/IO/ConnectionManager/DeviceIoRouter.h` | 入站分路、原始字节 tap 上限、写路径与应答捕获 |
| `core/Devices/IO/Drivers/Network.h` | TCP/UDP/WebSocket/HTTP 客户端门面，SocketType 编号即持久化编号 |
| `core/CMakeLists.txt` | 七层静态库分层与 `scripts/layer-verify.py` 的构建期约束 |
| `app/tests/tst_terminal_buffer.cpp`、`tst_console_text_format.cpp`、`tst_frame_reader_modes.cpp` | 终端行存储的封顶/裁剪/折叠、文本格式化、帧检测模式的已知答案测试 |

未逐行阅读：QML 仪表盘与 widget 层、工业协议（Modbus/CAN/OPC UA/S7）、JS/Lua 脚本引擎、AI 助手、SQLite/CSV 录制（`core/Storage/`）。这些与 easyDebug 首版无关，只在第 12 节登记入口。

## 2. 定位与借鉴边界

Serial Studio 解决的是"设备数据怎么变成好看的图"，easyDebug 解决的是"两个端点之间的原始字节怎么看得清、转得准、发得出"。产品目标不重叠，重叠的是**底层同一批硬问题**：字节流怎么切帧、文本视图在 MB/s 下怎么不卡、计数怎么统计才不骗人、驱动接口怎么抽象才不泄漏。

因此本工程的借鉴口径是：**借鉴结构化做法（分层治理、缓冲与计数、批量刷屏、行状态机），不借鉴产品形态（仪表盘、widget、协议脚本），不复制任何代码**——GPLv3 与商业双许可意味着连"照着写一段"都要谨慎，本项目只用它的设计结论与验证手段（测试名、阈值、不变量）。

## 3. 分层治理（最值得抄的一条）

`core/CMakeLists.txt` 把核心拆成**七个静态库，依赖只允许向下**：

```text
Ui -> Api -> Storage -> Pipeline -> Protocols -> Core -> Qt6::Core
           Devices -----^  （Pipeline 的兄弟层；Api 与 Ui 可见两者）
```

关键在它的**隔离是构建顺序的属性，不是 lint 规则**：`core/` 在 Qt 发现之后、`include_directories(app/src)` 之前被 `add_subdirectory`，于是库的翻译单元根本看不到 `app/src`。再由 `scripts/layer-verify.py` 兜底，对四类问题直接失败：include 越出该层的依赖图、CMakeLists 里越界的 include root 或 link、`core/` 下无人认领或被两个目标同时认领的源文件、CMake 里写了不存在的文件。跨库状态只走 `Core::Bus::MessageBus` 或组合根绑定的接口，任何库都不构造、不抓另一个库的单例。

对照 easyDebug：`.pri` 分层已经画好（`domain/transport/app/ui`），但"不许越界"目前靠评审发现（[ARCHITECTURE_REVIEW.md](ARCHITECTURE_REVIEW.md) 的 R 级问题里就有物理目录没跟上分层）。可落地的动作：

1. 写一个 `scripts/layer-verify.py`（同名即可）：解析 `src/**/*.pri` 的 `HEADERS/SOURCES/INCLUDEPATH` 与 `#include` 路径，断言 `domain` 不出现 `QtWidgets/QWidget`、`transport` 不出现 `ui/`、UI 允许依赖全部下层；CI 或提交钩子失败即拒。
2. 把"库先于应用 include 目录"这条 CMake 经验翻译成 qmake 的等价约束：`src/ui` 的 `INCLUDEPATH` 不反向灌给 `src/domain`。
3. 跨模块状态学习它"只走消息总线或组合根绑定的接口"，避免 `PortDiscoveryService` 之后出现第二个被 UI 直接抓的单例。

## 4. 端点抽象：HAL_Driver 对照 AbstractTransport

`HAL_Driver`（`core/Core/IO/HAL_Driver.h`）的契约：

| 能力 | Serial Studio | easyDebug |
| --- | --- | --- |
| 生命周期 | `open(OpenMode)` / `close()` / `isOpen()` / `isConnecting()` | `start(draft, generation)` / `stop()` + `Closed/Opening/Open`；语义等价，本项目多 generation 过滤旧会话 |
| 能力查询 | `isReadable()` / `isWritable()` / `configurationOk()` / `isStreamCapable()` | 按类型在 draft 校验阶段判定，未单独建模 |
| 写入 | `write(data)` 返回 `qint64` | `write(payload, peerId)` 返回本地接受字节数，区分 LocalCompleted/Partial/Failed |
| 参数 | `driverProperties()` 返回属性表 + `setDriverProperty(key, value)` | `PortDraft` 强类型结构体 |
| 结果上报 | `openFinished(ok, reason)` 由 `armOpenReport/disarmOpenReport` 闩锁保证"只报一次" | `errorOccurred` + 状态回 Closed；本项目用 `m_opening` 标志防 `open()` 内同步 emit 的重入（见项目长期记忆） |

三条可直接搬的做法：

- **open 结果闩锁**：连接管理器在 open 前 `armOpenReport()`，第一次上报即 `disarmOpenReport()`，后续"链路已建立"事件不能冒充拨号结论。easyDebug 的 TCP 客户端 Opening→Open 判定属于同一问题，当前用 `m_opening` 标志，语义一致，可考虑显式命名成"报告闩锁"并写进 `AbstractTransport` 契约注释。
- **独占资源声明**：`exclusiveResource()` 返回驱动要独占的设备名（串口返回端口名），项目里两个源指向同一设备时在打开前就拒绝，理由是"第二个 open 只会得到一条不指名来源的驱动级 busy 错误"。easyDebug 的 A/B 双端点同样可能出现两端点选同一串口，M4 之前值得加这条前置校验。
- **属性表 + visibleWhen**：参数以 `DriverProperty`（key/label/description/type/value/options/min/max）描述，某模式用不到的行用 `visibleWhen` 隐藏而不是删掉，因此"这份配置能持久化什么"与"界面显示什么"解耦。easyDebug 是强类型 `PortDraft`，不必改结构，但**低频参数就地展开的显示/隐藏逻辑**可以照这个规则表写，避免 `if (kind == Serial)` 散落各处。

## 5. 数据块与时间戳

```cpp
struct CapturedData {
  QByteArray data;
  std::chrono::steady_clock::time_point timestamp;   // 捕获边界处盖戳
  std::chrono::nanoseconds frameStep = 1ns;          // 帧间步长
  qsizetype logicalFramesHint = 0;
};
using CapturedDataPtr = std::shared_ptr<const CapturedData>;
```

块在捕获边界盖一次戳，下游**只推导不重盖**（密集采样块里逐样本时间 = `t0 + i*dt`），且以 `shared_ptr<const>` 跨层传递，实现零拷贝共享。easyDebug 的 `Traffic::Message` 同样带 `monotonicNs`，方向一致；值得学的是**把"时间基准 + 步长"随块下发**，将来若做批量帧的时间戳插值（帧提取把一大块切成 N 帧时，按 endOffset 反推每帧时间）不必回头改模型。

另有一条硬约束写在驱动注释里：**终端视图走 `publishConsoleData()`，绝不能再进 FrameReader 或导出扇出**，否则每个样本被记两遍。easyDebug 的"监视区文本视图是唯一显示、TrafficModel 只做结构化存储"是同一原则，M4 桥接接入时要防止桥接 TX 二次进显示。

## 6. 帧提取与统计（对标 M3/M4 组帧器）

`FrameReader`（`core/Pipeline/IO/FrameReader.h`）的组合：

- **定容环形缓冲** `CircularBuffer<QByteArray,char>`：容量向上取整到 2 的幂，溢出字节数用 `std::atomic<qsizetype>` 累加并可 `resetOverflowCount()`；查找按模式长度分三级——≤8 字节短模式专用扫描、字节级扫描、KMP（`buildKMPTable`/`PatternAnchors`，最多 8 个模式同时找 `findFirstOfPatterns`）。
- **校验**：帧尾按算法表校验，返回 `FrameOk / ChecksumError / ChecksumIncomplete`（长度不够时不算错）。
- **五个诊断计数**：`bytesIn`、`framesExtracted`、`droppedFrames`、`checksumErrors`、`overflowBytes`，外加 `m_lastDropNotify/m_lastOverflowLog/m_lastChecksumLog` 三个**节流时间戳**——同一类异常不逐帧刷日志。
- **对象池 + SPSC 队列**：`kCapturedPoolSize = 4096` 个复用 `CapturedData` 槽（按 FIFO 排空顺序、最多探 64 次找引用计数归零的槽），帧经 `moodycamel::ReaderWriterQueue` 出队。

对 easyDebug 的落点：

| 点 | 借鉴结论 |
| --- | --- |
| 溢出与丢帧分开计数 | easyDebug 已有暂停丢弃计数；建议再分"缓冲溢出字节"与"丢弃帧数"两列，避免用显示丢弃冒充通信丢包（PLAN §3 已有此要求） |
| 异常日志节流 | 组帧器/桥接实现时照抄时间戳节流，别让 34 万行时的每条消息都发一次状态通知 |
| ChecksumIncomplete 单独一类 | 校验和不完整不算错误，避免误报；本项目若做帧校验可照此三态 |
| 短模式专用扫描 | 定界符通常 1–4 字节，`CircularBuffer` 的三级扫描思路可直接用于组帧器的定界符查找 |
| 帧时间戳插值 | 切帧时按 `frameTimestamp(endOffset)` 反推，而不是给每帧盖 `now()` |

## 7. 控制台文本与终端缓冲（最贴近 traffictextview 的一节）

### 7.1 跨 chunk 的行状态机

`TextFormat::LineState { bool isStartingLine; bool lastCharWasCR; }`：**每个流一份**（共享视图一份、每个设备一份）。`formatIncoming()` 处理三件事——上一块以 CR 结尾且本块以 LF 开头则吞掉这个 LF（CRLF 跨批）；把 `\r\n`/`\r` 统一成 `\n`；只给"此处开始且非全空白"的行加时间戳前缀。

对照 easyDebug：监视区把 `\r\n` 经 `PayloadCodec::renderMultiline` 还原为真实换行、续行不带行号。**CR 停在块尾、LF 在下一块开头**这个跨批边界，值得补一条明确的测试（用分两次写入的 FakeTransport 断言不产生多余空行）。

### 7.2 hex dump 的行渲染

`hexDump()` 每行写进**固定 80 个 `char16_t` 的栈上 scratch**（偏移 5 位十六进制 + `|` + 16 字节 + 第 8 字节后加一空格 + `|` + ASCII 列），一次 `QString::append(const QChar*, len)` 收尾，ASCII 列整行走 SIMD（`DSP::simdAsciiDots16`）。注释直接写明动机：这样替换掉了开启 hex 视图时占据 GUI 时间的逐字符 append。

对照 easyDebug：`PayloadCodec::render` 是渲染唯一真源，若将来提供"HEX 分栏 + ASCII 列"的显示模式，应照这个"定长 scratch + 一次 append"的写法，禁止逐字节 `QString::append`。

### 7.3 UI 节拍批量刷屏

`Handler` 把待显示文本累积到 `m_pendingDisplay`，挂在全局 **UI 节拍定时器**（`TimerEvents::uiTimeout`，默认 60 Hz，1–240 Hz 可配）上一次 `emit displayString(...)` 后清空；同时另存一份 UTF-8 到 `m_textBuffer` 环形缓冲供导出/搜索。多设备时每个设备各有一份 `DeviceConsoleState`，非当前设备的缓冲有 **10 KiB 上限**（超出从头部整块丢弃），只在切到该设备时回放。

对照 easyDebug：本项目是"到达即按到达时格式固化、一次 movePosition(End) 批量游标追加"，节拍靠批量而非定时器；两者目标一致（GUI 不按消息条数付费）。可参考的两点：**刷新频率做成可配置项**（默认 60 Hz 上限 240 Hz 的取值区间值得抄）与**非当前视图的设备缓冲单独封顶**（easyDebug 若将来做端点分路视图，别让后台端点无限累积）。

### 7.4 scrollback 与折叠

| 项 | Serial Studio | easyDebug |
| --- | --- | --- |
| 行数上限 | `scrollbackLines`，默认 1000，`qBound(100, n, 100000)`，`QSettings` 持久化 | 文本视图 `maximumBlockCount(100 万行)` + 字符预算 2.56 亿（超限头部整块淘汰） |
| 上限下调 | 一次性裁掉多余行并回报裁掉多少，视图状态随之平移 | 未涉及 |
| 相邻重复行折叠 | `collapseDuplicates` 开关，计数并合并相邻重复行（非相邻不合并） | 未实现 |
| 光标/视图 | 游标行列被夹到列数与 scrollback 上限内；裁剪时游标行随保留行平移 | 跟随 = 追加后 setValue(maximum)，valueChanged<max 即解除跟随 |

easyDebug 的预算更大（100 万行 vs 10 万行），方向不回退；可借鉴的是**"折叠相邻重复行 + 显示重复次数"这个开关**（`duplicateCollapseCountsRepeats` / `duplicateCollapseNeedsAdjacentRows` 两个测试给出了精确语义：只合并相邻行、带计数），对周期性心跳帧刷屏的场景很实用，属于低成本高收益。

## 8. 计数与负载治理

- **`LinkStats`：1 Hz 拉取、按增量消费**。注释写明"reader 在连接/重配时会被重建，所以计数减少意味着重置，消费者一律按 delta 处理（spec 0033：pulled, never pushed）"。
- **`ParseBudget`：公平份额抽稀**。每个源的解析耗时计入 EWMA 负载（漏积分器，tau 250 ms）；总负载超过 0.90 才介入，回落到 0.70 以下才解除（迟滞，避免振荡）；**只有超过公平份额（0.90/活跃源数）的源被抽稀**，抽稀比按超出比例算，上限 1024；被抽稀的工作不计入负载（否则估计值一介入就腰斩，治理器变成张弛振荡器）；另有 1 Hz 的 `maintain()` 衰减扫，让"被抽稀后又静默"的源一秒内恢复，不会永久挂着旧系数。

对照 easyDebug：摘要用 `m_totalAccepted`（会话累计条数）而不是任一侧的行数，已经是同类判断（模型与文本各有淘汰，任一方行数都会缩水）。可再补两点：**所有对外计数明确"可重置、取增量"语义**（M5 记录与诊断的摘要栏）；**诊断面板 1 Hz 拉取而非事件推送**（本项目已有"每条消息的通知开销"这条性能铁律，_parse budget_ 是同一原则在解析侧的完整版本，M4 桥接拥塞治理时可直接参考其阈值与迟滞设计）。

## 9. 发送侧（对标 R09/M5）

`SendLibrary`（spec 0091）把发送预设分成两层：**项目级 Actions**（随项目文件持久化，唯一耐久层）与**控制台级 pins + 历史**（`QSettings` 持久化，不进项目文件）。循环发送是"单条 payload + 间隔"：`armCyclic(payload)` / `disarmCyclic()` / `setCyclicIntervalMs()`，`cyclicArmed` 状态与连接状态联动（断开自动停）。`Handler` 侧另有 `echo`、`lineEnding`（无/CR/LF/CRLF）、`dataMode`（UTF-8/HEX）、`checksumMethod`（算法表下标）、`displaySentData()` 回显、发送即入历史。

对照 easyDebug：发送区已有格式/行尾/定时发送，M5 待做命令列表。可借鉴：

| 参考做法 | easyDebug 落点 |
| --- | --- |
| 历史与 pins 分层，pins 不进工作区文件 | `SessionStore`（命令列表）/ `PreferencesStore`（发送历史）的分工已有对应，按此定键名 |
| 循环发送 = 一条 payload + 间隔，断开自动解除武装 | M5 周期任务：用单调时钟、超时跳过积欠 tick 不突发补发（PLAN 已写），再加"端点关闭即解除" |
| 发送内容即入历史并重发 | `PreferencesStore::rememberAddress` 已有地址历史，发送历史同构扩展 |
| 校验和方法随发送区选择 | 发送区可增"追加校验和"选项，算法表复用 `IO::availableChecksums()` 的注册表思路 |
| `SendHistoryItem` 式记录（内容+编码+行尾） | 与 [REFERENCE_BLE_TOOL.md](REFERENCE_BLE_TOOL.md) §6 的清单一致，M3/M5 落地 |

## 10. 入站分路与写路径（对标 M4 桥接）

`DeviceIoRouter`：一份 `std::array<IRawByteTap*, 6>` 的 tap 集合，**由组合根绑定一次、之后只遍历不 resize**；入站走 `onRawDataReceived` 扇出到各 tap 与 ingest binder，`onConsoleDataReceived` 单独一路；出站 `writeToDevice` / `writeAndArmReply` + `pollReplyBuffer`，暂停用 `const std::atomic<bool>&` 引用组合根持有的成员（构造后由组合根填充的"后绑定引用"写法）。

对照 M4：easyDebug 的桥接是"源头限定为真实接收事件、手动发送与桥接 TX 不再次触发路由"，与"注入字节（`tapInjectedBytes`）与接收字节分流"是同一条规则。**tap 数量固定、绑定后不 resize** 这条可以直接抄——它避免了遍历时的生命周期竞态，也让"tap 开销"是编译期常数，符合本工程"每条消息的通知开销"铁律。

## 11. 网络驱动门面

`Drivers::Network` 一个门面管 TCP/UDP/WebSocket/HTTP 四种客户端（都不监听），按 `SocketType` 分派，每种传输在 `Drivers/Network/` 下各自一个翻译单元。三条值得记的约定：

- **枚举编号即持久化编号**：注释明确"这个编号就是工程文件、JSON-RPC API 与 CLI 持久化的 socket type 下标，只追加，永不重排"。easyDebug 的 `PortDraft::Kind` 若进入 `SessionStore` JSON，应照此立规矩（[ARCHITECTURE_REVIEW.md](ARCHITECTURE_REVIEW.md) 已指出用 `currentIndex()` 当 Kind 的风险）。
- **部分写续传**：`m_tcpPendingWrites` 累积未发出的字节，`queueTcpWrite()` 在未连接时入队。easyDebug 的 `write()` 返回本地接受字节数 + Partial 语义是同一问题的另一种解法，可对照确认"队列结算"在断开时怎么做。
- **UDP 细节**：本地端口 + 远端端口分离、组播开关、`enlargeUdpReceiveBuffer()` 主动扩大接收缓冲（高速收包时内核缓冲溢出是丢包的隐形来源）。easyDebug M6 压测时值得补这一项。
- 另有异步 DNS（`AsyncTcpDial` + `lookup()`/`lookupFinished`）与 `preferredAddress()` 选择，对应本项目"TCP 客户端允许主机名并异步解析"。

## 12. 记录与导出（未逐行阅读，仅登记入口）

| 目录 | 内容 | 对标 |
| --- | --- | --- |
| `core/Storage/CSV/` | `Export`（650 行）、`Player`（回放/seek）、`PlayerLoaderWorker`、`SparseRowMerger` | R11 记录格式与流式读取、回放 |
| `core/Storage/Sessions/` | `DatabaseManager/DatabaseWorker`（SQLite 录制）、`Export`、`HtmlReport`、`Verifier/VerifierRegression`、`BlockFingerprint` | R11 记录与校验；**`Verifier` 与 `BlockFingerprint` 是"记录可校验"的现成答案**，M5 需要时优先读它 |
| `core/Storage/Replay/`、`MDF4/`、`InfluxDB/`、`MQTT/` | 回放、MDF4、时序库 sink | 首版不做 |

[REFERENCE_BLE_TOOL.md](REFERENCE_BLE_TOOL.md) §6 已列 JSONL 流式导出作为备选；Serial Studio 给的是 SQLite + 校验器路线。M5 选型时两条都查，按"写盘失败要能明确停止并报告缺口"（PLAN M5）这条硬要求取舍。

## 13. 尚未借鉴、按需参考的能力

| 参考能力 | 位置 | easyDebug 可能的落点 |
| --- | --- | --- |
| 帧校验三态 + 算法注册表 | `FrameReader.h`、`Checksum.h/.cpp` | M3/M4 组帧器（如需校验） |
| hex dump 分栏 + ASCII 列渲染 | `TextFormat::hexDump` | 监视区 HEX 显示增强 |
| 折叠相邻重复行 + 重复计数 | `TerminalBuffer`（`tst_terminal_buffer.cpp`） | 监视区显示开关 |
| 刷新频率可配（默认 60 Hz，1–240） | `TimerEvents`（`uiRefreshRate`） | M6 性能调参项 |
| 解析负载公平份额抽稀 | `ParseBudget` | M4 桥接拥塞治理 |
| 诊断 1 Hz 拉取 + 增量语义 | `LinkStats` | M5 运行状态摘要 |
| 发送 pins/历史/循环/校验和 | `SendLibrary`、`Handler` | M5 命令列表与周期发送 |
| 独占资源前置校验 | `HAL_Driver::exclusiveResource` | A/B 选同一串口的拒绝提示 |
| 记录校验器与块指纹 | `Sessions/Verifier`、`BlockFingerprint` | M5 记录完整性校验 |
| 已知答案测试命名与覆盖点 | `tst_terminal_buffer.cpp`、`tst_frame_reader_modes.cpp`、`tst_console_text_format.cpp` | 组帧器与文本视图的测试用例清单 |

## 14. 明确不迁移的部分

- **许可红线**：GPLv3 / 商业双许可。不复制代码、不搬运实现，只引用设计结论；若将来真要引入某段实现，必须单独评估许可（这也是本项目一贯的"参考工程是语义与教训来源，不是代码基座"）。
- QML/Qt Quick/Qt Graphs 仪表盘与 23 个 widget、Project Editor、Quick Plot——产品形态不同。
- 工业协议（Modbus/CAN/OPC UA/S7/EtherNet/IP/IEC 104）、MQTT/Sparkplug、DBC 导入——easyDebug 首版不做协议解析。
- JS（QJSEngine）/Lua（LuaJIT）脚本解析与转换、AI 助手、gRPC/MCP 自动化接口——首版不做脚本与自动化。
- vendored 依赖（`lib/`）、PGO+LTO 发布流水线、签名与更新器——工程规模与发布形态不同。
- 双许可下的 Pro 模块、试用与激活体系——与本项目无关。

## 15. 使用约定

后续开发遇到组帧、文本视图性能、计数与摘要、发送预设、桥接分路、记录校验类需求时，先查本文档第 6–11 节的对照表；需要细节时按第 1 节的文件清单到参考仓库对应文件取语义，并在迁移提交说明里记录来源路径与指纹。参考仓库不进入本项目工作区（克隆在 `D:/AWork/myself/QTProject/_refs/Serial-Studio`），需要时重新克隆。

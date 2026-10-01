# 参考工程分析与复用方案

阅读日期：2026-10-01。分析对象是本地工作区源码，包含参考工程尚未提交的修改。

## 1. 版本与阅读范围

| 工程 | 路径 | 阅读时 HEAD | 工作区情况 |
| --- | --- | --- | --- |
| debugTool | `D:/AWork/myself/QTProject/debugTool` | `d391e82` | version/version.h 有修改，另有未跟踪归档文件 |
| YmodemTool | `D:/AWork/myself/QTProject/YmodemTool` | `2c2ca24` | mainwindow.cpp/.ui、TCP Client 源码等有未提交修改 |

逐项阅读了两工程的 `.pro`、端口抽象/工厂/管理器、五种端口实现与参数页、主窗口收发接线与配置逻辑。另阅读 debugTool 的 Global、调试日志及数据转换关键实现，以及 YmodemTool 的 README、移植说明、修复说明、Ymodem 状态机与自定义命令转义实现。相同端口文件通过 SHA-256 比对，差异文件通过文本 diff 对照。

与本次设计无直接关系的视频、云台、摇杆、设备参数等模块只检查组织方式和依赖，不作完整业务审计。未运行参考工程、未验证其 README 中的测试声明；本次发现来自源码。相关文件指纹见 [reference-snapshot.csv](reference-snapshot.csv)。

## 2. 现有组织方式

`debugTool` 使用 C++17、Qt Widgets/Network/SerialPort 和 qmake；`port/` 承担多类型端口，`From/` 承担功能页面，`api/` 承担协议及工具，`global/` 提供共享对象。主窗口持有主/从两个端口入口，并通过 Global 访问各功能页。其 `.pro` 同时链接 OpenCV，业务范围大于通用调试工具。

`YmodemTool` 延续同一套 `port/`，`.pro` 选择 C++23，主窗口把端口收发与 `Ymodem : QObject` 连接。Ymodem 通过 `tansferData(QByteArray)` 请求发送，通过 `transferStatusReport` 报告状态，体现了协议层与实际链路分离的方向。

```mermaid
flowchart LR
    Combo[端口类型 QComboBox] --> Manager[PortManager]
    Manager --> Factory[五种 AbstractPortFactory]
    Factory --> Port[AbstractPort QWidget]
    Port --> Page[参数 UI]
    Port --> Device[QSerialPort / Socket]
    Device --> Rx[portReceived QByteArray]
    Rx --> Window[MainWindow]
    Window --> Forward[另一个 PortManager 或协议处理]
```

该图描述参考工程当前实现，不是 easyDebug 的目标结构。

## 3. 可复用思路

| 来源 | 已有能力 | easyDebug 采用方式 |
| --- | --- | --- |
| 两工程 `port/AbstractPort.h` | open/close/write 和接收/状态事件统一 | 保留抽象方向，拆成纯通信接口与纯参数控件 |
| `port/portmanager.cpp` | 工厂注册、下拉框和 QStackedWidget 联动 | 注册表以 PortType 为键，支持多实例，页面按指针选择 |
| `port/serial/serialport.ui` | 串口列表、可编辑波特率、紧凑横向布局 | 作为控件布局基础，补齐串口高级参数 |
| TCP/UDP 参数页 | 地址、端口历史，网卡地址展示 | 保存结构化地址到 itemData，不解析显示文本 |
| debugTool `mainwindow.cpp` | 主从端口双向转发 | 转移至独立 Router，变成可启停的规则 |
| debugTool `From/myDebug/debug.cpp` | 50 ms 批量刷新、最大显示块数 | 采用批量思想，进一步限制入口队列字节和记录数 |
| debugTool 串口实现 | errorOccurred 感知错误，打开时停止扫描 | 保留事件驱动方向，发现服务从 GUI/I/O 线程移出 |
| Ymodem `api/Ymodem/ymodem.*` | 协议状态机与链路分离、定时器按需运行 | 作为未来协议扩展参考；首版只实现通用收发 |
| Ymodem 主窗口 | 指令历史、转义、延迟发送 | 独立为 SendService 和 Scheduler，使用严格字节编码 |

## 4. 必须在新设计中解决的限制

下列定位均以本次工作区内容为准；行号随参考工程后续变更可能变化，函数名与源码指纹可用于复核。

| 定位 | 源码事实与影响 | 设计处理 |
| --- | --- | --- |
| 两工程 `port/AbstractPort.h:7` | 抽象继承 QWidget；后端直接持有控件，不能直接整体移到工作线程 | AbstractTransport 继承 QObject；配置值通过命令传入 |
| debugTool `port/portmanager.cpp:56`；Ymodem 同函数 | setupPort 每次重新 connect，未先断开，反复切换可能重复收到同一事件 | 每实例只安装一次连接，保存 connection 生命周期，回归测试覆盖 |
| debugTool `port/portmanager.cpp:62` | 使用 index+1，与主窗口 `.ui` 的默认占位页耦合 | 使用 PortType→QWidget 映射及 setCurrentWidget |
| Ymodem `port/portmanager.cpp` | 移除默认页后使用 index，修正了该工程的页面偏移 | 参考修正意图，不继续依赖数字偏移 |
| debugTool `mainwindow.cpp:207`、`:446` | 接收槽直接转发另一端，且主端接收同时进行设备协议解析 | Router 独立，UI/协议旁路不能阻塞透明转发 |
| 两工程 `port/tcps/tcpserverport.cpp:170` | 接收信号只传 QByteArray，丢失是哪一个 client 发来的信息 | IngressFrame 带 PeerId 和远端地址 |
| 同文件 `:67`、`:92` | 写入默认广播所有 client；isOpen 只说明 listener 存在 | 分离 Listening 和可写 peer；目标显式选择 |
| 两工程 `port/udps/udpserverport.cpp:60`、`:148` | write 只发给 lastSender，多个来源竞争同一状态 | 固定目的地址或用户选定来源快照，不提供隐式“最后发送方”路由 |
| 两工程 `port/udp/udpclientport.cpp:233` | 读到了 sender/senderPort，但向上仅传 payload | 保留来源与 Datagram 类型 |
| 同文件 `:102`；debugTool `api/common/quitool.cpp:488` | 绑定后的等待分支调用 sleep；Qt 5+ 路径实际是 msleep | 不迁移轮询等待，使用 API 返回值、状态事件和异步计时 |
| debugTool `port/serial/serialport.cpp:72` | TX 先按请求大小计数，再 write；忽略返回值 | 记录 accepted/written/failed，处理短写和错误 |
| 同文件 `:44` | 限制串口 Qt 写缓存为 128 KiB，但没有补发未接受尾部 | 有界应用队列保存 offset，bytesWritten 驱动继续写 |
| debugTool `From/myDebug/debug.cpp:35` | 显示文档有 2000 块上限，但前置 logStrList 无容量上限，刷新 timer 无 parent | 从入口限制内存，批次通知限额，明确 timer 所有者 |
| debugTool `api/common/quitooldata.cpp:301` | HEX 转换先替换非法字符，可能使输入被悄悄改写 | 严格语法与错误位置，非法输入不发送 |
| Ymodem `api/common/quitooldata.cpp:8`、主窗口发送槽 | 转义先生成 QString，再 toLocal8Bit；高位转义字节受编码影响 | 字节转义直接生成 QByteArray，普通文本独立编码 |
| Ymodem `mainwindow.cpp` | 主窗口保存自身历史，但未调用 PortManager 的 loadConfig/saveConfig | SessionStore 统一覆盖所有配置，不依赖控件自己落盘 |

debugTool 中“QSerialPort 默认写缓冲只有 4096 字节”的注释不作为本项目设计依据；写缓存行为以选定 Qt API 和实际短写测试为准。

## 5. 两份端口代码的差异

读取时，`AbstractPort.h`、`portmanager.h`、TCP Server、UDP Client/Server 的 `.h/.cpp/.ui` 以及 TCP Client `.ui` 内容一致。差异集中在：

- `portmanager.cpp`：Ymodem 清理默认 stacked page，索引由 index+1 变为 index。
- 串口：debugTool 已采用错误信号并在打开时停扫描；Ymodem 保留周期扫描和短期修复逻辑，其 `repair_countdown` 为函数静态量，多实例时不适合作为实例状态。
- TCP Client：Ymodem 工作区设置 NoProxy、增加 abort 和 errorOccurred 接线；错误目前主要输出 qDebug，仍需结构化上报。
- 串口 `.ui`：Ymodem 调整字体、高度和波特率控件宽度；新控件采用布局尺寸策略适配缩放。

因此按模块挑选可用经验，不将任一参考工程的 `port/` 原样整体复制。

## 6. 迁移边界

先提取参数页布局、串口/网卡展示方式和必要图标，再重新实现通信生命周期与路由。旧类名、旧配置键、Global 单例和设备协议不进入新核心；`.ui` 使用独立表单类和明确父对象。

迁入源码或资源时，在对应提交说明记录来源工程、路径和源码指纹。参考工程保持原状。本阶段仅产出设计，没有迁入其业务代码或二进制。

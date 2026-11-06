# QQCom 性能验证

性能结论必须附工具链、载荷、速率、历史规模和测量口径。先通过功能回归，再单独运行探针；避免与构建/其他压测同时执行。硬件和长时间测试不能由一次微基准替代。

## 构建探针

在已加载 Qt/MSVC 环境的 cmd.exe、仓库根目录执行。GUI 探针需先生成版本头，见 [开发说明](../docs/DEVELOPMENT.md)。

```bat
mkdir build\framer-perf
cd build\framer-perf
qmake ..\..\tests\perf\receive_framer_perf.pro -spec win32-msvc "CONFIG+=release" "CONFIG-=debug"
nmake release
release\QQCom_framer_perf.exe
cd ..\..
```

其余 `.pro` 分别放在独立构建目录，以相同方式 qmake / nmake，Qt DLL 需在 PATH 或可执行文件旁：

| 工程 | 程序及示例参数 | 测量对象 |
| --- | --- | --- |
| `perf/receive_framer_perf.pro` | `QQCom_framer_perf.exe` | 分片结束符扫描及完整帧批处理，7 次中位数，验证输出长度/帧数 |
| `perf/traffic_perf.pro` | `QQCom_traffic_perf.exe 1024 20 200 1 0` | 实际主窗口/监视面板，载荷/批条数/批次数/拖动开关/投影行数 |
| `perf/traffic_load_perf.pro` | `QQCom_traffic_load_perf.exe 1000000 10000 10 64 1 plain HEX` | 百万条历史、跨线程逐条投递、继续刷新与日志；参数为历史条数/每秒速率/秒数/载荷字节/日志开关/场景/格式 |
| `perf/command_table_perf.pro` | `QQCom_command_perf.exe 250 500` | 指令表删除条数/总行数 |
| `perf/command_refresh_perf.pro` | `QQCom_command_refresh_perf.exe 2000` | 指令表建行、布局、整表刷新及控件数量 |
| `perf/command_panel_perf.pro` | `QQCom_command_panel_perf.exe 500` | 4 组各 500 条指令，60 次勾选编辑、100 次数据快照、5 次无关组改名 |

组帧工程可传 `FRAMER_SOURCE_ROOT=<源码目录>`，目录需含 `app/receiveframer.*` 和 `domain/receiveframing.h`，用同一探针对照旧实现。指令探针另将结果写入当前工作目录的 `QQCom_cmdperf_out.txt` / `QQCom_cmdrefresh_out.txt`。

`command_panel_perf` 直接构建真实面板、管理器及持久化代码，以临时配置隔离日常数据。可传 `COMMAND_SOURCE_ROOT=<src目录>` 对照修改前源码，结果以微秒追加到当前工作目录的 `command_panel_perf.txt`。计时包含同步控件处理和配置序列化；勾选段不处理磁盘刷新或事件循环，改名段包含布局、事件处理和延迟析构。对照应在相同样式、平台、行数及 Kit 下交错运行至少 3 轮，并在构建结束后单独执行。

监视探针需同时看总周期、等待和长尾。所谓“净工作”是总周期减人为等待，等待期间也可能处理插入，不能单看该差值比较不同实现。历史重投影随条数增长；不要由小批次结果推断百万行仍不卡顿。批量文本编辑块应保留，逐字段单独插入会放大文档布局开销。

`traffic_load_perf` 使用独立临时配置、真实面板、带换行载荷和 10 ms 心跳；`lag_*` 是心跳超出预期间隔的延迟，不是每条消息的端到端耗时。`max_backlog` 在心跳采样时比较已投递条数与已入模条数，包含排队信号和攒批，不能视作理论最大队列容量。预填历史直接进入真实模型，随后触发真实「应用过滤」；重建耗时包含尾部 40 ms 等待。场景 `changes` 在接收期间连续改变显示设置并两次应用过滤，检查旧任务取消、相同请求不重启且最终视图一致。场景 `transfer` 使用实际 FileTransferService / EndpointRuntime，把 32 MiB 文件经 TCP 发给独立线程，按字节数和 SHA-256 验收；日志开、关需交错重复比较。本地磁盘结果不能代表网络目录或慢盘。

## 真实链路与事件延迟

以测试配置运行程序，不要用含定时发送、自动记录等设置的日常工作配置做吞吐比较。探针只在显式传参时启用，每轮使用新的输出文件：

```powershell
.\build\release\release\QQCom.exe --auto-open tcp-server:9000 --latency-probe build/probe.txt
python tests/stress/traffic_stress.py --proto tcp --host 127.0.0.1 --port 9000 --rate 500 --size 256 --duration 30
python tests/stress/analyze_probe.py build/probe.txt
python tests/stress/soak_trend.py build/probe.txt --bucket 30
```

`--auto-open` 支持 `tcp-server:<port>`、`udp:<port>`、`tcp-client:<host>:<port>`、`serial:<name>`，只作用于端点 A。UDP 发压改用 `--proto udp`；`--ramp` 将发送速率从 0 线性升至指定值。纯标准库脚本无需 Python 第三方依赖。

报文含 8 字节递增序号、8 字节毫秒时间戳和填充，脚本输出 `sent_packets / sent_bytes / actual_rate / errors`。TCP 为字节流，readyRead/组帧记录数不等于发送次数，按 RX 字节对账；UDP 还需检查来源和报文序号。仅看发送端“无错误”不能证明接收端无丢失。

GUI 探针使用 20 ms QTimer，每 25 次采样输出：

```text
epoch_ms sample_count P50_ms P95_ms P99_ms MAX_ms record_count rx_bytes
```

25 个样本的最近秩 P99 等于 MAX；相邻两行也是最大值型判据，实际覆盖时长在繁忙时会超过一秒。分析脚本的 `sec_window_P99_max` 是历史名称，不是严格的全程 P99。趋势脚本给出各桶最大值，不应将其解读为全桶原始样本的百分位。

建议载荷 64/256/1024 B，速率 100/500/1000/2000/5000/10000 条每秒逐档测试，每档至少 30 秒。以窗口最大延迟超过 50 ms 作为交互卡顿观察线，同时记录字节对账、内存、历史条数和是否开启写盘/换行。记录开关、编码、过滤和组帧配置必须一致。

长稳测试至少一小时，观察固定速率下内存和延迟趋势，并检查切格式、应用过滤、滚动、正常关闭排空。当前入口队列无容量，模型/视图虽有安全阀仍不保证长期总内存有界；历史重建已分批，清空旧文档和慢盘同步日志仍可能出现停顿。测试结束正常关闭窗口，不用强杀来验证退出完整性。

所有原始结果和截图写入 `build/`，不提交临时日志。回归测试及人工硬件验收见 [开发说明](../docs/DEVELOPMENT.md)。

# 自动换行性能：根因、方案选择与实测

> **修订（第二轮）**：初版结论是"换行模式所有行统一行高"，用观感换性能。
> 实际使用后确认这个取舍不可接受——短报文占着长报文的高度，一屏只看得到五六条。
> 本轮重新评估后改为**按内容给每一行定高**，并实测了它的真实代价与"文本刷新"
> 方案的对照，结论见第 1、3、7 节。

本文回答一个问题：**监视区开自动换行就卡，显示区该怎么做？**

结论分两层：

1. **卡顿与"用表格还是用文本控件"无关。** 根因是表格里三处全量工作：逐行测量行高、
   把整段长文本交给委托、预算溢出时整表重置。前两处已经修掉，
   修掉之后**任何**方案都不会再出现整秒冻结。
2. **但"自然行高"在 `QTableView` 里是有代价的。** 表格的行高存在 `QHeaderView` 的
   section 里，一旦逐行不同，视图就离开了它的快路径：实测每帧多 11 ms
   （18–20 ms vs 8 ms，见第 1、7 节）。它不卡（无长帧），但帧预算被吃掉了。
   要同时拿到"自然行高"和"完整帧预算"，只有让显示区自己管布局——
   也就是 klogg 那条路（自定义 `QAbstractScrollArea`），或接受 `QPlainTextEdit` 的取舍。

---

## 1. 先看结论数据

在真实 `MainWindow` + `TrafficPanel` 上，按 16 ms 一拍灌入，测每个刷新周期
（追加 → 入模 → 滚底 → 重绘）的**净工作耗时**（已扣除刻意等待攒批的时间）。
测试机：Qt 6.10.1 / MSVC 2022 / Release / Windows，窗口 1400×900，保留记录 1024–3212 条。
复现方式见 [tests/PERFORMANCE_TEST.md](../tests/PERFORMANCE_TEST.md)。

| 显示形态 | 每帧净工作 | 一屏条数 | 说明 |
| --- | --- | --- | --- |
| 修复前（换行开，逐行重算行高） | 62.1 ms，最差一拍 **3779 ms** | 3 行 × 裁切 | 整秒冻结的来源 |
| 统一行高（初版方案） | **7.6–8.4 ms** | 约 8 条 | 短报文留白，已弃用 |
| **按内容定行高（现方案）** | **17.9–19.9 ms** | 短报文 34 条 / 长报文 8 条 | 自然行高，无长帧 |
| 换行关闭（参照） | 8.0–10.0 ms | 约 26 条 | 无显式行高 |

三个关键读法：

1. **整秒冻结已经消失。** 那 3779 ms 来自旧实现每 200 ms 触发的
   `resizeRowsToContents()`（本机 3212 行一次 2879 ms，约 0.9 ms/行）。
   现在完全不调用它，四档都是"无长帧"（120 拍里 0 拍超过 100 ms）。
2. **按内容定行高比统一行高每帧贵约 11 ms，而且与消息速率无关**：
   每拍 1 条、2 条、5 条、20 条的净工作都是 18–22 ms。
   也就是说这不是"插入开销"，而是视图在"逐行不同行高"下的固有绘制/几何开销
   （同样 104 px 行的统一行高只要 7.6–8.4 ms）。约 50 fps，流畅但不宽裕。
3. **换行关闭反而更快且有更多条数**，因为 33 px 行既没有显式行高、一屏也能放更多条。
   它唯一的缺点是长报文单行省略——这正是换行模式要解决的问题。

> 顺带说明"修复前最差一拍 3779 ms"是怎么来的：旧实现的 200 ms 防抖在持续高速时
> 一直被推迟，数据一停顿就到点，于是每 200 ms 换来一次整秒冻结。**防抖只降低触发
> 频率，不改变单次代价**，两头都不对。

---

## 2. 根因：三处，都不是"表格"的错

### 2.1 逐行测量行高：O(行数) × 整段文本布局

`resizeRowsToContents()` 会对**每一行**（包括屏幕外的行）调用委托的 `sizeHint()`，
而换行模式下的 `sizeHint()` 会为**整段**单元格文本建立折行布局。
实测增长曲线（1 KB 报文 → 3071 字符 HEX，本机）：

| 行数 | 一次 `resizeRowsToContents()` | 每行 |
| --- | --- | --- |
| 250 | 283 ms | 1.13 ms |
| 1000 | 1182 ms | 1.18 ms |
| 2000 | 2302 ms | 1.15 ms |
| 4000 | 4976 ms | 1.24 ms |

严格线性，且与"屏幕上有几行"无关。旧实现用 200 ms 防抖来缓解，
但防抖只是降低**触发频率**：数据一停顿定时器就会到点，于是每 200 ms 换来一次整秒冻结；
持续高速时它反而一直被推迟，行高停留在估算值上，长内容被裁掉。
**两头都不对。**

这一点在 Qt 侧也是已知的、且官方尚未提供 API：

- [QTBUG-144127](https://qt-project.atlassian.net/browse/QTBUG-144127)（**Reported，未解决**）
  "Add a way to perform automatic resizing only on the visible columns and rows"——
  原话就是 `resizeRowsToContents()` 无法只处理可见行。
- [QTBUG-100818](https://qt-project.atlassian.net/browse/QTBUG-100818)（Closed/Invalid）里，
  维护者确认 `sizeHint` 会被**重复冗余调用**（5×5 模型的 2×2 可见区域触发了 6 轮全表询问）。
- `QTableView` **没有** `setUniformRowHeights()` 这类开关——那是 `QTreeView` / `QListView`
  （`uniformItemSizes`）才有的；表格的行高是按 section 存在 `QHeaderView` 里的，
  没有"统一高度"的快路径。唯一便宜的路径就是固定 `defaultSectionSize`。

同类症状的外部佐证：

- Qt Forum [topic/135107](https://forum.qt.io/topic/135107/how-to-display-multiline-messages-in-qtableview)
  同一套组合（`QAbstractTableModel` + `QSortFilterProxyModel` + `QTableView`）：
  "It shows multilines but it will slow down GUI. I can't even scroll down when there are lots of rows."
  Qt Champion 的回答："Don't use ResizeToContent as it is recalculating everything for each cell."
- [QTBUG-8540](https://qt-project.atlassian.net/browse/QTBUG-8540)：
  "Using ResizeToContents with a QSortFilterProxyModel is far from optimal"（本项目的组合正是如此）。

### 2.2 单元格文本长度 = 每帧成本

委托拿到的文本有多长，每帧就要布局多长——**跟换行开关无关**。
固定行数、固定行高，只改文本长度，实测单次整视口重绘：

| 单元格文本 | 1 KB 报文 | 4 KB 报文 |
| --- | --- | --- |
| 60 字符 | 7.0 ms | 6.4 ms |
| 300 字符 | 9.4 ms | 10.8 ms |
| 900 字符 | 8.4 ms | 18.8 ms |
| 不裁剪（3071 / 12287 字符） | 54 ms | **83 ms** |

换行关闭、不裁剪时更糟：1 KB 报文 99 ms，4 KB 报文 **441 ms**（2.3 fps）。

### 2.3 预算溢出时整表重置

原 `appendRecords()` 一旦因字节预算丢弃最旧记录，就走 `beginResetModel()`。
记录写满之后**每一批追加都会重置整表**：视图丢弃选中项与滚动位置，
`QHeaderView` 重新初始化，并且（配合 `ResizeToContents` 时）重新触发全表测量。
而渲染缓存当时是按**行号**索引的，行号一平移就整体失效。

---

## 3. 那"文本刷新"方案呢？

### 3.1 调研：快日志查看器确实都用文本式渲染

| 项目 | 做法 |
| --- | --- |
| **Qt Creator** 输出面板 | 直接继承 `QPlainTextEdit`（`Core::OutputWindow`），外加分块 + 自适应节流 + 洪泛丢弃 |
| **Qt 官方文档** | `QPlainTextEdit` "replaces a pixel-exact height calculation with a line-by-line … scrolling approach … still resize the editor with **line wrap enabled in real time**"；"`setMaximumBlockCount()` + `appendPlainText()` turns QPlainTextEdit into an **efficient viewer for log text**" |
| **glogg / klogg** | 自定义 `QAbstractScrollArea`，**结构化数据仍在自己的模型里**（`AbstractLogData*`），只对可见行做折行并缓存成 pixmap |

klogg 那一套正是"内存保持结构化 + 文本式渲染"的形态：**折行高度只在绘制可见行时算**，
`finalLineHeight = fontHeight × wrappedLinesCount()`，滚动到底就 `break`，
没有全局行高模型、没有 `resizeRowsToContents` 的对应物。这就是文本方案快的原因，
而且它是**结构性**的：`QPlainTextDocumentLayout` 的高度就是行数而不是像素
（`documentSize()` 直接返回 `document()->lineCount()`），块布局按需计算并缓存在
`QTextBlock::layout()` 里，所以"文本控件能实时换行"是设计使然，不是调优出来的。

### 3.2 文本刷新方案与表格的实测对照

同一台机器、同样内容。指标是**稳态下追加 1 条 → 滚底 → 重绘的每帧净工作**，
随已保留行数变化（这才是"跑久了会不会卡"的判据）：

| 保留行数 | 表格·统一行高 | **表格·按内容定行高** | 文本·纯追加 | 文本·块上限 | 文本·分块淘汰 |
| --- | --- | --- | --- | --- | --- |
| 1 000 | 12.2 ms | 17.3 ms | 13.1 ms | 18.1 ms | 14.7 ms |
| 5 000 | 19.5 ms | 20.4 ms | 14.5 ms | 19.6 ms | 15.1 ms |
| 20 000 | 19.4 ms | **19.3 ms** | 14.5 ms | 20.7 ms | **13.1 ms** |
| 工作集（20 000 行） | 121 MiB | 121 MiB | 261 MiB | 260 MiB | 262 MiB |

（1 KB 报文。文本控件必须再存一份渲染后的文本，所以同进程对比下多占约 140 MiB；
绝对值含未归还的已释放内存，但三档行数下趋势一致。）

四点结论：

1. **`QPlainTextEdit` 的追加成本不随历史长度增长**，而且 `setMaximumBlockCount`
   是个陷阱——Qt 文档推荐它做日志查看器，但实测它每帧要 18–21 ms（最差 45 ms），
   比手动分块淘汰（13–15 ms）更差。要用文本控件就该用**分块淘汰**。
2. **内存是文本方案的硬伤**：约 140 MiB 的额外占用，换来的只是"渲染文本的副本"，
   而本项目明确要求内存维持结构化保存。
3. **在 `QTableView` 里，"按内容定行高"比"统一行高"贵约 11 ms/帧**，
   与消息速率无关。原因是行高存在 `QHeaderView` 的 section 里：
   一旦逐行不同，视图就离开"所有行等高"的快路径，绘制与几何计算都要走通用分支。
   它不卡（无长帧），代价是帧预算从 ~8 ms 变成 ~19 ms。
4. 所以要**同时**拿到自然行高与完整帧预算，只能让显示区自己管布局：
   即 klogg 那条路（自定义 `QAbstractScrollArea`，只对可见行算折行并缓存）。
   `QPlainTextEdit` 是它的现成近似版，代价是丢列、丢结构化过滤、多 140 MiB。

### 3.3 什么时候值得换成文本视图

现在可以说得更准：**当你既要自然行高、又要保住帧预算时，就值得。**
`QTableView` 的两条路都不完美：

- 统一行高 → 快，但短报文留白（已实测被否）；
- 按内容定行高 → 自然，但每帧多 11 ms。

本项目当前采用后者：先拿到观感，代价是约 50 fps 的上限（无长帧，不卡）。
若将来需要在这之上再要余量（例如窗口更大、报文更长、或要开多个监视区），
再把它换成自绘视图——那时结构化数据、过滤、导出都不动，只换渲染层。

真要做的话，正确形态是 klogg 那套：
自定义 `QAbstractScrollArea`，结构化记录留在现有模型里，只对可见行算折行 + 缓存，
滚动按行而不是按像素（这也是 `QPlainTextEdit` 与 Notepad++ 的做法）。

---

## 4. 已实施的修改

### 4.1 换行模式：按内容给每一行定高，但绝不测量文本

`applyWrapMode()` 把行高基线设成"一行文本"，再由 `refreshRowHeights()` /
`applyRowHeights()` 按各条内容的实际行数把行调高：

- **行高在插入时算一次**，不是每帧算。新增行只在 `m_filter` 的 `rowsInserted`
  里按新行区间设定；过滤/排序让"视图行 → 记录"的对应关系整体变化时，
  才在 `layoutChanged` / `modelReset` 上全表重排一次。
- **行数用算术折算，不做文本测量**：模型按消息 id 缓存"渲染文本有多少字符"
  （`m_renderLengths`），行数 = `ceil(字符数 / 每行字符数)`，再钳到 1..6 行。
  所以列宽变化引起的全表重排是纯算术，不会退化成逐行文本布局——
  后者正是"开了换行就卡"的根因（`resizeRowsToContents()` 实测约 0.9 ms/行）。
- **`resizeRowsToContents()` 与 200 ms 防抖定时器已彻底删除。**

代价（已在第 1、3 节实测）：逐行不同的行高让视图离开"所有行等高"的快路径，
每帧比统一行高多约 11 ms。相对旧实现仍是数量级的改善：旧实现最差一拍 3779 ms，
现在 120 拍里 0 拍超过 100 ms。

### 4.2 单元格文本按列宽裁剪（`TrafficModel::setTextBudget`）

模型为数据内容列维护一个字符预算，`renderDataColumn()` 渲染后按预算截断并补省略号。
预算由面板按**真实列宽**算出（`updateTextBudget()`），列宽变化时防抖重算：

```
预算 = max(128, 每行字符数 × 可见行数 × 90%)
```

两条边界都是有理由的，不是拍出来的：

- **乘 90%**：折行按词边界断行，比按字符数估算多占位置；取满额会把省略号挤到
  可见区之外，长报文看起来就像"到这里就结束了"。少画几个字符，换"截断可见"。
- **下限 128 字符**：这个量级的布局开销可以忽略（实测 60 字符整视口 7 ms、
  300 字符 13 ms、900 字符 40 ms）。窄列里按容量算出的预算会小到把一条
  "本来只是被省略显示"的短报文改成"看起来被截断"——那是把渲染细节
  泄漏成了数据事实。128 字符（HEX 下 64 字节）以内一律不裁。

行数折算另有一个 92% 的折扣（`WrapColumnEstimatePercent`）：按字符数折算是下界，
少算一行就会把最后一行裁掉，宁可多给一点高度。

**裁剪的只是绘制用的那一份。** 原始字节、真实长度、悬停提示的完整 HEX、
右键复制、导出、以及搜索匹配（`TrafficFilter` 直接读 `record.raw` 现渲染）
全部仍是完整内容，符合 `ARCHITECTURE.md` §3"原始字节是唯一事实依据、派生文本现算"。

### 4.3 预算溢出改为增量通知，缓存按消息 id 索引

`appendRecords()` 现在用 `beginRemoveRows` + `beginInsertRows` 两段增量通知，
不再整表 `beginResetModel`；并且**先**把本批计入总量再算出包含本批的淘汰量，
所以落定后一定在预算内（先裁后插会超出整整一批，把硬约束变成软约束）。
字节总数改为运行累计（`m_rawBytes`），不再每次追加都遍历全表求和。

历史上限补上了**条数**这一维（`HistoryRowBudget = 20000`，与
`ARCHITECTURE.md` §7.1 的"16 MiB 或 20000 条"一致）。只限字节是不够的：
串口遥测常见几十字节的小帧，光靠 4 MiB 能堆到六万条以上，
而每条 `Message`（时间、来源、三个 QUuid、两个 QString）的元数据开销
远大于它那 64 字节载荷——"限了字节"不等于"限了内存"。

渲染缓存键从**行号**换成**消息 id**（`Traffic::Message::id`，会话内唯一、
满足 domain 里"UI 稳定 key"的定义），因此淘汰旧记录造成的行号平移不会让缓存失效。

### 4.4 列宽测量只看新记录

`fitFixedColumns()` 原本每次追加都遍历全部记录。端点标签与对端地址的最大宽度
只会随新记录增长，改为只量 `id` 大于上次扫描位置的新记录。

### 4.5 摘要标签不再重复赋值

`QLabel::setText()` 每次都重算尺寸并请求重绘；16 ms 一拍的节奏下，
文案没变就不赋值。

---

## 5. 验证

功能回归（`tests/ui`，真实 Windows 平台与 offscreen 各跑一遍）：**55 passed / 0 failed**；
核心传输测试 **10 passed / 0 failed**。
其中三个用例是这次新加/改写的契约守卫：

- `trafficWrapSwitchAdjustsView`：换行开启后，**每一行的行高必须等于"该行内容
  需要的行数 × 行高单位 + 绘制余量"**（逐行独立，不是全表一个常数）——
  有人退回统一行高、或把行高算错，这个用例都会失败；同时校验 1 KB 报文
  确实比十几字节的示例报文占更多行，且行数被 6 行上限钳住。
  关闭换行后所有行必须回到统一 33 px。
- `hugeMessagesRenderBoundedTextOnly`：4 KB 报文进模型后，
  绘制文本必须显著短于完整 HEX 且以省略号结尾，同时
  `record.raw` 仍是 4096 字节、悬停提示仍含完整 HEX。
- `historyBudgetAlsoLimitsRowCount`：三万条 2 字节小报文之后，
  模型必须停在 20000 条，且保留的是最新的一批。

> 该用例在离屏平台（Qt 不再自带字体，字体度量为 0）下不校验"行数 → 像素高度"
> 这一层，因为那里行高会退化成只剩余量；判定改成直接比对模型给出的行数。
> Windows 平台跑的是完整校验。

性能基准（`tests/perf`，见 [tests/PERFORMANCE_TEST.md](../tests/PERFORMANCE_TEST.md)）
给出第 1 节的对照数据。

另外做了出货程序的渲染核对：用 `easyDebug.exe --sample --snapshot <path>`
在真实 Windows 平台下出图（离屏平台没有字体，只能看几何、看不到文字）。
换行开启后，十几字节的示例报文各自只占一行，观感与关闭换行时一致，
不再有"短报文占着 6 行高度"的留白；端口/对端列各自单行不折行，
字节流片段仍带 `⏎` 续接标记。

重新出图：

```powershell
cd build\release\release
$env:QT_QPA_PLATFORM='windows'
.\easyDebug.exe --sample --snapshot D:\tmp\wrap.png
```

（换行开关读的是 `%LOCALAPPDATA%\easyDebug\easyDebug\preferences.ini` 里的
`[traffic] wrap`，出图前先设成想要的状态。）

---

## 6. 仍然待办 / 已知边界

- **每帧多约 11 ms**：逐行不同的行高让 `QTableView` 离开等高快路径，
  约 50 fps 上限。不卡（无长帧），但余量不多。要拿回余量只能换自绘视图（第 3.3 节）。
- 行数用"字符数 ÷ 每行字符数"折算，对词边界折行是下界，已打 92% 折扣兜底；
  极端情况下（超长不可断词）仍可能差一行，被悬停提示覆盖。
- 字符串预算用等宽字符宽度估算，遇到大量宽字符（中文/全角）会偏保守，
  即比实际可显示的少一点。宁可少显示也不多给：多给就是白算布局。
- `m_renderLengths` 按消息 id 缓存，淘汰记录时同步回收；格式切换时清空。
- 未采集长稳（数小时持续高速）数据；M6 压测时补。
- 未接真实 COM1 设备：本次验证走的是出货代码的真实入口（`appendMessages`，
  即 `MainWindow::handleMessage` 调用的那一个）加载荷，加上单元测试与出图核对。
  真机观感需在实机上确认。

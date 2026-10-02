# easyDebug 工程文件架构评审

评审日期：2026-10-02。评审对象：`D:/AWork/myself/QTProject/easyDebug/easyDebug`（HEAD `8de9bb1`，工作区干净）。
参考工程：`debugTool`（HEAD `d391e82`）、`YmodemTool`（HEAD `2c2ca24`），均为本地工作区源码。
评审范围：目录与构建组织、模块依赖方向、分层边界、工程化配套。不评估尚未实现的通信运行时（M1–M6）。

## 0. 总体评价

**架构方向正确，且明显优于两个参考工程；当前文件组织的问题集中在"分层已经画好但物理目录没有跟上"，属于 M1 开工前必须收口的债，不属于架构错误。**

三个最值得肯定的地方：组合根真正做到了只装配不实现；`PayloadCodec` 成为编解码唯一真源；参考工程评审（`docs/REFERENCE_REVIEW.md`）带着 SHA-256 指纹，把"凭记忆抄"变成了"可复核"。这三点参考工程都做不到——`debugTool` 依赖 `global/global.h` 单例、`AbstractPort : QWidget` 让通信与控件无法分离、HEX 转换会悄悄改写用户输入。

按评审清单逐项过完，**没有发现会导致运行时错误或崩溃的正确性缺陷（🔴 0 项）**。发现 4 项重要问题（🟠）和 8 项建议（🟡），全部是结构与可维护性问题，无一需要改动已有功能逻辑。

## 1. 与参考工程的对比

### 1.1 物理组织

| 维度 | debugTool / YmodemTool | easyDebug | 判定 |
| --- | --- | --- | --- |
| 目录含义 | `port/` 按端口类型分（serial/tcpc/tcps/udp/udps），`From/` 按功能页面分，两套维度混在一层 | `src/` 下按**技术职责**分（domain/services/ui），`ui/` 再按面板分 | ✅ 更好 |
| 端口抽象 | `AbstractPort : public QWidget`，后端直接持有控件 | 草稿 `PortDraft` 是纯值对象，不含 QWidget | ✅ 已修正 |
| 全局状态 | `global/global.h` 单例贯穿各页面 | 无单例，组合根显式注入 | ✅ 已修正 |
| UI 构建 | 大量 `.ui` 表单 + 手工 connect | 代码构建，`.pri` 模块化，`tests` 复用同一批 `.pri` | ✅ 更可测 |
| 构建描述 | 40+ 条 `SOURCES` 平铺在 `.pro` | 拆成 `ui.pri` / `services.pri`，主 `.pro` 只做装配 | ✅ 更好 |
| 测试 | 无（仅一个游离的 `test_append.cpp`，未接入 `.pro`） | `tests/ui/` 独立 Qt Test 工程，18 个用例 | ✅ 更好 |
| 第三方耦合 | `.pro` 硬编码 OpenCV 绝对路径 `D:/openLib/...` | 无 | ✅ 更好 |

### 1.2 依赖方向

参考工程是 `port/ → QWidget`、`MainWindow → PortManager → 端口控件` 的双向缠绕，`AbstractPort::loadConfig/saveConfig` 让控件自己落盘。easyDebug 当前的实际依赖方向是正确的单向：

```
main.cpp → mainwindow.cpp → panels → common / domain
                                    ↘ discovery, persistence
domain 不含任何 Qt Widgets 头
```

`src/domain/` 4 个文件全部只依赖 QtCore，验证通过。`PortDiscoveryService`、`PreferencesStore` 均不依赖 QWidget，符合 `ARCHITECTURE.md` §2 的约束。

### 1.3 值得肯定的地方

- **`PortDiscoveryService` 用 `QFutureWatcher` + 值快照**，只在列表真变化时发信号（`portdiscoveryservice.cpp:26-31`），并在 lambda 里不捕获任何外部对象——这正是 `REFERENCE_REVIEW.md` 第 46 行对 debugTool "发现服务移出 GUI/I/O 线程" 的要求，已落实。
- **`PreferencesStore` 的地址校验复用 `domain/networkaddress.h`**，不重新实现一套正则；历史条目过滤无效值、去重、限 20 条（`preferencesstore.cpp:31-40`）。
- **`updateValidation()` 会在高级面板折叠时自动展开它**（`portselectorwidget.cpp:673-680`），避免了错误信息藏在折叠区里无法触达——这是从真实使用角度想出来的设计。
- 测试用例 `portControlsShowTheirOwnContent()` 遍历 2 端点 × 4 类型 × 折叠/展开，用 `SC_ComboBoxEditField` 断言内容绘制区容得下最宽值。这种"控件自己保证内容可见"的回归防线，参考工程完全没有。

## 2. 问题清单

### 🟠 [重要] 1. `src/services.pri` 把三个不同抽象层挤进一个构建单元

**位置**：`src/services.pri:5-13`

**问题**：一个 `.pri` 同时编入 `domain/`（纯值类型）、`discovery/`（后台服务）、`persistence/`（QSettings 存储），并在文件头声明 `QT += serialport concurrent`。后果是 `domain/` 无法被单独使用——任何只想引用 `PortDraft` 或地址校验的单元，都被迫链接 Qt SerialPort。

**影响**：与 `ARCHITECTURE.md` §11 规划的分层不一致；M1 要写不依赖 GUI 的核心测试时（`TEST_PLAN.md` §1 明确要求"测试核心不得需要创建 MainWindow"），无法只链接 domain。

**建议**：拆成三个 `.pri`，`services.pri` 改为聚合入口：

```qmake
# src/domain/domain.pri —— 纯值类型，无 GUI 依赖
INCLUDEPATH += $$PWD/..
SOURCES += $$PWD/networkaddress.cpp
HEADERS += $$PWD/networkaddress.h $$PWD/portdraft.h $$PWD/serialportdescriptor.h
```

```qmake
# src/services.pri —— 聚合入口，保持现有 include 方式可用
include($$PWD/domain/domain.pri)
include($$PWD/discovery/discovery.pri)
include($$PWD/persistence/persistence.pri)
```

`QT += serialport concurrent` 只留在 `discovery.pri`（只有它用 `QSerialPortInfo` 和 `QtConcurrent`），`concurrent` 其实只在 discovery 用到，`persistence` 并不需要。

### 🟠 [重要，需要决策] 2. `PayloadCodec` 的归属是"可争议的取舍"，不是单纯的错误

**位置**：`src/ui/common/payloadcodec.{h,cpp}`，`src/ui/ui.pri:4`

**事实**：这个模块只做字符串↔字节转换，不含任何 QWidget；`ui.pri:4` 为它在文件头声明了 `QT += core5compat`，导致**整个 UI 模块被强制依赖 QtCore5Compat，只为了一个 GBK 编解码**。

**⚠️ 这里存在一次既往决策，本次评审不推翻它。** 上一轮多角色评审明确记录过判断：「PayloadCodec 留在 `ui/common` 是正确决定（render/formatName 是显示层语义），API 形状不动」。这个理由站得住——`formatName()` 返回的是组合框项文本，`render()` 服务于表格列显示和悬停提示，两者都确实是显示层关注点。

**但代价在三处，且都在 M1 之后才会显形**：

1. `ARCHITECTURE.md` §9 把 `PayloadEncoder` 归在 `sending/`，§8 的 `CaptureWriter` 和未来的 `protocols/` 都要复用它。到那时非 UI 模块需要 include `ui/common/`，形成 `sending → ui/common` 的反向依赖。
2. `core5compat` 挂在 `ui.pri` 顶部意味着删掉 GBK 格式也要拖着一个 Qt 模块进来。
3. 上一轮把 `QT += core5compat` 从两个 `.pro` 下沉到 `ui.pri`，理由是"哪个 .pri 的代码依赖哪个模块就由哪个 .pri 声明"——这个原则本身正确，但执行结果把 codec 的依赖扩散到了 traffic/sending/routing 三个面板。

**建议**（保持 API 与调用点不变，只动归属）：

- **最低成本版**：把 `QT += core5compat` 从 `ui.pri` 挪进一个只含 codec 的 `.pri`，UI 其余部分不再被强制依赖。改 4 行，立刻消除扩散。
- **完整版**：在 M1 落地 `sending/` 时，把 `parse()` 和 `render()` 的**声明**移到 `src/codec/`，`ui/common/` 只留一层薄薄的 `formatName()` 与组合框填充适配。此时"显示层语义"和"字节规则唯一真源"两个诉求同时满足。
- **不建议现在做**：M0 阶段移动文件会同时改 17 处 include 和两个 `.pri`，收益（架构纯度）远小于成本（引入无功能变化的回归面）。等 `sending/` 真正存在时再做，届时有真实的调用方来验证归属是否合适。

### 🟠 [重要] 3. 三个面板横向依赖 `portselectorwidget.h`

**位置**：`trafficpanel.h:5`、`sendpanel.h:4`、`routeeditorwidget.h:4`

**问题**：三者 `#include "../portselector/portselectorwidget.h"`，但它们真正需要的只是里面声明的**自由函数**（`endpointLabel`、`fitComboToItems`）和 `PortDraft` 类型。`portselectorwidget.h` 还拖进来 `QIcon`、`QSet`、两个 domain 头。

**影响**：`portselectorwidget.cpp` 已 682 行、21 个成员，是最重的文件；三个互不相干的面板编译时都依赖它。将来加 `ui/pages/` 子目录时，会形成 `pages → panels/portselector` 的反向依赖，破坏 `PLAN.md` §4 规定的 `ui/pages` 与 `ui/panels` 同级关系。

**建议**：把这 7 个自由函数（`portKindName`、`endpointLabel`、`portDraftSummary`、`portDraftDetails`、`portActionName`、`portActionIconName`、`fitComboToItems`）拆到 `src/ui/common/endpointformat.h`（纯函数，无 QWidget）和 `src/ui/common/combometrics.h`。注意 `portselectorwidget.h:19-21` 的注释已经说明了 `endpointLabel` 是跨面板概念，代码位置没跟上注释的意图。

### 🟠 [重要] 4. `mainwindow.h` 用项目根相对路径，破坏了 UI 层可复用性

**位置**：`mainwindow.h:5`

```cpp
#include "src/ui/panels/portselector/portselectorwidget.h"
```

**问题**：面板之间用 `../portselector/portselectorwidget.h` 相对引用，只有 MainWindow 用了 `src/` 开头的根相对路径。这依赖 include path 里恰好有项目根。

**影响**：`tests/ui/ui_smoke.pro:7` 必须写 `INCLUDEPATH += ../..` 才能编过——测试工程为了迁就一个头文件的路径写法而改动构建配置。`ARCHITECTURE.md` §11 规划了 `ui/shell/` 目录（`PageHost`、`PageRegistry`），MainWindow 迁进去后这个 include 立刻失效。

**建议**：统一在 `ui.pri` 里加 `INCLUDEPATH += $$PWD/../..`，全部改成 `ui/panels/portselector/portselectorwidget.h` 风格。测试工程的 `INCLUDEPATH` 就能删掉。

### 🟠 [重要] 5. 三个 common 组件零引用，且是自绘控件

**位置**：`src/ui/common/elidedlabel.h`、`contentscrollarea.h`、`distributedlabel.h`

**问题**：三者已登记进 `ui.pri` 和 git，但全工程零引用（`grep` 结果为空）。`DistributedLabel` 和 `ElidedLabel` 都覆写了 `paintEvent` 自绘文字。

**影响**：违反项目自己的铁律——「所有控件优先使用 Qt 自带控件与自身属性解决外观，不轻易自绘控件」。自绘 QLabel 绕过了 QSS、深色调色板和字体回退（`MEMORY.md` 里已记录本机是深色主题，深色下调色板会下发给控件）。而且三份文件是"已提交但没人用"的资产，读者无法判断是"为后续页面预留"还是"废弃代码"。

**建议**：二选一，别悬着。要么在 `common/` 加一行注释说明"为 M5 记录页/设置页预留，落地时须先在 windows 平台验证深色外观"；要么直接删掉，`contentscrollarea.h` 在设计文档里没有任何对应条目，删除风险为零。

### 🟡 [建议] 6. `easyDebug_zh_CN.ts` 是空文件，但 i18n 流程已铺开

**位置**：`easyDebug_zh_CN.ts`（97 字节）、`main.cpp:31-38`、`resources/resources.qrc`

`main.cpp:35` 从 `:/i18n/` 加载翻译，但 `resources.qrc` 里**没有 `i18n` 资源条目**（只有 styles 和 icons）。`TRANSLATIONS += easyDebug_zh_CN.ts` 配 `CONFIG += lrelease embed_translations` 走的是另一条编译路径。当前效果：所有 `tr()` 调用返回原文——对中文界面是正确行为，但翻译流程实际从未被验证过。

**建议**：`PLAN.md` 里"语言"是 M5 的偏好项。现在要么删掉空的 `.ts` 和 `main.cpp` 的 translator 段，要么在 `.qrc` 里补 `<file>../easyDebug_zh_CN.ts</file>` 并加一条 `lrelease` 冒烟验证。**不要维持现状**——空 `.ts` 会让人误以为翻译链路是通的。

### 🟡 [建议] 7. `PortDraft::Kind` 枚举值与下拉框索引强耦合

**位置**：`portselectorwidget.h:32-38`、`portselectorwidget.cpp:195`、`:590`

```cpp
m_type->addItem(portKindName(i), i);   // :195
result.kind = m_type->currentIndex();  // :590
```

`ARCHITECTURE.md` §12 决策记录明确写了「UDP 统一一种端口」时靠"类型标识"而非索引来保证含义稳定，`REFERENCE_REVIEW.md:58` 也专门批评了参考工程「使用 index+1，与主窗口 `.ui` 的默认占位页耦合」。当前实现把 `currentIndex()` 直接当 `PortDraft::Kind` 用——**一旦将来在类型下拉框里插入"最近使用"分组或排序，`values` 里所有已保存配置的语义会静默错位**。

**影响**：目前 UI 是写死的 4 项固定顺序，暂时安全。但这是设计文档已经识别、代码还没修的债——M5 的 `SessionStore` 要把 `PortDraft` 持久化成 JSON，那时 `kind` 已经是磁盘格式，改不动了。

**建议**：`m_type->addItem(name, int(PortDraft::Serial))` 显式绑定，并用 `currentData().toInt()` 反读，不再用 `currentIndex()`。改动约 3 行，永久消除索引与枚举的隐式绑定。

### 🟡 [建议] 8. 四个 `.pri` 的职责划分与文档不一致

`easyDebug.pro` 目前 include `src/ui/ui.pri` + `src/services.pri`。问题 1、2 修完后，`.pri` 列表会变成 `ui.pri / codec.pri / domain.pri / discovery.pri / persistence.pri`。此时应同步更新 `ARCHITECTURE.md` §11 的目录树，把已落地的模块标出来——目前文档里 `src/app`、`src/transport`、`src/routing`、`src/sending`、`src/monitoring` 全部为空目录占位（代码里根本没建），读者分不清"待建"和"漏了"。

### 🟡 [建议] 9. `mainwindow.cpp:165` 的 `findChild` 无空指针保护

```cpp
void MainWindow::showRoutes() { m_bridge->findChild<QWidget *>("bridgeButton")->setFocus(); }
```

`findChild` 找不到时返回 `nullptr`，直接 `->setFocus()` 会崩。当前 objectName 确实存在（`routeeditorwidget.cpp:65`），但这是靠字符串跨文件约定的隐式契约——改 objectName 或重构路由面板时崩在 `--routes` 命令行路径上。

**建议**：加 `if (auto *b = m_bridge->findChild<QWidget *>("bridgeButton")) b->setFocus();`。同样的模式在 `portselectorwidget.cpp:651` 用的是 `findChild` + 三元判空，做法不一致。

### 🟡 [建议] 10. 测试文件有未使用的 include

`tests/ui/tst_ui.cpp:20` 的 `QRadioButton` 和 `:28` 的 `QTabWidget` 实际有用（`:793`、`:807` 断言它们**不存在**），但 `QActionGroup`（`:11`）、`QPasteEvent` 在文件里已经没有了。这些 include 是把 `QCheckBox` 改成可勾选 `QPushButton` 时留下的残留——反映了这轮改动做过、没清理。

### 🟡 [建议] 11. 无格式与静态检查配置

参考工程也没有，但 easyDebug 的文档质量明显更高，加上收益更大。缺 `.clang-format`（代码里已有大量 4 空格、`QStringLiteral` 包裹字面量的隐式约定，靠人维持）、`.editorconfig`、`.gitattributes`。`git log` 显示只有 4 个提交、代码风格已经明显漂移过一次（`src/ui/panels/traffic/trafficpanel.cpp` 是 2 空格缩进，其余是 4 空格）。

### 🟡 [建议] 12. 版本生成在 `.pro` 里用 `error()` 硬失败

`easyDebug.pro:13-15` 在 `win32 {}` 里跑 `update_version.ps1`，失败即 `error()` 终止 qmake。这让**任何非 Windows 平台无法 configure 项目**，即使 `version/version.h` 已在仓库中（虽然它被 `.gitignore` 排除了，见 `.gitignore:66`）。

**建议**：改为警告 + 复用已有 `version.h`，或在非 Windows 平台跳过脚本。当前主验收环境是 Windows，优先级低，但"跨平台"这个说法一旦写进 README 就会有人踩。

## 3. 是否满足需求

对照 `PLAN.md` §2 的 R01–R11：

| 需求 | 架构是否支撑 | 说明 |
| --- | --- | --- |
| R01 固定双端点 | ✅ 满足 | `mainwindow.cpp:59-79` 循环建两个 `PortSelectorWidget`，身份用 `endpointLabel(index, kind)` 派生，不可编辑 |
| R02 串口 | ✅ 架构就绪 | 后台扫描 + 拔插标记 + 保留不可用选择（`portselectorwidget.cpp:501-505`）已实现；通信按 M2 |
| R03–R05 TCP/UDP | ✅ 架构就绪 | 字段、校验、本地/远端地址分组、`udp.target` 可空均已就位；`UdpTransport` 统一设计见 ARCHITECTURE §6.2 |
| R06 实时显示 | ✅ 架构就绪 | `TrafficModel` + `TrafficFilter` + `QTableView` 骨架、批量重置、有界缓存（2000 条 / 4 MiB）已实现 |
| R07 A/B 桥接 | ✅ 架构就绪 | `RouteEditorWidget` 方向 + 启停 + 端点变更自动失效（`routeeditorwidget.cpp:90-99`）已实现 |
| R08 高效流转 | ⚠️ 未开始 | `app/` / `transport/` / `routing/` / `monitoring/` 均未建目录。这是最大的未完成面 |
| R09 自定义发送 | ✅ 架构就绪 | 七种格式共用 `PayloadCodec`，预览严格用 `parse` 结果（`sendpanel.cpp:163-169`） |
| R10 配置 | ⚠️ 部分 | `PreferencesStore` 覆盖轻量偏好；`SessionStore` 属于 M5 |
| R11 记录诊断 | ⚠️ 未开始 | `CaptureWriter` 属于 M5 |

**结论：需求覆盖上，架构是"够用且不返工"的**——M1 要加的 `AbstractTransport`、`Router`、`SendQueue` 都能落在现有分层空位上，不需要推倒重来。两个真正会导致返工的点已经标出：**问题 7（`kind` 索引耦合）必须在写 `SessionStore` 之前修**；**问题 1、2（`.pri` 拆分与 codec 归位）必须在 M1 开工前修**，否则新模块会继续往错误的方向依赖。

## 4. 写得好的地方（明确保留，不要在重构中破坏）

1. **组合根真的只装配**。`mainwindow.cpp` 全文没有一处 socket 操作、没有一处 QSettings 读写，扫描和存储都下沉到服务。参考工程的 `mainwindow.cpp:207`、`:446` 接收槽直接转发，本工程没有这个问题。
2. **编解码单一真源**。`MEMORY.md` 里已定为铁律，代码执行到位：`sendpanel.cpp:163` 和 `trafficpanel.cpp:118` 都走 `PayloadCodec`，占位示例甚至复用 `render()` 的输出避免字符串漂移（`sendpanel.cpp:58`）。
3. **参考分析带 SHA-256 指纹**（`docs/reference-snapshot.csv`，58 条）。这让"从哪个版本抄的"可复核，避免了参考工程那种"注释说的和代码做的对不上"。
4. **控件尺寸靠 Qt 属性而非像素**。`PortSpinBox::sizeHint()` 覆写、`fitComboToItems()` 策略设置，都是让控件跟随字体/DPI 自动定宽，而不是钉死像素值。
5. **注释解释"为什么"**。例如 `portselectorwidget.cpp:25-28` 说明为什么是 20px、`trafficpanel.cpp:117` 说明为何短路匹配四个变体、`payloadcodec.cpp:113` 说明为何 `%4==1` 无解。注释密度和参考工程（大量空实现式中文注释）不在一个层级。

## 5. 修复优先级

| 序 | 问题 | 何时 | 成本 |
| --- | --- | --- | --- |
| 1 | 🟠-7 `kind` 索引耦合 | **M5 写 SessionStore 之前，拖不得** | 约 3 行 |
| 2 | 🟠-1 拆分 `.pri` | M1 开工前 | 4 个新文件，改 include |
| 3 | 🟠-3 拆分自由函数头 | M1 开工前 | 新增 2 头文件 |
| 4 | 🟠-4 统一 include 路径 | 与 2/3 同批 | 删测试的 `INCLUDEPATH` |
| 5 | 🟠-2 `core5compat` 依赖收敛 | 随时（低成本版 4 行） | 4 行 |
| 6 | 🟠-5 清理零引用组件 | M3 之前 | 删或加注释 |
| 7 | 🟡-6 i18n 空链路 | M5 语言偏好之前 | 删或补全 |
| 8 | 🟡-9/10/11/12 健壮性与规范 | 随时 | 各 1–2 行 + 2 个配置文件 |

前 4 项建议**一次性做完并单独提交**，它们共同完成"物理目录追平设计文档"这一件事。提交后 `ARCHITECTURE.md` §11 的目录树与代码一致，M1 的模块落位就有了明确依据。

问题 2 完整版（codec 移出 `ui/`）**不进这一批**，理由见该条：M0 阶段移动会改 17 处 include 且无功能收益，等 `sending/` 落地时再做。

## 6. 修复记录（2026-10-02 同日落地）

评审完成后按第 5 节优先级实施了优化，全部通过既有 45 项 UI 测试与主工程构建验证：

| 问题 | 处置 | 结果 |
| --- | --- | --- |
| 🟠-1 `.pri` 混装 | 新建 `domain.pri` / `discovery.pri` / `persistence.pri`，`services.pri` 改为聚合入口；`serialport`+`concurrent` 只在 discovery.pri 声明 | M1 起核心测试可只链 domain |
| 🟠-3 面板横向依赖 | 6 个格式化函数迁至 `ui/common/endpointformat.{h,cpp}`，`fitComboToItems` 迁至 `ui/common/combometrics.h`；traffic/send/routing 不再 include `portselectorwidget.h` | 附带收益：暴露并补上了三个面板头缺失的 `<QWidget>`（原先靠传递包含） |
| 🟠-4 include 不统一 | 全部改项目根相对路径（`ui/...`、`domain/...`），INCLUDEPATH 由各 `.pri` 自带；`ui_smoke.pro` 删除 `INCLUDEPATH += ../..` | — |
| 🟠-7 `kind` 索引耦合 | `addItem` 显式绑定 `PortDraft::Kind` 枚举；`draft()`/`updateActionLabels`/`updateValidation` 改走 `currentData().toInt()`；`setDraft` 用 `findData` 定位，越界回落串口 | 排序/插入分组不再改变配置含义 |
| 🟠-2 codec 依赖收敛 | `QT += core5compat` 与 payloadcodec 登记收敛进 `ui/common/codec.pri`；文件位置按既往决策不动 | 迁移 `src/codec/` 时整文件搬走即可 |
| 🟠-5 零引用组件 | 删除 `elidedlabel.h` / `contentscrollarea.h` / `distributedlabel.h`（git 历史可找回） | — |
| 🟡-9 `findChild` 判空 | `mainwindow.cpp` `showRoutes()` 加空指针保护 | — |
| 🟡-11 格式配置 | 新增 `.clang-format`（4 空格、120 列、指针右对齐）与 `.editorconfig`；历史文件的 2 空格缩进留待整文件重构时统一 | — |
| 🟡-10 测试残留 include | **更正：评审误报**。`QClipboard` 实际通过 `QApplication::clipboard()` 返回值使用，include 已恢复；`QRadioButton`/`QTabWidget`/`QInputMethodEvent` 均有断言用途。测试 include 无残留 | — |

未实施（维持评审结论）：🟡-6 i18n 空链路与 🟡-12 版本脚本 `error()` 属产品决策项，留待 M5。

验证：主工程 debug 构建链接通过（post-link windeployqt 在沙箱被拦，与代码无关）；`easyDebug_ui_tests` 45 项全部通过；`--snapshot` 离屏与 windows 平台双快照目视确认界面无回归。

## 7. 评审纪律说明

- 本次未运行构建与测试，判断基于源码静态阅读 + 目录实地勘察。`tests/ui` 18 个用例的存在与命名已核对，但未执行。
- 性能结论一律未给出：`ARCHITECTURE.md` §7.1 的所有容量初值和 `TEST_PLAN.md` §4 的吞吐目标均为待 M6 校准的规划值，本评审不将其视为已验证。
- 死代码判断依据为全仓库 `grep` 零引用（含 `src/`、`tests/`、`mainwindow.cpp`）。
- 关于"是否过度设计"：当前实现无过度设计迹象——`ui/common` 只有 5 个文件、4 个面板，没有为假想需求预留抽象。问题 5 指出的 3 个零引用文件是唯一接近"预留过多"的地方，且已给出两种处置方式。

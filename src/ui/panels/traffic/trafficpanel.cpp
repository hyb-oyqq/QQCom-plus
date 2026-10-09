#include "trafficpanel.h"
#include "filterbar.h"
#include "filterflags.h"
#include "traffictextview.h"
#include "viewtoggles.h"
#include "ui/common/bytesformat.h"
#include "ui/common/combometrics.h"
#include "ui/common/endpointformat.h"
#include "ui/common/rowmetrics.h"
#include "ui/common/stayopenmenu.h"
#include "ui/common/tintedicon.h"
#include "ui/common/styledialogs.h"
#include "persistence/preferencesstore.h"

#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QShortcut>
#include <QStackedWidget>
#include <QStyle>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>

namespace {

// 顶栏与过滤条的字形尺寸，与动作按钮一致；两行并排出现，尺寸必须一样。
constexpr int BarIconSize = 14;

// 目录比较不要求文件夹已经存在。QDir 的相等判断会先做 canonical，不存在就判成不等。
bool sameDirectory(const QString &a, const QString &b)
{
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

// 没手动指定时的模板：程序目录/log/QQCom-log.txt。实际文件名另外加时间戳。
QString defaultLogTemplate()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("log/QQCom-log.txt"));
}

// 重投影的防抖窗口。连点开关、连续切换只做一次重投影；50 ms 短于人能察觉的
// "卡一下"，又足够把同一轮操作里的多次变化合成一次。
constexpr int ReprojectDebounceMs = 50;

// 超过这个条数才值得亮等待光标：更少的量重投影只要几十毫秒，
// 闪一下光标反而比等待更烦人。
constexpr int ReprojectWaitCursorRows = 8000;

// 真实消息到达的攒批窗口：一帧左右的时间足够把同一次 readyRead 的多条消息
// 合成一个插入通知；再长会让人感觉记录"迟一拍"。
constexpr int FlushIntervalMs = 16;

// 性能心跳：250 ms 一拍、8 拍一个窗口（2 秒）。这个频率下心跳自身几乎不占成本，
// 而"界面线程被长任务占住"会直接表现为心跳迟到——迟滞因此是用户感知卡顿的直接代理量。
constexpr int PerfTickMs = 250;
// 迟滞超过这个值就当"卡"了，在状态栏摘要里说出来。100 ms 与 perf 基准里的
// over100 判据同一口径：不是帧率洁癖，而是"人开始觉得顿"的那条线。
constexpr qint64 PerfJankWarnMs = 100;

} // namespace

TrafficPanel::TrafficPanel(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("Surface");
    auto *layout = new QVBoxLayout(this);
    // 内边距与行距按"紧凑布局、空间全给数据显示区"收紧。左右仍是 6，
    // 上下与行距是 4。仍保留可见留白，不与卡片边框糊在一起。
    // **左右必须是 6**：数据区外边距 6 + 这里 6 = 12，与端口条内容的缩进对齐
    //（用户反馈"边框距离内部元素没有对齐"）——改这一项要同步核对那两个数。
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(4);

    // 两行头：第一行是原顶栏（A/B、编码、过滤折叠按钮、显示设置、导出、滚动到底、清空），
    // 第二行是过滤条件条。两行同属一个头块，行距压到 4px——它们是"一次头"，
    // 不是两块独立面板。数据区高度只被这一条头带走两行控件的高度。
    auto *head = new QWidget(this);
    auto *headLayout = new QVBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(4);

    auto *top = new QHBoxLayout;
    top->setSpacing(4);

    // A/B 端点开关直接放在选项栏里：点哪个看哪个，都点看全部，无需进菜单。
    const auto portToggle = [this](const QString &letter, const QString &name) {
        auto *button = new QPushButton(letter, this);
        button->setObjectName(name);
        button->setCheckable(true);
        button->setChecked(true);
        button->setToolTip(tr("显示端点 %1 的记录").arg(letter));
        connect(button, &QPushButton::toggled, this, &TrafficPanel::updatePortVisibility);
        return button;
    };
    m_portAButton = portToggle(QStringLiteral("A"), QStringLiteral("showPortA"));
    top->addWidget(m_portAButton);
    m_portBButton = portToggle(QStringLiteral("B"), QStringLiteral("showPortB"));
    top->addWidget(m_portBButton);

    m_format = new QComboBox;
    m_format->setObjectName("trafficFormat");
    // 显示格式与发送区共用 PayloadCodec 的同一套解析/渲染规则。
    for (const auto format : PayloadCodec::formats())
        m_format->addItem(PayloadCodec::formatName(format));
    fitComboToItems(m_format);
    top->addWidget(m_format);

    // 过滤入口：顶栏的折叠按钮，位置就是原搜索框的位置——
    // 关键词输入已并入过滤条第 1 格，同一个"高频快捷位"不重复放两个输入框。
    m_filterToggle = new QPushButton(tr("过滤"), this);
    m_filterToggle->setObjectName("filterToggle");
    m_filterToggle->setCheckable(true);
    m_filterToggle->setChecked(true);
    bindToggleIcon(m_filterToggle, QStringLiteral(":/icons/filter-variant.svg"), BarIconSize);
    m_filterToggle->setToolTip(tr("展开 / 收起过滤条件条（Ctrl+Shift+F）；Ctrl+F 直接聚焦关键词"));
    connect(m_filterToggle, &QPushButton::toggled, this, &TrafficPanel::setFilterExpanded);
    top->addWidget(m_filterToggle);

    // 方向开关（RX / TX）就住在折叠按钮旁边（用户钦定）：它们**不进可折叠的第二行**，
    // 这样条件条收起来时这两个开关仍在原位。事件类型（状态/警告/错误）在显示设置
    // 菜单里——切换频次低得多，且与那批行内容开关同类，放一起更顺（用户钦定）。
    m_flags = new FilterFlags(this);
    top->addWidget(m_flags);

    // 顶栏剩下的按钮仍留在原来的视觉位置（右端）：中间的空档留给过滤条上的条件。
    top->addStretch(1);

    // 日志显示设置：行内容开关收进菜单，顶栏只留一个入口。
    // 方向与事件开关不在这里——它们管的是"哪些行进来"，不是外观：方向常驻顶栏，
    // 事件（状态/警告/错误）与上面那批"显示哪些行类型"的项一起留在这个菜单里。
    auto *settings = new QPushButton(tr("显示设置"));
    settings->setObjectName("displaySettings");
    settings->setIcon(mutedIcon(QStringLiteral(":/icons/cog.svg"), BarIconSize));
    settings->setToolTip(tr("日志显示选项：行前缀、自动换行、记录"));
    auto *displayMenu = new StayOpenMenu(settings);
    // 行内容开关：控制台语义，全部在消息到达时生效，已固化的行不重排。
    const auto rowToggle = [this, &displayMenu](const char *name, const QString &text, bool checked) {
        auto *action = displayMenu->addAction(text);
        action->setObjectName(QString::fromLatin1(name));
        action->setCheckable(true);
        action->setChecked(checked);
        connect(action, &QAction::toggled, this, &TrafficPanel::updateFilter);
        return action;
    };
    m_numberAction = rowToggle("showLineNumber", tr("显示行号"), true);
    m_timestampAction = rowToggle("showTimestamp", tr("显示时间戳"), true);
    m_directionAction = rowToggle("showDirection", tr("显示数据方向"), true);
    m_lengthAction = rowToggle("showLength", tr("显示数据长度"), false);
    // 顶栏按钮文案一律取短（用户反馈"按钮文字太长了"）：这一行要塞下 5 个开关 +
    // 一批动作按钮，每个字都是宽度。**菜单项与顶栏按钮共用 QAction 的文案**（不另抄
    // 一份短名）——两处说的必须是同一件事，完整解释放进 tooltip。
    //
    // 第三个刻意保留"自动"二字：本应用里「换行」另有含义（发送区的结尾符 CR/LF），
    // 只说「换行」会被读成"给指令加换行符"，那是另一件事。
    m_hexAction = rowToggle("showHex", tr("HEX 同显"), false);
    m_hexAction->setToolTip(tr("同时显示 HEX：数据行之后另起一行显示 HEX 对照（HEX 显示格式下自动省略）"));
    // 顶栏只放三个字。「纯数据」看不出在管什么；「是否显示前缀」又太长。
    // 点亮即去掉行号、时间、方向、长度，日志里只留数据。
    m_pureAction = rowToggle("pureData", tr("无前缀"), false);
    m_pureAction->setToolTip(tr("去掉行号、时间、方向和长度，日志里只留数据本身"));
    // 纯数据开启时，行前缀开关全部失效置灰，状态一目了然。
    connect(m_pureAction, &QAction::toggled, this, [this](bool pure) {
        for (auto *action : {m_numberAction, m_timestampAction, m_directionAction, m_lengthAction,
                             m_hexAction})
            action->setEnabled(!pure);
    });
    m_wrapAction = rowToggle("wrapTraffic", tr("自动换行"), false);
    m_wrapAction->setToolTip(tr("自动换行：按窗口宽度折行显示；关闭时长报文水平滚动"));
    connect(m_wrapAction, &QAction::toggled, this, [this](bool wrap) {
        m_text->setLineWrapping(wrap);
        savePreferences();
    });
    displayMenu->addSeparator();
    // 事件类型开关：状态迁移 / 警告 / 错误。它们与上面那批"我关心哪些行"的选项同类，
    // 只是管的是行**类型**而非行**长相**，放在同一个菜单里比在顶栏并排更顺；
    // 顶栏那个位置让给了调用频次更高的显示开关（见下）。
    m_stateAction = rowToggle("showStates", tr("状态迁移"), true);
    m_stateAction->setToolTip(tr("显示状态迁移（端口打开 / 关闭 / 重连）"));
    m_warningAction = rowToggle("showWarnings", tr("警告"), true);
    m_warningAction->setToolTip(tr("显示警告日志"));
    m_errorAction = rowToggle("showErrors", tr("错误"), true);
    m_errorAction->setToolTip(tr("显示错误日志"));

    // 显示开关（纯数据模式 / 同时显示 HEX / 自动换行）常驻顶栏，紧跟方向开关之后（用户钦定）：
    // 三者都是"随手切一下"的外观项目，频次高到不该藏在菜单里两步。顶上的是同一批
    // QAction 的门面——菜单项照旧保留，两处永远一致（见 ViewToggles）。
    // 插在 m_flags 之后而不是 addWidget：顶栏右端那批动作按钮的位置与顺序不动。
    m_viewToggles = new ViewToggles(this);
    m_viewToggles->bind(m_pureAction, m_hexAction, m_wrapAction);
    top->insertWidget(top->indexOf(m_flags) + 1, m_viewToggles);
    // 菜单项显示 tooltip：顶栏按钮文案为了省宽度已经收短，完整解释只在这里还留着，
    // 不该让用户"按了才知道是什么意思"。
    displayMenu->setToolTipsVisible(true);

    displayMenu->addSeparator();
    // 记录到文件：与"导出"互补。导出是"把当前所见存一份"，记录是"边看边记"——
    // 记录行跟随当前显示格式（HEX 模式记 HEX、文本模式记文本），所见即所记。
    m_recordAction = displayMenu->addAction(tr("记录到文件"));
    m_recordAction->setObjectName("recordTraffic");
    m_recordAction->setCheckable(true);
    connect(m_recordAction, &QAction::toggled, this, [this](bool enabled) {
        toggleLogRecording(enabled);
        savePreferences();
    });
    // 路径一旦写入偏好，再次勾选就不再询问，所以改目录必须有自己的入口。
    // 选的是文件夹：每次记录仍另存为带时间戳的新文件，基名和扩展名沿用当前模板。
    m_logLocationAction = displayMenu->addAction(tr("日志位置…"));
    m_logLocationAction->setObjectName(QStringLiteral("chooseLogDirectory"));
    connect(m_logLocationAction, &QAction::triggered, this, [this] {
        const QString dir = StyleDialogs::getExistingDirectory(this, tr("日志位置"), logDirectory());
        if (!dir.isEmpty()) setLogDirectory(dir);
    });
    m_resetLogDirectoryAction = displayMenu->addAction(tr("恢复默认位置"));
    m_resetLogDirectoryAction->setObjectName(QStringLiteral("resetLogDirectory"));
    m_resetLogDirectoryAction->setToolTip(tr("改回程序目录下的 log 文件夹"));
    connect(m_resetLogDirectoryAction, &QAction::triggered, this, &TrafficPanel::resetLogDirectory);
    refreshLogLocation();
    settings->setMenu(displayMenu);
    top->addWidget(settings);

    auto *exportButton = new QPushButton(tr("导出"));
    exportButton->setObjectName("exportTraffic");
    exportButton->setIcon(mutedIcon(QStringLiteral(":/icons/export.svg"), BarIconSize));
    top->addWidget(exportButton);

    m_follow = new QPushButton(tr("滚动到底"));
    m_follow->setObjectName("followTraffic");
    m_follow->setCheckable(true);
    m_follow->setChecked(true);
    bindToggleIcon(m_follow, QStringLiteral(":/icons/arrow-down.svg"), BarIconSize);
    m_follow->setToolTip(tr("始终滚动到最新记录；向上翻阅时自动取消"));
    top->addWidget(m_follow);

    auto *clear = new QPushButton(tr("清空"));
    clear->setObjectName("clearTraffic");
    // 垃圾桶用和按钮文字同一档红。灰图标放在红字旁边会看成没画完。
    clear->setIcon(tintedIcon(QStringLiteral(":/icons/delete-outline.svg"),
                              QColor(0xa7, 0x30, 0x1f), BarIconSize));
    top->addWidget(clear);

    // 右侧指令面板的开关，紧挨"清空"（用户钦定把面板的收起/展开放在这里）。
    // 做成勾选型开关（默认按下=面板展开）：勾选态用 QSS 的 checked 底衬。
    // 收起时按钮换琥珀色 + 右向箭头（"可展开"），展开时蓝色 + 左向箭头（"可收起"），
    // 两种状态都有明确的字形与颜色，不会让人以为按钮失效了。
    m_commandPanelToggle = new QPushButton(tr("扩展"), this);
    m_commandPanelToggle->setObjectName("commandPanelToggle");
    m_commandPanelToggle->setCheckable(true);
    m_commandPanelToggle->setChecked(true);
    m_commandPanelToggle->setToolTip(tr("展开 / 收起右侧快捷指令面板"));
    connect(m_commandPanelToggle, &QPushButton::toggled, this, [this](bool expanded) {
        emit commandPanelToggled(expanded);
    });
    // 图标随勾选态换：QPushButton 的图标状态位跟的是 pressed 不是 checked，必须手动换。
    // 展开 → 双左箭头（提示"可以往左收起"）；收起 → 双右箭头（提示"可以往右展开"）。
    const QIcon expandedIcon = tintedIcon(QStringLiteral(":/icons/chevron-double-left.svg"),
                                          QColor(0xff, 0xff, 0xff), BarIconSize);
    const QIcon collapsedIcon = tintedIcon(QStringLiteral(":/icons/chevron-double-right.svg"),
                                           QColor(0x7a, 0x3f, 0x00), BarIconSize);
    const auto refreshToggleAppearance = [this, expandedIcon, collapsedIcon](bool expanded) {
        m_commandPanelToggle->setIcon(expanded ? expandedIcon : collapsedIcon);
        // 收起态给按钮加一个属性，供 QSS 画出"值得注意"的琥珀底（用户钦定）。
        m_commandPanelToggle->setProperty("collapsed", !expanded);
        m_commandPanelToggle->style()->unpolish(m_commandPanelToggle);
        m_commandPanelToggle->style()->polish(m_commandPanelToggle);
    };
    connect(m_commandPanelToggle, &QPushButton::toggled, this, refreshToggleAppearance);
    refreshToggleAppearance(m_commandPanelToggle->isChecked());
    top->addWidget(m_commandPanelToggle);

    // 顶栏这一行"最少要多宽"由控件自己说了算：一律按 sizeHint 占位
    // （中间那个 spacer 会被跳过，它不产生宽度需求）。于是这一行的下限就是布局算出来的
    // minimumSizeHint，不需要（也不允许）外部再逐项求和——手工对账漏掉面板内边距 /
    // 边框就会差几个像素，表现为最后一个按钮探出右缘（见 rowmetrics.h）。
    pinRowToContents(top);

    headLayout->addLayout(top);

    // 过滤条：恒为一行，宽度铺满。它是"哪些行进来"的唯一出口，
    // 判定本身不在这里（TrafficFilter::Matcher 一处实现）。
    m_filterBar = new FilterBar(head);
    headLayout->addWidget(m_filterBar);
    layout->addWidget(head);

    m_model = new TrafficModel(this);
    // 攒批入模：16ms 一拍把同批消息合成一次插入通知，事件开销与消息速率解耦。
    m_flushTimer = new QTimer(this);
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(FlushIntervalMs);
    connect(m_flushTimer, &QTimer::timeout, this, &TrafficPanel::flushPending);
    // 性能心跳：始终开着（250 ms 一拍，成本可忽略）。窗口不可见时 updatePerformance
    // 会跳过采样——最小化/隐藏时 Windows 会合并定时器，照采会报出毫无意义的"迟滞"。
    m_perfTimer = new QTimer(this);
    m_perfTimer->setInterval(PerfTickMs);
    connect(m_perfTimer, &QTimer::timeout, this, &TrafficPanel::updatePerformance);
    m_perfClock.start();
    m_perfTimer->start();
    // 重投影的防抖定时器：外观变化后排队一次，连续切换只落一次重投影。
    m_reprojectTimer = new QTimer(this);
    m_reprojectTimer->setSingleShot(true);
    m_reprojectTimer->setInterval(ReprojectDebounceMs);
    // 排上队之后外观又被改回去的情况很常见（连点两下开关），落定时再确认一次，
    // 避免做一次结果与当前外观无关的白工。
    connect(m_reprojectTimer, &QTimer::timeout, this, [this] {
        // 外观路径：用**上次已应用**的条件过滤，不把未应用条件套到历史行上；但它改的是
        // "每行长什么样"，而候选含"按当前格式渲染的文本"——格式专属渲染（DEC/BASE64/转义）
        // 里命中的条件会随格式移动命中行。也不播报"已按当前条件重投影"那句（会撒谎）。
        if (appearanceChanged())
            reprojectView(/*announce=*/false, /*applyCurrentConditions=*/false);
    });
    // 文本视图是唯一显示视图：每条消息按行号、时间、方向起头，载荷按文本本来的
    // 样子展开。模型只做结构化存储（导出/记录/搜索），不再驱动任何逐帧绘制。
    m_text = new TrafficTextView;
    // 文本视图自己做的头部淘汰必须回到摘要里（它不认识状态栏）。
    // 两条淘汰路径——模型（哪条还能重投影/导出）与文本（屏幕上还有多少）——
    // 各自记账，但都要让用户看得见。
    connect(m_text, &TrafficTextView::blocksReleased, this,
        [this](int count) { m_releasedLines += static_cast<quint64>(count); });
    auto *empty = new QLabel(tr("等待接收数据\n\n在上方配置 A、B 端点，收发记录将在这里显示。"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setObjectName("EmptyState");
    empty->setWordWrap(true);
    m_empty = empty;
    m_stack = new QStackedWidget;
    m_stack->addWidget(m_text);
    m_stack->addWidget(empty);
    layout->addWidget(m_stack, 1);

    connect(m_filterBar, &FilterBar::changed, this, &TrafficPanel::updateFilter);
    connect(m_flags, &FilterFlags::changed, this, &TrafficPanel::updateFilter);
    // 重置要把常驻顶栏的方向开关与菜单里的事件开关一并复位：条件分散在两个控件里，
    // "回到不过滤"必须是整套动作，否则会留下半套条件。
    connect(m_filterBar, &FilterBar::resetRequested, this, [this] {
        m_flags->restore(DirectionFlags());
        // 事件类型开关也要一并复位：条件分散在两个控件里，"回到不过滤"必须是整套动作。
        for (auto *action : {m_stateAction, m_warningAction, m_errorAction}) {
            const QSignalBlocker blocker(action);
            action->setChecked(true);
        }
        updateFilter();
        // **必须显式重投影**：updateFilter() 末尾的 scheduleReproject() 只在"外观变了"
        // 时才排队（过滤条件的变化按设计只作用于新行），所以仅靠它，按「清除过滤」后
        // 屏幕上已经过滤掉的行**不会回来**——用户点了却看不到"全部"，等于没反应。
        // 「清除过滤」与「应用过滤」同属"用户显式要求屏幕跟着变"的动作，都走全量重放，
        // 区别只在此时条件是空的。announce 为真：状态栏会把重放了多少条说出来。
        reprojectView(/*announce=*/true, /*applyCurrentConditions=*/true);
    });
    // 按钮是"立即把条件补到已有记录上"的显式入口：直接调用，不走防抖——
    // 用户点了就要这一下改变屏幕，没道理再等 50ms。
    // 槽带默认参数，所以必须包一层 lambda：信号没有参数可传。
    // 「应用过滤」= 让视图跟上**当前**条件（行集可变）：announce 与 applyCurrentConditions 都为真。
    connect(m_filterBar, &FilterBar::rebuildRequested, this, [this] {
        // **先刷新条件快照**：正常敲字时 changed() 已经刷过，但「清除过滤」的停用期间
        // changed() 拿到的是空条件（state() 被停用短路），快照停在空上——这里不补一次，
        // 点「应用过滤」就会拿旧快照重放，表现为"点了应用过滤却不过滤"。
        updateFilter();
        reprojectView(/*announce=*/true, /*applyCurrentConditions=*/true);
    });
    connect(m_format, &QComboBox::currentIndexChanged, this, [this](int) {
        // 编码格式是"渲染规则"，和显示开关一样属于视图外观：切换会重建判定器
        // （渲染文本与 HEX 视图都按它构造），并排队一次重投影把已有内容一起换过来。
        updateFilter();
        // 记录期间换格式，要在日志里留一行标记：同一份文件里可能有两种格式的数据，
        // 没有这行，事后翻日志无从判断某一段是按什么格式记的（数据行本身不带格式）。
        if (m_logFile) {
            writeLogText(tr("# 格式切换 → %1 %2")
                             .arg(PayloadCodec::formatName(currentDisplayFormat()),
                                 QDateTime::currentDateTime().toString(
                                     QStringLiteral("yyyy-MM-dd HH:mm:ss")))
                         + QLatin1Char('\n'));
        }
    });
    // Ctrl+F 聚焦关键词（必要时先展开）；Ctrl+Shift+F 展开/收起整条。
    auto *focusFilter = new QShortcut(QKeySequence::Find, this);
    connect(focusFilter, &QShortcut::activated, this, [this] {
        setFilterExpanded(true);
        m_filterToggle->setChecked(true);
        m_filterBar->focusKeywords();
    });
    auto *toggleFilter = new QShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")), this);
    connect(toggleFilter, &QShortcut::activated, this,
        [this] { m_filterToggle->setChecked(!m_filterToggle->isChecked()); });
    connect(clear, &QPushButton::clicked, this, [this] {
        m_pending.clear();
        m_model->replaceRecords({});
        m_text->clearAll();
        m_totalAccepted = 0;
        m_filteredOut = 0;
        m_releasedRecords = 0;
        m_releasedLines = 0;
        // 缓冲清空了：视图与条件之间不再有"未追平"的关系。
        // 清空是会话边界，此刻屏幕为空——以**当前条件**作为这一会话的已应用基线不会丢任何
        // 已显示的行（本来就没有已显示的行）。由此随后的外观重投影一律走"用已应用条件重放"
        // 的同一条路径，不再有"没有基线就退回当前条件"的例外分支——那条例外会在
        // 「清空 → 追加 → 敲关键词未应用 → 改外观」这条链上把关键词无声套到历史上，
        // 破坏"外观不改变行集"的不变量。同时这也让清空后首批数据到达时不会被误判为
        // "待应用"（那批数据本就是按当前条件收的）。
        markReprojected();
        // 清空是会话边界：正在记录时换一个带时间戳的新文件（旧文件原样留在磁盘上，
        // 不覆盖、不追加）。记录开关保持勾选——用户清的是屏幕，不是记录任务。
        if (m_logFile) {
            closeLogFile();
            openNewLogFile();
        }
        updateSummary();
    });
    connect(exportButton, &QPushButton::clicked, this, &TrafficPanel::exportVisible);
    connect(m_follow, &QPushButton::toggled, this, [this](bool follow) {
        if (follow) m_text->scrollToBottom();
        savePreferences();
    });
    // 用户拖动和滚动动作取消跟随；程序追加导致的 valueChanged 不影响跟随。
    connect(m_text->verticalScrollBar(), &QScrollBar::sliderMoved, this, [this](int position) {
        if (position < m_text->verticalScrollBar()->maximum())
            m_follow->setChecked(false);
    });
    connect(m_text->verticalScrollBar(), &QScrollBar::actionTriggered, this, [this](int) {
        const auto *scroll = m_text->verticalScrollBar();
        if (scroll->sliderPosition() < scroll->maximum()) m_follow->setChecked(false);
    });
    updateSummary();
    // 文本视图按到达时的过滤条件收流，条件在这里做第一次汇聚；
    // m_preferences 此时尚未注入，updateFilter 里的 savePreferences 是空操作。
    updateFilter();
}

void TrafficPanel::setPorts(const QList<PortDraft> &ports)
{
    // 端点身份在会话内稳定；这里只刷新两个开关携带的身份并重算可见集合。
    m_portAButton->setProperty("endpoint", ports.value(0).id);
    m_portBButton->setProperty("endpoint", ports.value(1).id);
    updatePortVisibility();
}

void TrafficPanel::loadExample(const QList<PortDraft> &ports)
{
    if (ports.isEmpty()) return;
    // 构造真实形状的消息，而不是"看起来像"的占位数据：示例走的是与真实数据
    // 完全相同的筛选、格式化与导出路径，将来换了字段不会只有示例数据对不上。
    const auto start = QDateTime::currentDateTime().addSecs(-2);
    const QList<QByteArray> data{QByteArray::fromHex("010300000002c40b"), QByteArray::fromHex("010304001900286bf6"),
        QByteArray("device.ready\r\n"), QByteArray("status: OK\r\n"),
        QByteArray::fromHex("aa550601100020003f0d0a"), QByteArray("ping\r\n"), QByteArray("pong\r\n"),
        QByteArray::fromHex("01060001000119ca"), QByteArray::fromHex("01060001000119ca"),
        QStringLiteral("温度: 25.0 C\r\n").toUtf8(), QByteArray("heartbeat: 128\r\n"), QByteArray("OK\r\n")};
    QList<Traffic::Message> records;
    records.reserve(data.size());
    quint64 id = 1;
    for (int i = 0; i < data.size(); ++i) {
        const int index = (i / 3) % ports.size();
        const auto &port = ports.at(index);
        // 交替演示三种边界语义：流片段、数据报、完整消息，悬停即可看出区别。
        const auto framing = i % 3 == 0 ? Traffic::Framing::Stream
                                         : (i % 3 == 1 ? Traffic::Framing::Datagram : Traffic::Framing::Message);
        auto message = Traffic::makeSampleMessage(id++, start.addMSecs(i * 137),
            Traffic::makeSource(port, index), i % 3 == 0 ? Traffic::Direction::Transmit : Traffic::Direction::Receive,
            data.at(i), framing);
        // 串口与 TCP Client 是字节流，一条记录只是一段，标注它还有后续片段。
        if (framing == Traffic::Framing::Stream && i + 1 < data.size()) {
            message.fragmentIndex = 0;
            message.hasMoreFragments = true;
        }
        if (port.kind == PortDraft::TcpServer || port.kind == PortDraft::Udp)
            message.source.peerAddress = QStringLiteral("127.0.0.1:%1").arg(52000 + index);
        records.append(message);
    }
    m_model->replaceRecords(records);
    // 示例也走同一条投影路径。视图是模型的投影，示例不该是例外：
    // 否则"切了显示设置有一批行不跟着变"、过滤条件对示例失效，两处都会让人以为坏了。
    // 载入示例的语义 = "让视图跟上当前条件"（applyCurrentConditions 为真）；
    // 只是不播报——用户点的是"载入示例"，不需要再被告知"视图被重投影过"。
    reprojectView(/*announce=*/false, /*applyCurrentConditions=*/true);
    if (m_follow->isChecked()) m_text->scrollToBottom();
}

void TrafficPanel::appendMessages(const QList<Traffic::Message> &messages)
{
    if (messages.isEmpty()) return;
    // 逐条入模 + 滚动的事件开销与消息速率成正比；
    // 先攒进缓冲，16ms 一拍整批入模，插入通知次数与消息速率解耦。
    m_pending += messages;
    if (!m_flushTimer->isActive()) m_flushTimer->start();
}

void TrafficPanel::appendMessage(const Traffic::Message &message)
{
    m_pending.append(message);
    if (!m_flushTimer->isActive()) m_flushTimer->start();
}

void TrafficPanel::flushPending()
{
    if (m_pending.isEmpty()) return;
    auto batch = std::move(m_pending);
    m_pending.clear();
    // 这一批在界面线程上花了多久：心跳把它当"下一拍可能迟到多少"的预告量上报。
    // 只记窗口内最大值（见 updatePerformance），单批的瞬时值没有意义。
    QElapsedTimer work;
    work.start();
    // 模型从头部释放的条数要累加播报：安全阀触顶是"历史被裁了"的唯一证据，
    // 缓冲区裁剪必须提示，避免用户把显示遗漏误认为设备丢数据。
    m_releasedRecords += static_cast<quint64>(m_model->appendRecords(batch));
    writeLog(batch); // 记录与显示同流：进模型的批次才落日志
    // 日志大小在写完这批后读一次（O(1)）：摘要里的"日志 X MB"由此刷新，
    // 不记录时 m_logBytes 保持 0，摘要自然不出现这段（只报状态，不做写满盘预警）。
    if (m_logFile) m_logBytes = m_logFile->size();
    // 文本视图按当前过滤条件逐条判定（TrafficFilter::Matcher 的唯一显示出口）。
    // 这是**增量路径**：只处理这一批新行，判定器是 updateFilter 里预编译好的，
    // 每条消息都不重新解释条件、也不重新编译正则。
    // 增量路径让到达速率与文档规模完全解耦（实测每条 ~6 µs 渲染 + 一次 match）；
    // 想改条件/外观后再看过去收到的内容，走的是 reprojectView 那条全量路径。
    const auto format = currentDisplayFormat();
    QList<Traffic::Message> accepted;
    accepted.reserve(batch.size());
    for (const auto &message : batch)
        if (m_matcher.accepts(message)) accepted.append(message);
    m_totalAccepted += accepted.size();
    // 挡住多少条必须单独计数：屏幕上"什么都没有"和"被条件挡掉了"是两个完全
    // 不同的事实，只有这个数字能把它们分开。
    m_filteredOut += static_cast<quint64>(batch.size() - accepted.size());
    m_text->appendMessages(accepted, format, m_displayOptions);
    if (m_follow->isChecked()) m_text->scrollToBottom();
    m_lastBatchWorkMs = qMax(m_lastBatchWorkMs, work.elapsed());
    updateSummary();
}

// m_format 下标 → 格式枚举的唯一换算点：格式切换的过滤联动与日志落盘都从这里取，
// 换算规则（越界退回 HEX）只维护这一份。
PayloadCodec::Format TrafficPanel::currentDisplayFormat() const
{
    return PayloadCodec::formats().value(m_format->currentIndex(), PayloadCodec::Format::Hex);
}

void TrafficPanel::toggleLogRecording(bool enabled)
{
    if (!enabled) {
        closeLogFile();
        return;
    }
    // 不再弹保存对话框。没手动指定目录时写到程序目录下的 log；
    // 要换地方走显示设置里的「日志位置」。
    openNewLogFile();
}

bool TrafficPanel::openNewLogFile()
{
    // 同一时刻只会有一个打开的文件：重开即换新的，先把上一个收口（幂等）。
    closeLogFile();

    // 没手动指定时用程序目录下的 log。目录不存在就建；建不了就说清原因，让用户改到可写的位置。
    const QString templatePath = logTemplatePath();
    const QFileInfo info(templatePath);
    if (!QDir().mkpath(info.absolutePath())) {
        reportSummary(tr("日志目录无法创建：%1").arg(QDir::toNativeSeparators(info.absolutePath())));
        const QSignalBlocker blocker(m_recordAction);
        m_recordAction->setChecked(false);
        return false;
    }
    // 只取模板的"目录 + 基名 + 扩展名"，实际文件名 = 基名 + -yyyyMMdd-HHmmss + 扩展名。
    // 无扩展名的模板按约定补 .txt。
    const QString dir = info.absolutePath();
    const QString base = info.completeBaseName().isEmpty() ? QStringLiteral("QQCom-log")
                                                           : info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QStringLiteral("txt") : info.suffix();
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString path = QDir(dir).filePath(QStringLiteral("%1-%2.%3").arg(base, stamp, suffix));
    // 同一秒内重开（清空后立刻再清空）会撞名：依次 -2、-3… 直到不冲突。
    // 用 exists 预判而不是等 open 失败再退，是为了不把"撞名"误报成打开错误。
    for (int n = 2; QFile::exists(path); ++n)
        path = QDir(dir).filePath(QStringLiteral("%1-%2-%3.%4").arg(base, stamp).arg(n).arg(suffix));

    auto *file = new QFile(path, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Text)) {
        // 沿用既有语义：错误提示经摘要信号顶替状态栏文本；文件未打开则复位勾选。
        reportSummary(tr("日志文件无法打开：%1").arg(file->errorString()));
        delete file;
        const QSignalBlocker blocker(m_recordAction);
        m_recordAction->setChecked(false);
        return false;
    }
    m_logFile = file;
    m_logFilePath = path;
    // 每个新文件都先写一行元信息头：日期时间 + 当前显示格式。数据行本身不带格式，
    // 有这行，事后翻日志才知道"这是哪一次会话、按什么格式记的"。
    writeLogText(tr("# QQCom %1 格式=%2")
                      .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                          PayloadCodec::formatName(currentDisplayFormat()))
                  + QLatin1Char('\n')
                  + QStringLiteral("# Time\tPort\tPeer\tDirection\tData\n"));
    refreshRecordTooltip();
    return true;
}

void TrafficPanel::writeLogText(const QString &text)
{
    if (!m_logFile || text.isEmpty()) return;
    QTextStream out(m_logFile);
    out << text;
    out.flush();
    // 写失败不能静默：说清原因并立即停止记录（数据批与 # 元信息行同此路径）。
    if (out.status() != QTextStream::Ok) stopLogOnFailure(m_logFile->errorString());
}

void TrafficPanel::stopLogOnFailure(const QString &reason)
{
    // 沿用既有文案：先说出原因（此时文件还开着，errorString 才是这一笔的），
    // 再关闭文件、复位勾选。收口只在这里做，避免数据路径与元信息路径各写一份。
    reportSummary(tr("日志写入失败，记录已停止：%1").arg(reason));
    closeLogFile();
    const QSignalBlocker blocker(m_recordAction);
    m_recordAction->setChecked(false);
}

void TrafficPanel::refreshRecordTooltip()
{
    // 「正在写 文件名」这一句保持在 tooltip 末尾：测试用它定位当前文件。
    // 目录写在前面，记录开着或关着都能看出文件落在哪。
    if (!m_recordAction) return;
    const QString dir = QDir::toNativeSeparators(logDirectory());
    m_recordAction->setToolTip(m_logFile
            ? tr("记录到文件 · 目录 %1 · 正在写 %2")
                  .arg(dir, QFileInfo(m_logFilePath).fileName())
            : tr("把新到达的消息按当前显示格式持续写入日志文件。目录：%1。可在「日志位置」中修改")
                  .arg(dir));
}

QString TrafficPanel::logTemplatePath() const
{
    return m_logPath.isEmpty() ? defaultLogTemplate() : m_logPath;
}

QString TrafficPanel::logDirectory() const
{
    return QFileInfo(logTemplatePath()).absolutePath();
}

void TrafficPanel::refreshLogLocation()
{
    if (m_logLocationAction) {
        m_logLocationAction->setToolTip(
            tr("当前目录：%1。每次记录另存为带时间戳的新文件").arg(QDir::toNativeSeparators(logDirectory())));
    }
    if (m_resetLogDirectoryAction) {
        const bool custom = !sameDirectory(logDirectory(), QFileInfo(defaultLogTemplate()).absolutePath());
        m_resetLogDirectoryAction->setEnabled(custom);
    }
    refreshRecordTooltip();
}

void TrafficPanel::setLogDirectory(const QString &directory)
{
    const QString dir = QDir::cleanPath(directory.trimmed());
    if (dir.isEmpty()) return;
    const QFileInfo info(logTemplatePath());
    const QString base = info.completeBaseName().isEmpty() ? QStringLiteral("QQCom-log")
                                                           : info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QStringLiteral("txt") : info.suffix();
    const QString next = QDir(dir).filePath(base + QLatin1Char('.') + suffix);
    if (QDir::cleanPath(next).compare(QDir::cleanPath(logTemplatePath()), Qt::CaseInsensitive) == 0) {
        refreshLogLocation();
        return;
    }
    m_logPath = next;
    savePreferences();
    refreshLogLocation();
    if (m_recordAction && m_recordAction->isChecked() && !openNewLogFile()) return;
    reportSummary(tr("日志目录已改为 %1").arg(QDir::toNativeSeparators(logDirectory())));
}

void TrafficPanel::resetLogDirectory()
{
    if (m_logPath.isEmpty()
        && sameDirectory(logDirectory(), QFileInfo(defaultLogTemplate()).absolutePath())) {
        refreshLogLocation();
        return;
    }
    m_logPath.clear();
    // 空路径表示默认。只清内存不够：savePreferences 在路径为空时不会删掉旧键。
    if (m_preferences) m_preferences->remove(QStringLiteral("traffic/recordPath"));
    refreshLogLocation();
    if (m_recordAction && m_recordAction->isChecked() && !openNewLogFile()) return;
    reportSummary(tr("日志目录已恢复为程序目录下的 log 文件夹"));
}

void TrafficPanel::writeLog(const QList<Traffic::Message> &messages)
{
    if (!m_logFile || messages.isEmpty()) return;
    // 一条一行，与监视区同源渲染：方向是事实字段，内容由当前显示格式决定。
    // 数据行格式保持原样（制表符分隔），仅新文件与格式切换时才多出 # 元信息行。
    const auto format = currentDisplayFormat();
    QString block;
    for (const auto &r : messages) {
        // 状态/警告/错误行的正文是 statusText（"打开串口 COM1 失败：…"这类原因），
        // raw 是空的。只写 render(raw) 会让日志里出现"有时间有方向、后面空的"行——
        // 端口异常因此既没弹窗也没日志可查。只有 raw 为空时才用 statusText 顶正文：
        // 两者都在时载荷本身才是用户要看的东西。
        QString body;
        if (!r.isData() && r.raw.isEmpty() && !r.statusText.isEmpty()) {
            body = r.statusText;
            // "一条一行"是日志的硬约定：正文里的制表符/换行必须扁平化，
            // 否则断行的后半截会被下游当成新的一条记录。
            body.replace(QLatin1Char('\t'), QLatin1Char(' '));
            body.replace(QLatin1Char('\n'), QLatin1Char(' '));
            body.replace(QLatin1Char('\r'), QLatin1Char(' '));
        } else {
            body = PayloadCodec::render(r.raw, format);
        }
        block += r.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
            + QLatin1Char('\t') + endpointLabel(r.source.endpointIndex, r.source.endpointKind)
            + QLatin1Char('\t') + (r.source.hasPeer() ? r.source.peerAddress : QStringLiteral("-"))
            + QLatin1Char('\t') + Traffic::directionName(r.direction)
            + QLatin1Char('\t') + body
            + QLatin1Char('\n');
    }
    writeLogText(block);
}

void TrafficPanel::closeLogFile()
{
    if (m_logFile) {
        m_logFile->close();
        delete m_logFile;
        m_logFile = nullptr;
    }
    // 没有打开的文件，就不该再声称"正在写某个文件"；字节数归零，摘要里的"日志 X"随之消失。
    m_logBytes = 0;
    m_logFilePath.clear();
    refreshRecordTooltip();
}

void TrafficPanel::setPreferences(PreferencesStore *store)
{
    m_preferences = store;
    loadPreferences();
}

void TrafficPanel::loadPreferences()
{
    if (!m_preferences) return;
    // 恢复过程中每个 setChecked 都会联动 updateFilter→savePreferences；
    // 若不抑制，"恢复了 rx 才轮到 whole"的中间状态会把已存的值覆盖掉。
    m_loading = true;
    // 只在值能对上现有格式时恢复，配置里残留的无效文本不动当前选择。
    const auto format = m_preferences->value("traffic/format").toString();
    if (m_format->findText(format) >= 0)
        m_format->setCurrentText(format);
    // 过滤条件：filter/* 是现在的键，旧版 traffic/rx|tx|errors 作为兜底——
    // 上一版把方向开关放在显示设置里，升级后不该让用户的开关状态凭空复位。
    // 方向两项（RX/TX）回到顶栏控件；事件三项回到显示设置的 QAction——它们各住一处，
    // 但共用同一套 filter/* 键，所以"条件存在哪"与"开关画在哪"互不影响。
    DirectionFlags directions;
    directions.rx = m_preferences->value("filter/rx", m_preferences->value("traffic/rx", true)).toBool();
    directions.tx = m_preferences->value("filter/tx", m_preferences->value("traffic/tx", true)).toBool();
    m_flags->restore(directions);
    {
        const auto legacyEvents = m_preferences->value("traffic/errors", true);
        const QSignalBlocker s(m_stateAction), w(m_warningAction), e(m_errorAction);
        m_stateAction->setChecked(m_preferences->value("filter/states", true).toBool());
        m_warningAction->setChecked(m_preferences->value("filter/warnings", legacyEvents).toBool());
        m_errorAction->setChecked(m_preferences->value("filter/errors", legacyEvents).toBool());
    }
    FilterBarState filter;
    filter.caseSensitive = m_preferences->value("filter/caseSensitive", false).toBool();
    filter.keywords = m_preferences->value("filter/keywords").toStringList();
    filter.excludes = m_preferences->value("filter/excludes").toStringList();
    m_filterBar->restore(filter);

    m_numberAction->setChecked(m_preferences->value("traffic/number", true).toBool());
    m_timestampAction->setChecked(m_preferences->value("traffic/timestamp", true).toBool());
    m_directionAction->setChecked(m_preferences->value("traffic/direction", true).toBool());
    m_lengthAction->setChecked(m_preferences->value("traffic/length", false).toBool());
    m_hexAction->setChecked(m_preferences->value("traffic/hex", false).toBool());
    m_pureAction->setChecked(m_preferences->value("traffic/pure", false).toBool());
    m_wrapAction->setChecked(m_preferences->value("traffic/wrap", false).toBool());
    m_follow->setChecked(m_preferences->value("traffic/follow", true).toBool());
    m_portAButton->setChecked(m_preferences->value("traffic/portA", true).toBool());
    m_portBButton->setChecked(m_preferences->value("traffic/portB", true).toBool());
    m_logPath = m_preferences->value("traffic/recordPath").toString();
    refreshLogLocation();
    // 折叠状态先落位（此时仍在抑制保存），再统一应用一次。
    m_filterToggle->setChecked(m_preferences->value("filter/expanded", true).toBool());
    // 启动恢复"记录到文件"：用 QSignalBlocker 拦住 toggled，避免在格式还没落定前
    // 就开文件。勾选先于 m_loading 复位，好让随后的 updateFilter→savePreferences
    // 把 record=true 原样写回。
    const bool recordOn = m_preferences->value("traffic/record", false).toBool();
    if (recordOn) {
        const QSignalBlocker blocker(m_recordAction);
        m_recordAction->setChecked(true);
    }
    m_loading = false;
    setFilterExpanded(m_filterToggle->isChecked());
    updatePortVisibility();
    updateFilter(); // 恢复完成后统一刷新一次过滤
    // 恢复完成后才真正开文件：此时显示格式已从偏好落位，元信息头能记下正确格式。
    // 没保存过目录时，openNewLogFile 写到程序目录下的 log。
    if (recordOn) openNewLogFile();
}

void TrafficPanel::savePreferences()
{
    if (!m_preferences || m_loading) return;
    // 过滤条件分散在两个控件里：顶栏的常驻开关 + 条件条的内容条件。
    // 两者都只是"收集条件"，落盘时一并搬运。
    const auto flags = filterFlagsState();
    // 用 **storedState()**（输入框里的原始内容）而不是 state()：后者在「清除过滤」之后
    // 是空的（那正是停用的表现），拿它落盘会把用户敲的条件**从配置里抹掉**，
    // 下次启动就真没了（用户反馈"关键词过滤与排除是否可以记忆……又得重新输入"）。
    const auto filter = m_filterBar->storedState();
    m_preferences->setValue("filter/expanded", m_filterExpanded);
    m_preferences->setValue("filter/rx", flags.rx);
    m_preferences->setValue("filter/tx", flags.tx);
    m_preferences->setValue("filter/states", flags.states);
    m_preferences->setValue("filter/warnings", flags.warnings);
    m_preferences->setValue("filter/errors", flags.errors);
    m_preferences->setValue("filter/caseSensitive", filter.caseSensitive);
    m_preferences->setValue("filter/keywords", filter.keywords);
    m_preferences->setValue("filter/excludes", filter.excludes);
    m_preferences->setValue("traffic/format", m_format->currentText());
    m_preferences->setValue("traffic/number", m_numberAction->isChecked());
    m_preferences->setValue("traffic/timestamp", m_timestampAction->isChecked());
    m_preferences->setValue("traffic/direction", m_directionAction->isChecked());
    m_preferences->setValue("traffic/length", m_lengthAction->isChecked());
    m_preferences->setValue("traffic/hex", m_hexAction->isChecked());
    m_preferences->setValue("traffic/pure", m_pureAction->isChecked());
    m_preferences->setValue("traffic/wrap", m_wrapAction->isChecked());
    m_preferences->setValue("traffic/follow", m_follow->isChecked());
    m_preferences->setValue("traffic/portA", m_portAButton->isChecked());
    m_preferences->setValue("traffic/portB", m_portBButton->isChecked());
    m_preferences->setValue("traffic/record", m_recordAction->isChecked());
    if (!m_logPath.isEmpty()) m_preferences->setValue("traffic/recordPath", m_logPath);
}

TrafficPanel::~TrafficPanel()
{
    writeLog(m_pending); // 退出时不能丢掉尚未到 16 ms 刷新周期的最后一批。
    savePreferences();
    closeLogFile();
}

void TrafficPanel::setCommandPanelToggle(bool expanded)
{
    if (!m_commandPanelToggle) return;
    // setChecked 会 emit toggled（状态真的变了才 emit），主窗口据此执行显隐。
    m_commandPanelToggle->setChecked(expanded);
}

void TrafficPanel::updatePortVisibility()
{
    // A/B 为独立开关：勾选集合即当前显示范围，两个都勾相当于看全部。
    m_visibleEndpoints.clear();
    for (auto *button : {m_portAButton, m_portBButton})
        if (button->isChecked()) m_visibleEndpoints.insert(button->property("endpoint").toUuid());
    updateFilter();
}

FilterFlagsState TrafficPanel::filterFlagsState() const
{
    // 条件分散在两个控件里：方向（RX/TX）在顶栏的 FilterFlags，事件类型（状态/警告/错误）
    // 在显示设置菜单的 QAction。汇总只此一处，其余代码一律读它，不再各自记"哪项在哪"。
    const auto directions = m_flags->directions();
    FilterFlagsState flags;
    flags.rx = directions.rx;
    flags.tx = directions.tx;
    if (m_stateAction) flags.states = m_stateAction->isChecked();
    if (m_warningAction) flags.warnings = m_warningAction->isChecked();
    if (m_errorAction) flags.errors = m_errorAction->isChecked();
    return flags;
}

int TrafficPanel::activeFlagCount() const
{
    // 默认全开：关掉任意一项即算"在过滤"。两项来源合并计数——只数顶栏会漏掉
    // "只关掉了警告"这种最容易被忘掉的情形，而那个数字正是「过滤 · N」的唯一出口。
    const auto flags = filterFlagsState();
    return (flags.rx ? 0 : 1) + (flags.tx ? 0 : 1) + (flags.states ? 0 : 1)
        + (flags.warnings ? 0 : 1) + (flags.errors ? 0 : 1);
}

void TrafficPanel::updateFilter()
{
    // 条件只有三个来源：顶栏的端点范围、方向开关（顶栏）+ 事件开关（显示设置）、
    // 第二行的条件条。三者都只是"收集条件"，判定在 Matcher 一处。端点不重复设条件——
    // A/B 按钮就是端点维度本身（设计稿 §3.1）。
    const auto flags = filterFlagsState();
    const auto filter = m_filterBar->state();
    TrafficFilter::Options options;
    options.endpoints = m_visibleEndpoints;
    options.rx = flags.rx;
    options.tx = flags.tx;
    options.states = flags.states;
    options.warnings = flags.warnings;
    options.errors = flags.errors;
    options.keywords = filter.keywords;
    options.excludes = filter.excludes;
    options.caseSensitive = filter.caseSensitive;
    // 文本视图的到达时过滤用这一份条件的快照；写入点只有这里。
    m_filterOptions = options;
    // 条件的可执行形态在这里构造一次：正则预编译、格式快照一并定下。
    // 消息到达时只做 match/contains，不重新解释条件、也不重新编译正则。
    m_matcher = TrafficFilter::Matcher(options, currentDisplayFormat());
    // 行内容的开关组合同样在这里重建快照，flushPending 直读。
    // 它既决定新行长什么样，也是"已有内容要不要重排"的判据之一。
    m_displayOptions.number = m_numberAction->isChecked();
    m_displayOptions.timestamp = m_timestampAction->isChecked();
    m_displayOptions.direction = m_directionAction->isChecked();
    m_displayOptions.length = m_lengthAction->isChecked();
    m_displayOptions.hex = m_hexAction->isChecked();
    m_displayOptions.pure = m_pureAction->isChecked();
    // 外观（显示设置/编码格式）一变，已经收到的内容必须立刻跟着变——这是视图的
    // 语义，不是可选项。过滤条件的变化则只作用于新行，等「应用过滤」把它们补到
    // 已有记录上（用户钦定的交互），这里不做任何重放。
    scheduleReproject();
    refreshFilterToggle();
    updateSummary();
    savePreferences(); // 过滤条件的汇聚点：任一条件变化即落盘
}

void TrafficPanel::setFilterExpanded(bool expanded)
{
    m_filterExpanded = expanded;
    // 收起只隐藏第二行（内容条件）：判定器、记忆状态一律不动——
    // "折叠"是显示行为，不是"关闭过滤"；顶栏的方向开关与显示开关都不受影响。
    m_filterBar->setVisible(expanded);
    refreshFilterToggle();
    savePreferences();
}

void TrafficPanel::refreshFilterToggle()
{
    // 生效条件数是两处的合计：常驻顶栏的开关 + 条件条的内容条件。
    // 只统计条件条会漏掉"只关掉 RX"这种最常见的情形。
    const int active = activeFlagCount() + m_filterBar->activeConditionCount();
    const QString caret = m_filterExpanded ? QStringLiteral("▾") : QStringLiteral("▸");
    // 生效条件数写在按钮上：条子收起时，这个数字是"内容条件里还有几条在用"的唯一出口。
    m_filterToggle->setText(active > 0 ? tr("过滤 · %1 %2").arg(active).arg(caret)
                                      : tr("过滤 %1").arg(caret));
    // 重置必须能清掉整组条件，因此它关心的也是这个合计数。
    m_filterBar->setResetAvailable(activeFlagCount() > 0);
}

void TrafficPanel::reprojectView(bool announce, bool applyCurrentConditions)
{
    // 视图与条件取得一致的唯一入口。它同时承担两件事：
    //   ① 外观（显示设置 / 编码格式）变化 —— 改"每行长什么样"。它只用**上次已应用的条件**
    //      （m_projectedOptions），因此**不会把未应用的条件套到历史上**；但候选项里含
    //      "按当前显示格式渲染的文本"（见 TrafficFilter::Matcher），所以**只在某格式专属
    //      渲染里命中的条件**（DEC / BASE64 / 转义字节等）会随编码格式移动命中的行。这是
    //      "同一条件在新渲染下的重新求值"，不是条件被换掉——判别依据：此时
    //      m_projectedOptions == m_filterOptions、「应用过滤」提示保持干净。
    //   ② 过滤条件变化 —— 换"哪些行进来"，只有「应用过滤」/载入示例才走这条（用户钦定）
    // 数据源是模型的完整缓冲，所以它覆盖的是**全部已到达的数据**，而不是"此后的新行"。
    //
    // 投影成本实测约 6.9 µs/条（见 tests/perf/traffic_perf.cpp）：
    // 关掉那个编辑块优化会让它退回 360 µs/条，5 万条就是 18 秒——这里的每一次调用
    // 都建立在 TrafficTextView::appendMessages 那个 beginEditBlock 之上。
    const int rows = m_model->rowCount();

    // 这次用哪份条件过滤，是唯一的分歧点：
    //   · applyCurrentConditions —— 用**当前**条件（m_matcher）：行集可变，代表"让视图跟上当前条件"。
    //   · 否则（外观路径）—— 用**上次已应用**的条件（见上①：不套未应用条件；但格式专属命中的
    //     条件会随格式重新求值，行集可能移动）。
    // ⚠️ 边界：一次投影都没做过时 m_projectedOptions 还是默认构造值（endpoints 为空 →
    //    Matcher::accepts 全 false，会把屏幕清空）。此时没有"已应用条件"可用，回退用当前
    //    条件，并把它记为已应用（见下方 conditionsApplied）；这条兜底只在外观路径上可能触发。
    const bool useAppliedConditions = !applyCurrentConditions && m_everProjected;
    TrafficFilter::Matcher appliedMatcher;
    const TrafficFilter::Matcher *matcher = &m_matcher;
    if (useAppliedConditions) {
        // 已应用条件是不含 format 的值类型，按**当前**编码格式现场预编译一份判定器：
        // HEX 候选视图依赖 format，不能直接复用绑了当前条件的 m_matcher。
        appliedMatcher = TrafficFilter::Matcher(m_projectedOptions, currentDisplayFormat());
        matcher = &appliedMatcher;
    }

    // 翻阅历史时切开关不该被弹回顶部：记下滚动位置的比例，重投影后按比例放回去。
    // 跟随模式下无所谓（反正要钉到底部），所以只在没跟随时记。
    auto *scroll = m_text->verticalScrollBar();
    const bool follow = m_follow->isChecked();
    const double scrollFraction = (!follow && scroll->maximum() > 0)
        ? double(scroll->value()) / double(scroll->maximum())
        : 0.0;

    const bool showWait = rows >= ReprojectWaitCursorRows;
    if (showWait) QApplication::setOverrideCursor(Qt::WaitCursor);

    QList<Traffic::Message> accepted;
    accepted.reserve(rows);
    for (int row = 0; row < rows; ++row) {
        const auto &message = m_model->record(row);
        if (matcher->accepts(message)) accepted.append(message);
    }
    // 行号按投影结果从 1 重新排：清空与重放是同一个动作的两半，中间不能让旧行号
    // 留在文档里，否则"第 3 行"会指向两条不同的记录。
    m_text->clearAll();
    m_text->appendMessages(accepted, currentDisplayFormat(), m_displayOptions);

    if (showWait) QApplication::restoreOverrideCursor();

    // 投影之后屏幕上的条数就是摘要该说的条数：会话累计口径在这里失效了，
    // 因为更早的行已经被这一次投影丢掉（缓冲之外的历史无法追溯）。
    m_totalAccepted = static_cast<quint64>(accepted.size());
    m_filteredOut = static_cast<quint64>(rows - accepted.size());
    // conditionsApplied：这次到底有没有"让视图跟上当前条件"。外观路径走已应用条件 → false，
    // 未应用的条件必须仍被判为待应用；回退用当前条件的那次（还没投影过）算真，因为确实用上了。
    markReprojected(applyCurrentConditions || !useAppliedConditions);
    if (follow) m_text->scrollToBottom();
    else if (scrollFraction > 0.0)
        scroll->setValue(qRound(scrollFraction * scroll->maximum()));
    updateSummary();
    // 只有"跟上当前条件"的这次才播报：那句文案说的是"当前条件"，用在外观路径上会撒谎；
    // 而外观变化本来就立刻可见，不必再报。
    if (announce && applyCurrentConditions && rows > 0)
        reportSummary(tr("已按当前条件重投影：显示 %1 条（缓冲 %2 条内匹配）")
                          .arg(accepted.size())
                          .arg(rows));
}

void TrafficPanel::scheduleReproject()
{
    if (m_model->rowCount() == 0) {
        // 缓冲是空的：没有已收到的内容需要追平，当前外观就已经是"视图的样子"。
        // 顺手记下签名，免得第一行数据刚进来就做一次没有意义的空重投影。
        markReprojected();
        return;
    }
    if (!appearanceChanged()) return; // 外观没变，没什么要重排的
    m_reprojectTimer->start();
}

bool TrafficPanel::appearanceChanged() const
{
    return m_projectedFormat != currentDisplayFormat() || m_projectedDisplay != m_displayOptions;
}

void TrafficPanel::markReprojected(bool conditionsApplied)
{
    // 外观签名无论哪条路径都要更新：否则 appearanceChanged() 会一直为真，
    // 防抖定时器每拍都会再做一次同样的重投影。
    m_projectedFormat = currentDisplayFormat();
    m_projectedDisplay = m_displayOptions;
    m_everProjected = true;
    // 只有真正"让视图跟上当前条件"的这次投影才更新已应用条件（外观路径不动它）：
    // 这一条正是「应用过滤 •」待应用提示能被正确保持/清除的根据。
    if (conditionsApplied) m_projectedOptions = m_filterOptions;
    // 已知残余（所选模型的固有代价，不是 bug）：视图 = 上次投影 + 此后按**当前**条件到达的
    // 新行。若用户改了条件却没点「应用过滤」，随后到达的、只命中新条件的行会先显示出来；
    // 此时一次外观重投影（按已应用基线重放）会把它们**收回**，屏幕会变少。这不可怕——
    // 只要「应用过滤 •」在 m_projectedOptions != m_filterOptions 期间一直可见，用户就知道
    // 屏幕还没有跟上他刚改的条件、点一下即可落定。所以外观路径绝不写 m_projectedOptions。
}

void TrafficPanel::refreshApplyHint()
{
    // "条件改了但还没补到已有记录上"是一个必须显式说出来的状态：
    // 不说，用户只会看到"我改了条件，屏幕没变"，然后判断成坏了。
    const bool pending = m_model->rowCount() > 0
        && (!m_everProjected || m_projectedOptions != m_filterOptions || appearanceChanged());
    m_filterBar->setApplyPending(pending);
}

void TrafficPanel::updateSummary()
{
    // 模型是导出用的结构化存储（有上限），屏幕上的历史在文本视图里（上限高得多），
    // 摘要按"会话累计接收条数"播报，不随任何一方的淘汰缩水。
    const bool filtering = activeFlagCount() + m_filterBar->activeConditionCount() > 0;
    // 有记录才谈得上"应用过滤"：置灰而不是点了没反应。
    m_filterBar->setRebuildAvailable(m_model->rowCount() > 0);
    // 条件/外观是否已经作用到已有内容上——放在每次摘要刷新时核对，
    // 这样"改完条件屏幕没动"永远有一个明确的出口文字，不用用户去猜。
    refreshApplyHint();
    if (m_totalAccepted == 0) {
        // 空屏有两种截然不同的成因：没收到，和被条件挡住了。
        // 不说清楚，用户会以为端口没通。
        m_empty->setText(filtering && m_filteredOut > 0
                ? tr("当前过滤条件下没有匹配记录\n\n已挡住 %1 条，调整上方的过滤条件即可看到它们。")
                      .arg(m_filteredOut)
                : tr("等待接收数据\n\n在上方配置 A、B 端点，收发记录将在这里显示。"));
    }
    m_stack->setCurrentWidget(m_totalAccepted == 0 ? static_cast<QWidget *>(m_empty)
                                                   : static_cast<QWidget *>(m_text));
    reportSummary([this] {
        // 摘要 = 会话累计 + 挡住多少 + 安全阀释放了多少。三段都是事实，缺一段就会让人
        // 误判："看到的少"到底是没收到、被条件挡了，还是最早的被裁掉了。
        // 释放量只在真的发生过时出现（正常情况下永远是 0，摘要保持干净）。
        QString summary = m_filteredOut > 0
            ? tr("共 %1 条 · 过滤掉 %2 条").arg(m_totalAccepted).arg(m_filteredOut)
            : tr("共 %1 条").arg(m_totalAccepted);
        // 缓冲已满是一个事实，不是一个计数：无论触顶发生在模型侧（释放最旧记录）
        // 还是文本侧（裁掉最早的行），对用户的含义都是"最早的那段已经不在屏幕上了"。
        // 只报这一句，不再摊开两串对不上号的条数/行数——用户要的是判断，不是算术。
        if (m_releasedRecords > 0 || m_releasedLines > 0) summary += tr(" · 缓冲区已满");
        // 日志大小是"用户自己看着办"的状态信息：写满盘由用户判断，我们只报当前多大。
        if (m_logFile) summary += tr(" · 日志 %1").arg(formatBytes(m_logBytes));
        // 卡了才说：迟滞超过 100 ms（人开始觉得顿的那条线）才在状态栏出现，
        // 健康时一个字都不加；详细数值在状态栏的 tooltip 里（performanceChanged）。
        if (m_reportedLagMs >= PerfJankWarnMs)
            summary += tr(" · 界面迟滞 %1 ms").arg(m_reportedLagMs);
        return summary;
    }());
}

void TrafficPanel::updatePerformance()
{
    // 迟滞 = 这一拍实际比预期晚了多少。用 restart() 拿到距上一拍的真实间隔：
    // 界面线程被长任务占住时，连 250 ms 的心跳都会迟到，迟到量就是感知到的卡顿量。
    const qint64 delta = m_perfClock.restart();
    if (!isVisible()) {
        // 窗口不可见（最小化/隐藏）：定时器会被系统合并，采样没有意义，
        // 但要把窗口置空，免得恢复显示时把隐藏期间的值当成"刚才卡了一下"报出去。
        for (int i = 0; i < PerfWindowTicks; ++i) m_lagSamples[i] = m_workSamples[i] = 0;
        m_lastBatchWorkMs = 0;
        m_reportedLagMs = 0;
        return;
    }
    m_lagSamples[m_perfCursor] = qMax<qint64>(0, delta - PerfTickMs);
    m_workSamples[m_perfCursor] = m_lastBatchWorkMs;
    m_lastBatchWorkMs = 0;
    m_perfCursor = (m_perfCursor + 1) % PerfWindowTicks;
    // 报窗口内最大值而不是平均：用户感知的是长尾，一次 300 ms 的卡顿会被平均值抹平。
    qint64 maxLag = 0;
    qint64 maxWork = 0;
    for (int i = 0; i < PerfWindowTicks; ++i) {
        maxLag = qMax(maxLag, m_lagSamples[i]);
        maxWork = qMax(maxWork, m_workSamples[i]);
    }
    emit performanceChanged(tr("最近 %1 秒：界面迟滞 ≤ %2 ms · 单批处理 ≤ %3 ms · 缓冲 %4 · 日志 %5")
                                .arg(PerfWindowTicks * PerfTickMs / 1000)
                                .arg(maxLag)
                                .arg(maxWork)
                                .arg(formatBytes(m_model->rawBytes()),
                                    m_logFile ? formatBytes(m_logBytes) : tr("未记录")));
    // 摘要里的告警只在阈值状态翻转时重算，不做每拍无谓的字符串拼接。
    const qint64 reported = maxLag >= PerfJankWarnMs ? maxLag : 0;
    if (reported != m_reportedLagMs) {
        m_reportedLagMs = reported;
        updateSummary();
    }
}

void TrafficPanel::reportSummary(const QString &text)
{
    if (text == m_lastSummary) return;
    m_lastSummary = text;
    emit summaryChanged(text);
}

void TrafficPanel::exportVisible()
{
    if (!m_model->rowCount()) { reportSummary(tr("当前没有可导出的记录")); return; }
    const auto path = StyleDialogs::getSaveFileName(this, tr("导出记录"), "QQCom-preview.tsv", tr("制表符文件 (*.tsv)"));
    if (path.isEmpty()) return;
    // 导出列与显示列解耦：导出的是完整的结构化事实。
    // 时间用 ISO 格式带毫秒，方向/边界/来源用稳定名称，HEX 用大写无分隔。
    QByteArray output("# QQCom traffic preview\n"
                      "Time\tPort\tPeer\tDirection\tOrigin\tFraming\tState\tBytes\tTruncated\tHEX\n");
    for (int i = 0; i < m_model->rowCount(); ++i) {
        const auto &r = m_model->record(i);
        auto clean = [](QString text) {
            text.replace(QLatin1Char('\t'), QLatin1Char(' '));
            text.replace(QLatin1Char('\n'), QLatin1Char(' '));
            text.replace(QLatin1Char('\r'), QLatin1Char(' '));
            return text;
        };
        output += QString("%1\t%2\t%3\t%4\t%5\t%6\t%7\t%8\t%9\t%10\n")
                      .arg(r.timestamp.toString(Qt::ISODateWithMs),
                           clean(endpointLabel(r.source.endpointIndex, r.source.endpointKind)),
                           clean(r.source.peerAddress), Traffic::directionName(r.direction),
                           Traffic::originName(r.origin), Traffic::framingName(r.framing),
                           Traffic::stateName(r.state), QString::number(r.displayBytes()),
                           r.truncated ? QStringLiteral("yes") : QStringLiteral("no"),
                           clean(PayloadCodec::render(r.raw, PayloadCodec::Format::Hex)))
                      .toUtf8();
    }
    QSaveFile file(path);
    const bool ok = file.open(QIODevice::WriteOnly) && file.write(output) == output.size() && file.commit();
    reportSummary(ok ? tr("记录已导出") : tr("导出失败：%1").arg(file.errorString()));
}

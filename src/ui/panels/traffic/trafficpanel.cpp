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
#include "ui/common/apptheme.h"
#include "ui/common/styledialogs.h"
#include "persistence/preferencesstore.h"

#include <QApplication>
#include <QBoxLayout>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
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
#include <QUrl>
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

// 真实消息到达的攒批窗口：一帧左右的时间足够把同一次 readyRead 的多条消息
// 合成一个插入通知；再长会让人感觉记录"迟一拍"。
constexpr int FlushIntervalMs = 16;
constexpr qsizetype AppendBatchRows = 512;
constexpr qsizetype AppendBatchBytes = 64 * 1024;
constexpr int ProjectionBatchRows = 256;
constexpr int ProjectionSliceMs = 6;

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

    // 两行头：第一行是原顶栏（编码、过滤、A/B、显示、导出、滚动、清空），
    // 第二行是过滤条件条。两行同属一个头块，行距压到 4px——它们是"一次头"，
    // 不是两块独立面板。数据区高度只被这一条头带走两行控件的高度。
    auto *head = new QWidget(this);
    auto *headLayout = new QVBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(4);

    auto *top = new QHBoxLayout;
    top->setSpacing(4);

    m_format = new QComboBox;
    m_format->setObjectName("trafficFormat");
    // 显示格式与发送区共用 PayloadCodec 的同一套解析/渲染规则。
    for (const auto format : PayloadCodec::formats())
        m_format->addItem(PayloadCodec::formatName(format));
    m_format->setCurrentIndex(PayloadCodec::formats().indexOf(PayloadCodec::Format::Utf8));
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

    // A/B 端点开关紧挨过滤：点哪个看哪个，都点看全部，无需进菜单。
    // 使能（勾选）用和其他开关同一套黄，未使能保持灰。
    const auto portToggle = [this](const QString &letter, const QString &name) {
        auto *button = new QPushButton(letter, this);
        button->setObjectName(name);
        button->setCheckable(true);
        button->setChecked(true);
        const QString colorName = letter == QStringLiteral("A") ? tr("琥珀色") : tr("青色");
        button->setToolTip(tr("显示端点 %1。纯数据时，%1 的接收字节为%2。点一下就作用到已经收到的行。")
                               .arg(letter, colorName));
        connect(button, &QPushButton::toggled, this, &TrafficPanel::updatePortVisibility);
        return button;
    };
    m_portAButton = portToggle(QStringLiteral("A"), QStringLiteral("showPortA"));
    top->addWidget(m_portAButton);
    m_portBButton = portToggle(QStringLiteral("B"), QStringLiteral("showPortB"));
    top->addWidget(m_portBButton);

    // 方向开关（RX / TX）就住在折叠按钮旁边（用户钦定）：它们**不进可折叠的第二行**，
    // 这样条件条收起来时这两个开关仍在原位。事件类型（信息/警告/错误/状态）在显示设置
    // 菜单里——切换频次低得多，且与那批行内容开关同类，放一起更顺（用户钦定）。
    m_flags = new FilterFlags(this);
    top->addWidget(m_flags);

    // 顶栏剩下的按钮仍留在原来的视觉位置（右端）：中间的空档留给过滤条上的条件。
    top->addStretch(1);

    // 日志设置：行内容开关收进菜单，顶栏只留一个入口。
    // 方向与事件开关不在这里——它们管的是"哪些行进来"，不是外观：方向常驻顶栏，
    // 事件（信息/警告/错误/状态）与上面那批"显示哪些行类型"的项一起留在这个菜单里。
    auto *settings = new QPushButton(tr("设置"));
    settings->setObjectName("displaySettings");
    setMutedIcon(settings, QStringLiteral(":/icons/cog.svg"), BarIconSize);
    settings->setToolTip(tr("显示前缀、颜色、HEX、换行、信息、警告、错误、状态，以及记录到文件"));
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
    // 菜单用完整名。顶栏四个按钮仍是短名，写在 ViewToggles::bind，避免把顶栏撑开。
    m_prefixAction = rowToggle("prefix", tr("显示前缀"), true);
    m_prefixAction->setToolTip(tr(
        "勾上后每条另起一行，并显示端口、收发方向、时间和长度。"
        "端口写成 [A]/[B]，接收是 <<--，发送是 -->>，转发写成 TX+FWD。"
        "信息、警告、错误、状态写成 I、W、E、S。\n"
        "关掉后是纯数据：接收字节连续接在上一包后面，换行只来自数据自身。"
        "发送、信息、警告、错误、状态和 HEX 都不显示。打开失败会写在端口行上。\n"
        "信息、警告、错误、状态四项会取消勾选并置灰，重新打开前缀后恢复之前的选择。\n"
        "行号在左侧，浅灰色。端口用行号旁的短色标区分，A 为琥珀色，B 为青色。"
        "正文保留数据自己的颜色。"
        "再次勾上后，纯数据期间没显示的行会重新出现。"));
    m_showAnsiAction = rowToggle("showAnsi", tr("显示颜色"), true);
    m_showAnsiAction->setToolTip(tr(
        "勾上后，UTF-8、GBK、ASCII 里的颜色标识按颜色显示，标识本身不出现。"
        "例如 ESC[36;22m 后面的字按青色显示，22 只取消加粗。"
        "关掉后，颜色前后缀按原文显示，正文不再上色。"
        "HEX 对照是这条记录的原始字节，不跟着屏幕上的替换字符和续接记号变。"));
    displayMenu->addSeparator();
    m_prefixLineNumberAction = rowToggle("prefixLineNumber", tr("前缀行号"), true);
    m_prefixLineNumberAction->setVisible(false);
    m_prefixTimestampAction = rowToggle("prefixTimestamp", tr("前缀时间戳"), true);
    m_prefixTimestampAction->setToolTip(tr("每条记录显示到达时间，精确到毫秒"));
    m_prefixLengthAction = rowToggle("prefixLength", tr("前缀数据长度"), true);
    m_prefixLengthAction->setToolTip(tr("在方向后面显示本条字节数，写成 [12B]"));
    displayMenu->addSeparator();
    m_alwaysShowHexAction = rowToggle("alwaysShowHex", tr("始终显示HEX"), false);
    m_alwaysShowHexAction->setToolTip(tr(
        "在数据行下面另起一行显示 HEX 对照。显示格式已经是 HEX 时不再重复。"
        "纯数据时不可用；勾上「显示前缀」后按当前勾选恢复。"));
    // 关掉「显示前缀」时，时间和长度、HEX 对照都置灰。换行不跟前缀走。
    // 行号画在左侧槽里，不跟这个开关。
    connect(m_prefixAction, &QAction::toggled, this, [this](bool prefixes) {
        for (auto *action : {m_prefixTimestampAction, m_prefixLengthAction, m_alwaysShowHexAction})
            action->setEnabled(prefixes);
    });
    m_autoWrapAction = rowToggle("autoWrap", tr("自动换行"), true);
    m_autoWrapAction->setToolTip(tr("按窗口宽度折行显示；关闭时长报文水平滚动"));
    connect(m_autoWrapAction, &QAction::toggled, this, [this](bool wrap) {
        m_text->setLineWrapping(wrap);
        savePreferences();
    });
    displayMenu->addSeparator();
    // 事件类型开关。点了必须立刻作用到已经收到的行，不能等「应用过滤」。
    // 前缀单字母与菜单顺序一致：I 信息、W 警告、E 错误、S 状态。
    m_showInfoAction = rowToggle("showInfo", tr("显示信息"), true);
    m_showInfoAction->setToolTip(tr(
        "显示信息，前缀为 I。例如首次出现的 UDP 来源、接入的客户端，以及打开「提示日志」后的组帧说明。"
        "关闭「显示前缀」时取消勾选且不可选；打开前缀后恢复之前的选择。"));
    m_showWarningAction = rowToggle("showWarning", tr("显示警告"), true);
    m_showWarningAction->setToolTip(tr(
        "显示警告，前缀为 W。例如串口消失后等待它重新出现再自动打开。"
        "关闭「显示前缀」时取消勾选且不可选；打开前缀后恢复之前的选择。"));
    m_showErrorAction = rowToggle("showError", tr("显示错误"), true);
    m_showErrorAction->setToolTip(tr(
        "显示错误，前缀为 E。例如打开失败、设备错误、连接断开和丢弃未写出的数据。"
        "关闭「显示前缀」时取消勾选且不可选；打开前缀后恢复之前的选择。端口告警和日志文件仍保留错误原因。"));
    m_showStateAction = rowToggle("showState", tr("显示状态"), true);
    m_showStateAction->setToolTip(tr(
        "显示状态，前缀为 S。例如端口打开、关闭，以及桥接建立、停止。"
        "关闭「显示前缀」时取消勾选且不可选；打开前缀后恢复之前的选择。"));

    // 显示开关（前缀 / 颜色 / HEX / 换行）常驻顶栏，紧跟方向开关之后：
    // 都是"随手切一下"的外观项目，频次高到不该藏在菜单里两步。顶上的是同一批
    // QAction 的门面——菜单项照旧保留，两处永远一致（见 ViewToggles）。
    // 插在 m_flags 之后而不是 addWidget：顶栏右端那批动作按钮的位置与顺序不动。
    // 颜色不跟前缀走：纯数据同样要决定转义是上色还是原文显示。
    m_viewToggles = new ViewToggles(this);
    m_viewToggles->bind(m_prefixAction, m_showAnsiAction, m_alwaysShowHexAction, m_autoWrapAction);
    top->insertWidget(top->indexOf(m_flags) + 1, m_viewToggles);
    // 菜单项显示 tooltip：顶栏按钮文案为了省宽度已经收短，完整解释只在这里还留着，
    // 不该让用户"按了才知道是什么意思"。
    displayMenu->setToolTipsVisible(true);

    displayMenu->addSeparator();
    // 记录到文件与导出都保留缓冲里的全部记录，含纯数据时屏幕上不显示的发送和事件。
    // 记录行跟随当前显示格式（HEX 模式记 HEX、文本模式记文本）。
    m_recordAction = displayMenu->addAction(tr("记录到文件"));
    m_recordAction->setObjectName("recordTraffic");
    m_recordAction->setCheckable(true);
    connect(m_recordAction, &QAction::toggled, this, [this](bool enabled) {
        toggleLogRecording(enabled);
        savePreferences();
    });
    // 打开的是正在写的那一份，不是目录。没在记录时没有当前文件，这项不可点。
    m_openLogAction = displayMenu->addAction(tr("打开当前日志"));
    m_openLogAction->setObjectName(QStringLiteral("openCurrentLog"));
    m_openLogAction->setToolTip(tr("用系统默认程序打开正在写入的日志文件"));
    connect(m_openLogAction, &QAction::triggered, this, &TrafficPanel::openCurrentLog);
    refreshOpenLogAction();
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
    setMutedIcon(exportButton, QStringLiteral(":/icons/export.svg"), BarIconSize);
    exportButton->setToolTip(tr("导出当前缓冲里的全部记录，含数据区没显示的发送、信息、警告、错误和状态"));
    top->addWidget(exportButton);

    m_follow = new QPushButton(tr("滚动"));
    m_follow->setObjectName("followTraffic");
    m_follow->setCheckable(true);
    m_follow->setChecked(true);
    bindToggleIcon(m_follow, QStringLiteral(":/icons/arrow-down.svg"), BarIconSize);
    m_follow->setToolTip(tr("始终滚动到最新记录；向上翻阅时自动取消，拖回底部自动恢复"));
    top->addWidget(m_follow);

    auto *clear = new QPushButton(tr("清空"));
    clear->setObjectName("clearTraffic");
    // 垃圾桶用和按钮文字同一档红。灰图标放在红字旁边会看成没画完。
    const auto refreshClearIcon = [clear] {
        const QColor ink = AppTheme::isDark() ? QColor(0xf0, 0xb4, 0xac) : QColor(0xa7, 0x30, 0x1f);
        clear->setIcon(tintedIcon(QStringLiteral(":/icons/delete-outline.svg"), ink, BarIconSize));
    };
    refreshClearIcon();
    connect(AppTheme::instance(), &AppTheme::schemeChanged, clear, refreshClearIcon);
    top->addWidget(clear);

    // 右侧指令面板的开关，紧挨"清空"（用户钦定把面板的收起/展开放在这里）。
    // 做成勾选型开关。初次启动面板收起，按钮也不按下；主窗口随后按已保存的状态对齐。
    // 收起时按钮换琥珀色 + 右向箭头（"可展开"），展开时蓝色 + 左向箭头（"可收起"），
    // 两种状态都有明确的字形与颜色，不会让人以为按钮失效了。
    m_commandPanelToggle = new QPushButton(tr("扩展"), this);
    m_commandPanelToggle->setObjectName("commandPanelToggle");
    m_commandPanelToggle->setCheckable(true);
    m_commandPanelToggle->setChecked(false);
    m_commandPanelToggle->setToolTip(tr("展开 / 收起右侧快捷指令面板"));
    connect(m_commandPanelToggle, &QPushButton::toggled, this, [this](bool expanded) {
        emit commandPanelToggled(expanded);
    });
    // 图标随勾选态换：QPushButton 的图标状态位跟的是 pressed 不是 checked，必须手动换。
    // 展开 → 双左箭头（提示"可以往左收起"）；收起 → 双右箭头（提示"可以往右展开"）。
    const auto refreshToggleAppearance = [this](bool expanded) {
        const QColor ink = expanded ? QColor(0xff, 0xff, 0xff) : checkedInk();
        const QString path = expanded ? QStringLiteral(":/icons/chevron-double-left.svg")
                                      : QStringLiteral(":/icons/chevron-double-right.svg");
        m_commandPanelToggle->setIcon(tintedIcon(path, ink, BarIconSize));
        // 收起态给按钮加一个属性，供 QSS 画出"值得注意"的琥珀底（用户钦定）。
        m_commandPanelToggle->setProperty("collapsed", !expanded);
        m_commandPanelToggle->style()->unpolish(m_commandPanelToggle);
        m_commandPanelToggle->style()->polish(m_commandPanelToggle);
    };
    connect(m_commandPanelToggle, &QPushButton::toggled, this, refreshToggleAppearance);
    connect(AppTheme::instance(), &AppTheme::schemeChanged, this, [this, refreshToggleAppearance] {
        refreshToggleAppearance(m_commandPanelToggle->isChecked());
    });
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
    m_projectionTimer = new QTimer(this);
    m_projectionTimer->setSingleShot(true);
    connect(m_projectionTimer, &QTimer::timeout, this, &TrafficPanel::advanceProjection);
    // 排上队之后外观又被改回去的情况很常见（连点两下开关），落定时再确认一次，
    // 避免做一次结果与当前外观无关的白工。
    connect(m_reprojectTimer, &QTimer::timeout, this, [this] {
        // 外观路径：关键词仍用上次已应用的条件。事件开关和行前缀一样，落定时时用当前值。
        // 也不播报"已按当前条件重投影"那句（会撒谎）。
        if (isReprojecting() || appearanceChanged() || m_incrementalDiverged
            || m_projectedOptions.liveVisibilityDiffers(m_filterOptions))
            reprojectView(/*announce=*/false, /*applyCurrentConditions=*/false);
    });
    // 文本视图是唯一显示视图：每条消息按行号、时间、方向起头，载荷按文本本来的
    // 样子展开。模型只做结构化存储（导出/记录/搜索），不再驱动任何逐帧绘制。
    m_text = new TrafficTextView;
    // 初次启动不折行。读到已保存的「自动换行」后再对齐。
    m_text->setLineWrapping(m_autoWrapAction->isChecked());
    // 文本视图自己做的头部淘汰必须回到摘要里（它不认识状态栏）。
    // 两条淘汰路径——模型（哪条还能重投影/导出）与文本（屏幕上还有多少）——
    // 各自记账，但都要让用户看得见。
    connect(m_text, &TrafficTextView::blocksReleased, this,
        [this](int count) { m_releasedLines += static_cast<quint64>(count); });
    // 监视区正文画在文档里，换样式表不会改已经写入的字色。系统主题变了就按新颜色重画。
    connect(AppTheme::instance(), &AppTheme::schemeChanged, this, [this] {
        reprojectView(/*announce=*/false, /*applyCurrentConditions=*/false);
    });
    auto *empty = new QLabel(waitingPlaceholder());
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
        for (auto &choice : m_eventChoices) choice = true;
        for (auto *action : {m_showInfoAction, m_showWarningAction, m_showErrorAction, m_showStateAction}) {
            const QSignalBlocker blocker(action);
            action->setChecked(m_prefixAction->isChecked());
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
        cancelProjection();
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
    // 离开底部就取消跟随，拖回或滚回底部再打开。只听拖动和滚动动作：
    // 程序追加走 valueChanged，不该把跟随状态改掉。
    connect(m_text->verticalScrollBar(), &QScrollBar::sliderMoved, this, [this](int position) {
        m_follow->setChecked(position >= m_text->verticalScrollBar()->maximum());
    });
    connect(m_text->verticalScrollBar(), &QScrollBar::actionTriggered, this, [this](int) {
        const auto *scroll = m_text->verticalScrollBar();
        m_follow->setChecked(scroll->sliderPosition() >= scroll->maximum());
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
    if (m_text) {
        m_text->setPortLabels(portShortLabel(ports.value(0)), portShortLabel(ports.value(1)));
    }
    updatePortVisibility();
}

void TrafficPanel::loadExample(const QList<PortDraft> &ports)
{
    if (ports.isEmpty()) return;
    cancelProjection();
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
        if (framing == Traffic::Framing::Stream && i + 1 < data.size())
            message.hasMoreFragments = true;
        if (port.kind == PortDraft::TcpServer || port.kind == PortDraft::Udp)
            message.source.peerAddress = QStringLiteral("127.0.0.1:%1").arg(52000 + index);
        records.append(message);
    }
    m_model->replaceRecords(records);
    m_everProjected = false; // 整体替换数据后，旧视图的快照不能用于去重。
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
    if (!m_flushTimer->isActive()) m_flushTimer->start(FlushIntervalMs);
}

void TrafficPanel::appendMessage(const Traffic::Message &message)
{
    m_pending.append(message);
    if (!m_flushTimer->isActive()) m_flushTimer->start(FlushIntervalMs);
}

void TrafficPanel::flushPending()
{
    if (m_pending.isEmpty()) return;
    // 突发到达时限制单轮工作量；剩余批次交还事件循环后继续，保持原顺序且不丢消息。
    qsizetype count = 0;
    qsizetype bytes = 0;
    while (count < m_pending.size() && count < AppendBatchRows
           && (count == 0 || bytes < AppendBatchBytes))
        bytes += m_pending.at(count++).raw.size();
    auto batch = m_pending.first(count);
    m_pending.remove(0, count);
    if (!m_pending.isEmpty()) m_flushTimer->start(0);
    // 这一批在界面线程上花了多久：心跳把它当"下一拍可能迟到多少"的预告量上报。
    // 只记窗口内最大值（见 updatePerformance），单批的瞬时值没有意义。
    QElapsedTimer work;
    work.start();
    // 模型从头部释放的条数要累加播报：安全阀触顶是"历史被裁了"的唯一证据，
    // 缓冲区裁剪必须提示，避免用户把显示遗漏误认为设备丢数据。
    const int released = m_model->appendRecords(batch);
    m_releasedRecords += static_cast<quint64>(released);
    if (m_projection) m_projection->row = qMax(0, m_projection->row - released);
    writeLog(batch); // 记录与显示同流：进模型的批次才落日志
    // 日志大小在写完这批后读一次（O(1)）：摘要里的"日志 X MB"由此刷新，
    // 不记录时 m_logBytes 保持 0，摘要自然不出现这段（只报状态，不做写满盘预警）。
    if (m_logFile) m_logBytes = m_logFile->size();
    if (m_projection) {
        // 重建期间只入模、记日志；重建游标会顺序读到新增记录，避免插入到历史中间。
        m_lastBatchWorkMs = qMax(m_lastBatchWorkMs, work.elapsed());
        return;
    }
    // 文本视图按当前过滤条件逐条判定（TrafficFilter::Matcher 的唯一显示出口）。
    // 这是**增量路径**：只处理这一批新行，判定器是 updateFilter 里预编译好的，
    // 每条消息都不重新解释条件、也不重新编译正则。
    // 增量路径避免为每批消息重新遍历历史；实际布局耗时仍受文档规模影响。
    // 想改条件/外观后再看过去收到的内容，走的是 reprojectView 那条全量路径。
    const auto format = currentDisplayFormat();
    QList<Traffic::Message> accepted;
    accepted.reserve(batch.size());
    int filtered = 0;
    for (const auto &message : batch) {
        if (!m_matcher.accepts(message)) {
            ++filtered;
            continue;
        }
        // 纯数据只上屏接收字节。发送和事件仍进模型，不算被过滤条件挡住。
        if (!m_displayOptions.prefix && message.direction != Traffic::Direction::Receive)
            continue;
        accepted.append(message);
    }
    m_totalAccepted += accepted.size();
    // 挡住多少条必须单独计数：屏幕上"什么都没有"和"被条件挡掉了"是两个完全
    // 不同的事实，只有这个数字能把它们分开。
    m_filteredOut += static_cast<quint64>(filtered);
    m_text->appendMessages(accepted, format, m_displayOptions);
    if (m_follow->isChecked()) m_text->scrollToBottom();
    // 防抖还没把已有行收成当前外观时，这批已经按当前条件画上去了。
    // 若随后开关被拨回上次投影的值，scheduleReproject() 会看成"没变化"直接返回，
    // 定时器回调也一样，这批行就留在屏幕上。记下偏离，让那次排队仍然落地。
    if (m_everProjected) {
        // 全部被临时条件挡住的批次也会造成偏离，恢复原条件后必须能补回。
        if (m_projectedOptions != m_filterOptions) m_filterHistoryDiverged = true;
        if (m_projectedOptions.liveVisibilityDiffers(m_filterOptions)
            || (!accepted.isEmpty() && appearanceChanged()))
            m_incrementalDiverged = true;
    }
    m_lastBatchWorkMs = qMax(m_lastBatchWorkMs, work.elapsed());
    // 第一批数据按当前条件进了屏幕。此刻还没有「已应用」基线时，把当前条件记下来，
    // 后面改前缀、HEX、换行才不会把还没点「应用过滤」的关键词套到这些行上。
    if (!m_everProjected) markReprojected();
    updateSummary();
}

// m_format 下标 → 格式枚举的唯一换算点：格式切换的过滤联动与日志落盘都从这里取，
// 换算规则（越界退回 UTF-8）只维护这一份。
PayloadCodec::Format TrafficPanel::currentDisplayFormat() const
{
    return PayloadCodec::formats().value(m_format->currentIndex(), PayloadCodec::Format::Utf8);
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
    refreshOpenLogAction();
    return true;
}

void TrafficPanel::writeLogText(const QString &text)
{
    if (!m_logFile || text.isEmpty()) return;
    if (!m_logStream) {
        m_logStream = new QTextStream(m_logFile);
        m_logStream->setEncoding(QStringConverter::Utf8);
    }
    *m_logStream << text;
    m_logStream->flush();
    // 写失败不能静默：说清原因并立即停止记录（数据批与 # 元信息行同此路径）。
    if (m_logStream->status() != QTextStream::Ok) stopLogOnFailure(m_logFile->errorString());
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
            ? tr("写入全部到达的消息，与数据区是否显示无关。目录 %1 · 正在写 %2")
                  .arg(dir, QFileInfo(m_logFilePath).fileName())
            : tr("把新到的接收、发送、信息、警告、错误和状态按当前显示格式写入日志，与数据区是否显示无关。目录：%1。可在「日志位置」中修改")
                  .arg(dir));
}

void TrafficPanel::refreshOpenLogAction()
{
    if (!m_openLogAction) return;
    const bool writing = m_logFile && !m_logFilePath.isEmpty();
    m_openLogAction->setEnabled(writing);
    m_openLogAction->setToolTip(writing
            ? tr("用系统默认程序打开 %1").arg(QDir::toNativeSeparators(m_logFilePath))
            : tr("先勾选「记录到文件」，再打开正在写入的日志"));
}

void TrafficPanel::openCurrentLog()
{
    if (!m_logFile || m_logFilePath.isEmpty() || !QFileInfo::exists(m_logFilePath)) {
        reportSummary(tr("当前没有正在写入的日志文件"));
        refreshOpenLogAction();
        return;
    }
    m_logFile->flush();
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(m_logFilePath)))
        reportSummary(tr("无法打开日志文件：%1").arg(QDir::toNativeSeparators(m_logFilePath)));
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
    // 流在文件打开期间复用：每批新建 QTextStream 会再编一次编码器，和这一批的正文无关。
    const auto format = currentDisplayFormat();
    QString block;
    block.reserve(messages.size() * 96);
    for (const auto &r : messages) {
        // 信息/警告/错误/状态行的正文是 statusText（"打开串口 COM1 失败：…"这类原因），
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
        const QString port = r.source.endpointIndex < 0
            ? QStringLiteral("-")
            : endpointLabel(r.source.endpointIndex, r.source.endpointKind);
        block += r.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
            + QLatin1Char('\t') + port
            + QLatin1Char('\t') + (r.source.hasPeer() ? r.source.peerAddress : QStringLiteral("-"))
            + QLatin1Char('\t') + Traffic::directionName(r.direction)
            + QLatin1Char('\t') + body
            + QLatin1Char('\n');
    }
    writeLogText(block);
}

void TrafficPanel::closeLogFile()
{
    delete m_logStream;
    m_logStream = nullptr;
    if (m_logFile) {
        m_logFile->close();
        delete m_logFile;
        m_logFile = nullptr;
    }
    // 没有打开的文件，就不该再声称"正在写某个文件"；字节数归零，摘要里的"日志 X"随之消失。
    m_logBytes = 0;
    m_logFilePath.clear();
    refreshRecordTooltip();
    refreshOpenLogAction();
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
    // 方向两项（RX/TX）回到顶栏控件；事件四项回到显示设置的 QAction——它们各住一处，
    // 但共用同一套 filter/* 键，所以"条件存在哪"与"开关画在哪"互不影响。
    DirectionFlags directions;
    directions.rx = m_preferences->value("filter/rx", m_preferences->value("traffic/rx", true)).toBool();
    directions.tx = m_preferences->value("filter/tx", m_preferences->value("traffic/tx", true)).toBool();
    directions.forward = m_preferences->value("filter/forward", true).toBool();
    m_flags->restore(directions);
    {
        const auto legacyEvents = m_preferences->value("traffic/errors", true);
        const auto stateShown = m_preferences->value("filter/state",
            m_preferences->value("filter/states", true));
        const QSignalBlocker infoBlock(m_showInfoAction), s(m_showStateAction),
            w(m_showWarningAction), e(m_showErrorAction);
        // 以前信息和状态共用「显示状态」。没单独存过信息开关时，沿用那一次选择。
        m_showInfoAction->setChecked(m_preferences->value("filter/info", stateShown).toBool());
        m_showStateAction->setChecked(stateShown.toBool());
        m_showWarningAction->setChecked(m_preferences->value("filter/warning",
            m_preferences->value("filter/warnings", legacyEvents)).toBool());
        m_showErrorAction->setChecked(m_preferences->value("filter/error",
            m_preferences->value("filter/errors", legacyEvents)).toBool());
        // 即使当前已是纯数据，也先读回用户在有前缀时的选择。
        m_eventChoices[0] = m_showInfoAction->isChecked();
        m_eventChoices[1] = m_showWarningAction->isChecked();
        m_eventChoices[2] = m_showErrorAction->isChecked();
        m_eventChoices[3] = m_showStateAction->isChecked();
        m_eventOptionsActive = true;
    }
    FilterBarState filter;
    filter.caseSensitive = m_preferences->value("filter/caseSensitive", false).toBool();
    filter.keywords = m_preferences->value("filter/keywords").toStringList();
    filter.excludes = m_preferences->value("filter/excludes").toStringList();
    m_filterBar->restore(filter);

    // 旧键只在新键缺失时兜底。traffic/pure 的含义是「去掉前缀」，读进来要取反。
    // 端口方向不再单独保存：它跟着「显示前缀」。
    m_prefixLineNumberAction->setChecked(m_preferences->value("traffic/prefixLineNumber",
        m_preferences->value("traffic/number", true)).toBool());
    m_prefixTimestampAction->setChecked(m_preferences->value("traffic/prefixTimestamp",
        m_preferences->value("traffic/timestamp", true)).toBool());
    // 初次启动：前缀数据长度开，始终显示 HEX 关。键不存在时用这个缺省。
    m_prefixLengthAction->setChecked(m_preferences->value("traffic/prefixLength",
        m_preferences->value("traffic/length", true)).toBool());
    m_alwaysShowHexAction->setChecked(m_preferences->value("traffic/alwaysShowHex",
        m_preferences->value("traffic/hex", false)).toBool());
    // 初次启动按颜色显示，并藏起颜色前后缀。键不存在时用这个缺省。
    m_showAnsiAction->setChecked(m_preferences->value("traffic/showAnsi", true).toBool());
    const bool prefixOn = m_preferences->contains("traffic/prefix")
        ? m_preferences->value("traffic/prefix").toBool()
        : !m_preferences->value("traffic/pure", false).toBool();
    m_prefixAction->setChecked(prefixOn);
    syncEventActions();
    // toggled 在值没变时不发。启动时前缀已是关掉的，要在这里把时间和长度、HEX 置灰。
    for (auto *action : {m_prefixTimestampAction, m_prefixLengthAction, m_alwaysShowHexAction})
        action->setEnabled(prefixOn);
    // 初次启动自动换行打开。键不存在时用这个缺省。
    m_autoWrapAction->setChecked(m_preferences->value("traffic/autoWrap",
        m_preferences->value("traffic/wrap", true)).toBool());
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
    m_preferences->setValue("filter/forward", flags.forward);
    m_preferences->setValue("filter/info", flags.infos);
    m_preferences->setValue("filter/state", flags.states);
    m_preferences->setValue("filter/warning", flags.warnings);
    m_preferences->setValue("filter/error", flags.errors);
    m_preferences->setValue("filter/caseSensitive", filter.caseSensitive);
    m_preferences->setValue("filter/keywords", filter.keywords);
    m_preferences->setValue("filter/excludes", filter.excludes);
    m_preferences->setValue("traffic/format", m_format->currentText());
    m_preferences->setValue("traffic/prefix", m_prefixAction->isChecked());
    m_preferences->setValue("traffic/prefixLineNumber", m_prefixLineNumberAction->isChecked());
    m_preferences->setValue("traffic/prefixTimestamp", m_prefixTimestampAction->isChecked());
    m_preferences->setValue("traffic/prefixLength", m_prefixLengthAction->isChecked());
    m_preferences->setValue("traffic/alwaysShowHex", m_alwaysShowHexAction->isChecked());
    m_preferences->setValue("traffic/showAnsi", m_showAnsiAction->isChecked());
    m_preferences->setValue("traffic/autoWrap", m_autoWrapAction->isChecked());
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
    // 条件分散在两个控件里：方向（RX/TX）在顶栏的 FilterFlags，
    // 事件类型（信息/警告/错误/状态）在显示设置菜单的 QAction。
    // 汇总只此一处，其余代码一律读它，不再各自记"哪项在哪"。
    const auto directions = m_flags->directions();
    FilterFlagsState flags;
    flags.rx = directions.rx;
    flags.tx = directions.tx;
    flags.forward = directions.forward;
    if (m_showInfoAction) flags.infos = m_eventOptionsActive ? m_showInfoAction->isChecked() : m_eventChoices[0];
    if (m_showWarningAction) flags.warnings = m_eventOptionsActive ? m_showWarningAction->isChecked() : m_eventChoices[1];
    if (m_showErrorAction) flags.errors = m_eventOptionsActive ? m_showErrorAction->isChecked() : m_eventChoices[2];
    if (m_showStateAction) flags.states = m_eventOptionsActive ? m_showStateAction->isChecked() : m_eventChoices[3];
    return flags;
}

int TrafficPanel::activeFlagCount() const
{
    // 默认全开：关掉任意一项即算"在过滤"。两项来源合并计数——只数顶栏会漏掉
    // "只关掉了警告"这种最容易被忘掉的情形，而那个数字正是「过滤 · N」的唯一出口。
    const auto flags = filterFlagsState();
    return (flags.rx ? 0 : 1) + (flags.tx ? 0 : 1) + (flags.forward ? 0 : 1)
        + (flags.infos ? 0 : 1) + (flags.warnings ? 0 : 1)
        + (flags.errors ? 0 : 1) + (flags.states ? 0 : 1);
}

void TrafficPanel::updateFilter()
{
    syncEventActions();
    // 条件只有三个来源：顶栏的端点范围、方向开关（顶栏）+ 事件开关（显示设置）、
    // 第二行的条件条。三者都只是"收集条件"，判定在 Matcher 一处。端点不重复设条件——
    // A/B 按钮就是端点维度本身（设计稿 §3.1）。
    const auto flags = filterFlagsState();
    const auto filter = m_filterBar->state();
    TrafficFilter::Options options;
    options.endpoints = m_visibleEndpoints;
    options.rx = flags.rx;
    options.tx = flags.tx;
    options.forwards = flags.forward;
    options.infos = flags.infos;
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
    m_displayOptions.prefix = m_prefixAction->isChecked();
    m_displayOptions.prefixLineNumber = m_prefixLineNumberAction->isChecked();
    m_displayOptions.prefixTimestamp = m_prefixTimestampAction->isChecked();
    m_displayOptions.prefixLength = m_prefixLengthAction->isChecked();
    m_displayOptions.alwaysShowHex = m_prefixAction->isChecked() && m_alwaysShowHexAction->isChecked();
    m_displayOptions.showAnsi = m_showAnsiAction->isChecked();
    // 外观、端点、方向、转发和事件开关一点就作用到已有行。
    // 关键词和排除词只作用于新行，等「应用过滤」再补到已有记录上。
    scheduleReproject();
    refreshFilterToggle();
    updateSummary();
    savePreferences(); // 过滤条件的汇聚点：任一条件变化即落盘
}

void TrafficPanel::syncEventActions()
{
    if (!m_showStateAction) return; // 构造期菜单尚未建完。
    const bool enabled = m_prefixAction->isChecked();
    const QList<QAction *> actions{m_showInfoAction, m_showWarningAction, m_showErrorAction, m_showStateAction};
    for (int i = 0; i < actions.size(); ++i) {
        auto *action = actions.at(i);
        if (m_eventOptionsActive) m_eventChoices[i] = action->isChecked();
        const QSignalBlocker blocker(action);
        action->setEnabled(enabled);
        action->setChecked(enabled && m_eventChoices[i]);
    }
    m_eventOptionsActive = enabled;
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
    m_reprojectTimer->stop();
    Projection request;
    // 重建进行中，任何条件变化都以最新条件重启。空闲时外观变化仍沿用已应用的关键词。
    request.options = applyCurrentConditions || isReprojecting() || !m_everProjected
        ? m_filterOptions : m_projectedOptions;
    request.options.copyLiveVisibilityFrom(m_filterOptions);
    request.format = currentDisplayFormat();
    request.display = m_displayOptions;
    request.dark = AppTheme::isDark();
    if (m_projection && m_projection->options == request.options
        && m_projection->format == request.format && m_projection->display == request.display
        && m_projection->dark == request.dark) {
        m_projection->announce |= announce;
        return; // 同一条件已在处理，不重复清空、也不再排一轮。
    }
    if (!m_projection && m_everProjected && !m_incrementalDiverged && !m_filterHistoryDiverged
        && m_projectedOptions == request.options && m_projectedFormat == request.format
        && m_projectedDisplay == request.display && m_projectedDark == request.dark)
        return; // 已完成且后续增量仍与快照一致，直接复用结果，不遍历历史。
    request.matcher = TrafficFilter::Matcher(request.options, request.format);
    request.announce = announce || (m_projection && m_projection->announce);
    auto *scroll = m_text->verticalScrollBar();
    request.scrollFraction = m_projection ? m_projection->scrollFraction
        : !m_follow->isChecked() && scroll->maximum() > 0
        ? double(scroll->value()) / scroll->maximum() : 0.0;
    // 全部在 GUI 线程串行执行。停止下一次回调后替换任务，无需等待旧历史扫描结束。
    m_projectionTimer->stop();
    m_projection = std::move(request);
    m_text->clearAll(m_model->rowCount());
    updateSummary();
    // 小历史仍在本次调用中完成；大历史通过定时器让出事件循环，不调用 processEvents。
    advanceProjection();
}

void TrafficPanel::advanceProjection()
{
    if (!m_projection) return;
    auto &job = *m_projection;
    QElapsedTimer work;
    work.start();
    do {
        QList<Traffic::Message> accepted;
        const int end = qMin(job.row + ProjectionBatchRows, m_model->rowCount());
        accepted.reserve(end - job.row);
        for (; job.row < end; ++job.row) {
            const auto &message = m_model->record(job.row);
            if (!job.matcher.accepts(message)) {
                ++job.filtered;
                continue;
            }
            if (!job.display.prefix && message.direction != Traffic::Direction::Receive) continue;
            accepted.append(message);
        }
        job.accepted += accepted.size();
        m_text->appendMessages(accepted, job.format, job.display);
    } while (job.row < m_model->rowCount() && work.elapsed() < ProjectionSliceMs);
    m_lastBatchWorkMs = qMax(m_lastBatchWorkMs, work.elapsed());
    if (m_follow->isChecked()) m_text->scrollToBottom();
    if (job.row < m_model->rowCount()) {
        m_projectionTimer->start(0);
        return;
    }

    m_totalAccepted = job.accepted;
    m_filteredOut = job.filtered;
    m_projectedOptions = job.options;
    m_projectedFormat = job.format;
    m_projectedDisplay = job.display;
    m_projectedDark = job.dark;
    m_everProjected = true;
    m_incrementalDiverged = false;
    m_filterHistoryDiverged = false;
    if (!m_follow->isChecked()) {
        auto *scroll = m_text->verticalScrollBar();
        scroll->setValue(qRound(job.scrollFraction * scroll->maximum()));
    }
    const bool announce = job.announce;
    const quint64 accepted = job.accepted;
    m_projection.reset();
    updateSummary();
    if (announce)
        reportSummary(tr("历史重建完成：显示 %1 条").arg(accepted));
}

void TrafficPanel::cancelProjection()
{
    m_reprojectTimer->stop();
    m_projectionTimer->stop();
    m_projection.reset();
}

void TrafficPanel::scheduleReproject()
{
    if (isReprojecting()) {
        // 编辑关键词/排除词也立即替换正在执行的任务，不等「应用过滤」或旧任务结束。
        reprojectView(/*announce=*/false, /*applyCurrentConditions=*/true);
        return;
    }
    if (m_model->rowCount() == 0) {
        // 缓冲是空的：没有已收到的内容需要追平，当前外观就已经是"视图的样子"。
        // 只记外观签名。事件开关留在默认值上，第一批数据进来后再关掉，才能被认成变化。
        m_projectedFormat = currentDisplayFormat();
        m_projectedDisplay = m_displayOptions;
        m_projectedDark = AppTheme::isDark();
        return;
    }
    // 端点、方向、转发和事件开关决定哪些行进来，不在外观签名里，所以这里单独比。
    if (!appearanceChanged() && !m_incrementalDiverged
        && !m_projectedOptions.liveVisibilityDiffers(m_filterOptions))
        return;
    m_reprojectTimer->start();
}

bool TrafficPanel::appearanceChanged() const
{
    return m_projectedFormat != currentDisplayFormat() || m_projectedDisplay != m_displayOptions
        || m_projectedDark != AppTheme::isDark();
}

void TrafficPanel::markReprojected()
{
    // 首批增量或清空建立当前基线；分批重建完成时直接提交任务自己的快照。
    m_projectedFormat = currentDisplayFormat();
    m_projectedDisplay = m_displayOptions;
    m_projectedDark = AppTheme::isDark();
    m_projectedOptions = m_filterOptions;
    m_incrementalDiverged = false;
    m_filterHistoryDiverged = false;
    m_everProjected = true;
}

void TrafficPanel::refreshApplyHint()
{
    // "条件改了但还没补到已有记录上"是一个必须显式说出来的状态：
    // 不说，用户只会看到"我改了条件，屏幕没变"，然后判断成坏了。
    const bool pending = m_model->rowCount() > 0
        && (isReprojecting() || !m_everProjected || m_incrementalDiverged || m_filterHistoryDiverged
            || m_projectedOptions != m_filterOptions || appearanceChanged());
    m_filterBar->setApplyPending(pending, isReprojecting());
}

QString TrafficPanel::waitingPlaceholder() const
{
    if (m_prefixAction && !m_prefixAction->isChecked())
        return tr("等待接收数据\n\n只连续显示接收字节。A 为琥珀色，B 为青色。");
    return tr("等待数据\n\n在上方配置 A、B 端点，收发记录将在这里显示。");
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
                : waitingPlaceholder());
    }
    m_stack->setCurrentWidget(isReprojecting() || m_totalAccepted > 0 ? static_cast<QWidget *>(m_text)
                                                                   : static_cast<QWidget *>(m_empty));
    reportSummary([this] {
        // 摘要 = 会话累计 + 挡住多少 + 安全阀释放了多少。三段都是事实，缺一段就会让人
        // 误判："看到的少"到底是没收到、被条件挡了，还是最早的被裁掉了。
        // 释放量只在真的发生过时出现（正常情况下永远是 0，摘要保持干净）。
        const bool pureData = m_prefixAction && !m_prefixAction->isChecked();
        QString summary = m_filteredOut > 0
            ? (pureData ? tr("接收 %1 条 · 过滤掉 %2 条") : tr("共 %1 条 · 过滤掉 %2 条"))
                  .arg(m_totalAccepted).arg(m_filteredOut)
            : (pureData ? tr("接收 %1 条") : tr("共 %1 条")).arg(m_totalAccepted);
        if (isReprojecting()) summary = tr("正在重建历史…");
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
                           clean(r.source.endpointIndex < 0
                               ? QStringLiteral("-")
                               : endpointLabel(r.source.endpointIndex, r.source.endpointKind)),
                           clean(r.source.peerAddress), Traffic::directionName(r.direction),
                           Traffic::originName(r.origin), Traffic::framingName(r.framing),
                           Traffic::stateName(r.state), QString::number(r.displayBytes()),
                           r.truncated ? QStringLiteral("yes") : QStringLiteral("no"),
                           clean(PayloadCodec::render(r.raw, PayloadCodec::Format::Hex)))
                      .toUtf8();
    }
    QSaveFile file(path);
    const bool ok = file.open(QIODevice::WriteOnly) && file.write(output) == output.size() && file.commit();
    reportSummary(ok ? tr("全部记录已导出") : tr("导出失败：%1").arg(file.errorString()));
}

#include "trafficpanel.h"
#include "filterbar.h"
#include "filterflags.h"
#include "traffictextview.h"
#include "ui/common/combometrics.h"
#include "ui/common/endpointformat.h"
#include "ui/common/stayopenmenu.h"
#include "ui/common/tintedicon.h"
#include "persistence/preferencesstore.h"

#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFile>
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
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>

namespace {

// 顶栏与过滤条的字形尺寸，与动作按钮一致；两行并排出现，尺寸必须一样。
constexpr int BarIconSize = 16;

// 单条消息最多保留的预览字节数。原始长度另存于 rawLength，显示的是"本条实际有多大"，
// 而不是"我们留下了多少"——两者不等即说明发生了截断。
constexpr int PreviewLimit = 4096;

// 模型的历史预算见 TrafficModel::kByteBudget / kRowBudget——那份预算同时就是
// "屏幕上能重画的历史"预算：监视区是模型的投影，能回溯多远与一次重投影要多久
// 都由它决定。实测（tests/perf，128 B/条）投影成本约 6.9 µs/条：
//   2 万条 ≈ 140 ms，5 万条 ≈ 350 ms，12.8 万条 ≈ 880 ms。
// 取 5 万条 / 16 MiB：重投影仍在"点一下就有"的量级内，同时是原先
// 2 万条 / 4 MiB 的四倍纵深。要更远必须先降 traffic_perf 里的投影成本。

// 重投影的防抖窗口。连点开关、连续切换只做一次重投影；50 ms 短于人能察觉的
// "卡一下"，又足够把同一轮操作里的多次变化合成一次。
constexpr int ReprojectDebounceMs = 50;

// 超过这个条数才值得亮等待光标：更少的量重投影只要几十毫秒，
// 闪一下光标反而比等待更烦人。
constexpr int ReprojectWaitCursorRows = 8000;

// 真实消息到达的攒批窗口：一帧左右的时间足够把同一次 readyRead 的多条消息
// 合成一个插入通知；再长会让人感觉记录"迟一拍"。
constexpr int FlushIntervalMs = 16;

// 丢弃最旧记录直到回到预算内。逐条移除而不是整体截断，保留最新的连续上下文。
// 字节与条数两个上限先到者为准；返回丢弃的条数，由调用方决定用哪种模型通知。
int trimToBudget(QList<Traffic::Message> &records, qint64 &bytes)
{
    int dropped = 0;
    while (!records.isEmpty()
           && (bytes > TrafficModel::kByteBudget || records.size() > TrafficModel::kRowBudget)) {
        bytes -= records.first().raw.size();
        records.removeFirst();
        ++dropped;
    }
    return dropped;
}

void enforceBudget(QList<Traffic::Message> &records)
{
    qint64 bytes = 0;
    for (const auto &record : records)
        bytes += record.raw.size();
    trimToBudget(records, bytes);
}

// 保留预览长度，并把 rawLength 修正为真实长度。
void applyPreviewLimit(Traffic::Message &record)
{
    record.rawLength = record.rawLength > 0 ? record.rawLength : record.raw.size();
    if (record.raw.size() > PreviewLimit) {
        record.raw.truncate(PreviewLimit);
        record.truncated = true;
    } else if (record.rawLength > record.raw.size()) {
        record.truncated = true;
    }
}

} // namespace

TrafficModel::TrafficModel(QObject *parent) : QObject(parent) {}
int TrafficModel::rowCount() const { return m_records.size(); }
const Traffic::Message &TrafficModel::record(int row) const { return m_records.at(row); }

void TrafficModel::replaceRecords(QList<Traffic::Message> records)
{
    // This preview model has a hard cap; live batching belongs to the I/O milestone.
    if (records.size() > 2000) records = records.last(2000);
    for (auto &r : records)
        applyPreviewLimit(r);
    enforceBudget(records);
    m_records = std::move(records);
    m_rawBytes = 0;
    for (const auto &r : m_records)
        m_rawBytes += r.raw.size();
}

void TrafficModel::appendRecords(QList<Traffic::Message> records)
{
    if (records.isEmpty()) return;
    for (auto &r : records)
        applyPreviewLimit(r);
    const int added = records.size();

    qint64 incomingBytes = 0;
    for (const auto &r : records)
        incomingBytes += r.raw.size();

    // 先把本批计入总量，再算出"包含本批在内"要淘汰多少条。反过来（先裁后插）
    // 会让上限超出整整一批，把硬约束变成软约束。
    int drop = 0;
    qint64 keptBytes = m_rawBytes;
    while (drop < m_records.size()
           && (keptBytes + incomingBytes > TrafficModel::kByteBudget
               || (m_records.size() - drop) + added > TrafficModel::kRowBudget)) {
        keptBytes -= m_records.at(drop).raw.size();
        ++drop;
    }

    if (drop > 0) m_records.remove(0, drop);
    m_rawBytes = keptBytes + incomingBytes;
    m_records += records;
}

TrafficFilter::TextCondition::TextCondition(const QString &expression, bool regex, Qt::CaseSensitivity sensitivity)
    : m_sensitivity(sensitivity)
{
    const auto trimmed = expression.trimmed();
    if (trimmed.isEmpty()) return;
    if (!regex) {
        m_literal = trimmed;
        return;
    }
    // 预编译一次，热路径只 match()。表达式无效则该条件不参与判定（isEmpty 为真，
    // Matcher 会把它丢掉），由条件条标红提示——退化的方向必须是"不过滤"：
    // 宁可多显示，也绝不因为写错一个括号把整屏数据静默吞掉。
    QRegularExpression pattern(trimmed);
    pattern.setPatternOptions(sensitivity == Qt::CaseSensitive ? QRegularExpression::NoPatternOption
                                                              : QRegularExpression::CaseInsensitiveOption);
    if (!pattern.isValid()) return;
    m_pattern = pattern;
    m_regex = true;
}

bool TrafficFilter::TextCondition::isEmpty() const
{
    return !m_regex && m_literal.isEmpty();
}

bool TrafficFilter::TextCondition::matches(const QString &text) const
{
    if (m_regex) return m_pattern.match(text).hasMatch();
    return !m_literal.isEmpty() && text.contains(m_literal, m_sensitivity);
}

TrafficFilter::Matcher::Matcher(const Options &options, PayloadCodec::Format format)
    : m_options(options), m_format(format)
{
    // 条件的解释只在这里发生一次：词表 → 预编译条件，热路径不再碰字符串表。
    const auto sensitivity = options.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    for (const auto &keyword : options.keywords) {
        TextCondition condition(keyword, options.regex, sensitivity);
        if (!condition.isEmpty()) m_keywords.append(condition);
    }
    for (const auto &exclude : options.excludes) {
        TextCondition condition(exclude, options.regex, sensitivity);
        if (!condition.isEmpty()) m_excludes.append(condition);
    }
}

bool TrafficFilter::Matcher::accepts(const Traffic::Message &r) const
{
    // 端点与方向是消息自带的事实字段，直接按结构判等，不解析显示文本。
    if (m_options.endpoints.isEmpty() || !m_options.endpoints.contains(r.source.endpointId)) return false;
    switch (r.direction) {
    case Traffic::Direction::Receive:
        if (!m_options.rx) return false;
        break;
    case Traffic::Direction::Transmit:
        if (!m_options.tx) return false;
        break;
    case Traffic::Direction::State:
        if (!m_options.states) return false;
        break;
    case Traffic::Direction::Warning:
        if (!m_options.warnings) return false;
        break;
    case Traffic::Direction::Error:
        if (!m_options.errors) return false;
        break;
    }
    if (m_keywords.isEmpty() && m_excludes.isEmpty()) return true;

    enum Verdict { KeepLooking, Decided, Rejected };
    bool keywordHit = m_keywords.isEmpty(); // 没写关键词 = 这一维不限
    bool rejected = false;
    const auto scan = [this, &keywordHit](const QString &text) {
        // 排除词优先于关键词：命中任一即否决，不再往下找。
        for (const auto &exclude : m_excludes)
            if (exclude.matches(text)) return Rejected;
        if (!keywordHit) {
            for (const auto &keyword : m_keywords) {
                if (keyword.matches(text)) {
                    keywordHit = true;
                    break;
                }
            }
        }
        // 关键词已满足、且没有排除词要继续查：后面更贵的视图（含两条 HEX 串）不必再构造。
        return keywordHit && m_excludes.isEmpty() ? Decided : KeepLooking;
    };
    // 返回 true 表示"已有结论"，调用方立即返回；scan 内部已经短路，
    // 这里不重算任何一遍匹配。
    const auto settled = [&scan, &rejected](const QString &text) {
        switch (scan(text)) {
        case KeepLooking:
            return false;
        case Decided:
            return true;
        default:
            rejected = true;
            return true;
        }
    };

    // 候选文本按"便宜 → 昂贵"排序，逐项短路。结构化字段与渲染文本先上，
    // 只有它们全都没给结论时才构造 HEX 串——常见关键词（"心跳"、对端地址）
    // 在到达时就命中，最贵的两条 HEX 根本不构造。
    if (settled(r.source.peerAddress)) return !rejected;
    if (settled(r.source.endpointName)) return !rejected;
    if (settled(r.statusText)) return !rejected;
    if (settled(r.detail)) return !rejected;
    const QString rendered = PayloadCodec::render(r.raw, m_format);
    if (settled(rendered)) return !rejected;
    if (m_format != PayloadCodec::Format::Utf8
        && settled(PayloadCodec::render(r.raw, PayloadCodec::Format::Utf8)))
        return !rejected;
    // 两种 HEX 写法都算所见即可搜：紧凑写法与带空格写法。
    if (settled(QString::fromLatin1(r.raw.toHex()))) return !rejected;
    if (settled(QString::fromLatin1(r.raw.toHex(' ')))) return !rejected;
    // 全部视图扫完仍没命中关键词：淘汰。
    return !rejected && keywordHit;
}

bool TrafficFilter::accepts(const Traffic::Message &r, const Options &options, PayloadCodec::Format format)
{
    // 一次性判定：等价于构造一次 Matcher 再判一条。热路径（消息到达）必须自己
    // 持有 Matcher，否则每条消息都要重新预编译一遍正则。
    return Matcher(options, format).accepts(r);
}

TrafficPanel::TrafficPanel(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("Surface");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(12);

    // 两行头：第一行是原顶栏（A/B、编码、过滤折叠按钮、显示设置、导出、滚动到底、清空），
    // 第二行是过滤条件条。两行同属一个头块，行距压到 6px——它们是"一次头"，
    // 不是两块独立面板。数据区高度只被这一条头带走两行控件的高度。
    auto *head = new QWidget(this);
    auto *headLayout = new QVBoxLayout(head);
    headLayout->setContentsMargins(0, 0, 0, 0);
    headLayout->setSpacing(6);

    auto *top = new QHBoxLayout;
    top->setSpacing(6);

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
    m_filterToggle->setIcon(mutedIcon(QStringLiteral(":/icons/filter-variant.svg"), BarIconSize));
    m_filterToggle->setToolTip(tr("展开 / 收起过滤条件条（Ctrl+Shift+F）；Ctrl+F 直接聚焦关键词"));
    connect(m_filterToggle, &QPushButton::toggled, this, &TrafficPanel::setFilterExpanded);
    top->addWidget(m_filterToggle);

    // 方向与事件开关就住在折叠按钮旁边（用户钦定）：它们**不进可折叠的第二行**，
    // 这样条件条收起来时这排开关仍在原位。图标 + 两字文案，不需要悬停就能读懂。
    m_flags = new FilterFlags(this);
    top->addWidget(m_flags);

    // 顶栏剩下的按钮仍留在原来的视觉位置（右端）：中间的空档留给过滤条上的条件。
    top->addStretch(1);

    // 日志显示设置：行内容开关收进菜单，顶栏只留一个入口。
    // 方向与事件开关不在这里——它们管的是"哪些行进来"，已迁到过滤条（设计稿 §7.4）。
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
    m_hexAction = rowToggle("showHex", tr("同时显示 HEX"), false);
    m_hexAction->setToolTip(tr("数据行之后另起一行显示 HEX 对照（HEX 显示格式下自动省略）"));
    m_pureAction = rowToggle("pureData", tr("纯数据模式"), false);
    m_pureAction->setToolTip(tr("移除行号、时间、方向等一切前缀，只显示数据本身"));
    // 纯数据开启时，行前缀开关全部失效置灰，状态一目了然。
    connect(m_pureAction, &QAction::toggled, this, [this](bool pure) {
        for (auto *action : {m_numberAction, m_timestampAction, m_directionAction, m_lengthAction,
                             m_hexAction})
            action->setEnabled(!pure);
    });
    m_wrapAction = rowToggle("wrapTraffic", tr("自动换行"), false);
    m_wrapAction->setToolTip(tr("按窗口宽度折行显示；关闭时长报文水平滚动"));
    connect(m_wrapAction, &QAction::toggled, this, [this](bool wrap) {
        m_text->setLineWrapping(wrap);
        savePreferences();
    });
    displayMenu->addSeparator();
    // 记录到文件：与"导出"互补。导出是"把当前所见存一份"，记录是"边看边记"——
    // 记录行跟随当前显示格式（HEX 模式记 HEX、文本模式记文本），所见即所记。
    m_recordAction = displayMenu->addAction(tr("记录到文件"));
    m_recordAction->setObjectName("recordTraffic");
    m_recordAction->setCheckable(true);
    m_recordAction->setToolTip(tr("把新到达的消息按当前显示格式持续写入日志文件"));
    connect(m_recordAction, &QAction::toggled, this, [this](bool enabled) {
        toggleLogRecording(enabled);
        savePreferences();
    });
    settings->setMenu(displayMenu);
    top->addWidget(settings);

    auto *exportButton = new QPushButton(tr("导出"));
    exportButton->setObjectName("exportTraffic");
    top->addWidget(exportButton);

    m_follow = new QPushButton(tr("滚动到底"));
    m_follow->setObjectName("followTraffic");
    m_follow->setCheckable(true);
    m_follow->setChecked(true);
    m_follow->setIcon(mutedIcon(QStringLiteral(":/icons/arrow-down.svg"), BarIconSize));
    m_follow->setToolTip(tr("始终滚动到最新记录；向上翻阅时自动取消"));
    top->addWidget(m_follow);

    auto *clear = new QPushButton(tr("清空"));
    clear->setObjectName("clearTraffic");
    top->addWidget(clear);

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
    // 重投影的防抖定时器：外观变化后排队一次，连续切换只落一次重投影。
    m_reprojectTimer = new QTimer(this);
    m_reprojectTimer->setSingleShot(true);
    m_reprojectTimer->setInterval(ReprojectDebounceMs);
    // 排上队之后外观又被改回去的情况很常见（连点两下开关），落定时再确认一次，
    // 避免做一次结果与当前外观无关的白工。
    connect(m_reprojectTimer, &QTimer::timeout, this, [this] {
        if (appearanceChanged()) reprojectView();
    });
    // 文本视图是唯一显示视图：每条消息按行号、时间、方向起头，载荷按文本本来的
    // 样子展开。模型只做结构化存储（导出/记录/搜索），不再驱动任何逐帧绘制。
    m_text = new TrafficTextView;
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
    // 重置要把常驻顶栏的方向/事件开关一并复位：条件分散在两个控件里，
    // "回到不过滤"必须是整套动作，否则会留下半套条件。
    connect(m_filterBar, &FilterBar::resetRequested, this, [this] {
        m_flags->restore(FilterFlagsState());
        updateFilter();
    });
    // 按钮是"立即把条件补到已有记录上"的显式入口：直接调用，不走防抖——
    // 用户点了就要这一下改变屏幕，没道理再等 50ms。
    // 槽带默认参数（announce），所以必须包一层 lambda：信号没有参数可传。
    connect(m_filterBar, &FilterBar::rebuildRequested, this, [this] { reprojectView(); });
    connect(m_format, &QComboBox::currentIndexChanged, this, [this](int) {
        // 编码格式是"渲染规则"，和显示开关一样属于视图外观：切换会重建判定器
        // （渲染文本与 HEX 视图都按它构造），并排队一次重投影把已有内容一起换过来。
        updateFilter();
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
        // 缓冲清空了：视图与条件之间不再有"未追平"的关系，签名也一起复位。
        m_everProjected = false;
        updateSummary();
    });
    connect(exportButton, &QPushButton::clicked, this, &TrafficPanel::exportVisible);
    connect(m_follow, &QPushButton::toggled, this, [this](bool follow) {
        if (follow) m_text->scrollToBottom();
        savePreferences();
    });
    // 任何形式的向上翻阅（拖动、滚轮、翻页）都解除“滚动到底”。
    // 视图自己改滚动范围时不算翻阅，否则每次追加都会把跟随关掉；
    // 追加后立刻钉回底部，不会误触发。
    connect(m_text->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int position) {
        if (position < m_text->verticalScrollBar()->maximum()) m_follow->setChecked(false);
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
    // 不播报摘要——用户点的是"载入示例"，不需要再被告知"视图被重投影过"。
    reprojectView(false);
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
    m_model->appendRecords(batch);
    writeLog(batch); // 记录与显示同流：进模型的批次才落日志
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
    if (m_logPath.isEmpty()) {
        // 首次开启才选择文件；之后重新勾选沿用上次路径，继续追加而不是另起文件。
        m_logPath = QFileDialog::getSaveFileName(this, tr("选择日志文件"),
            QStringLiteral("easyDebug-log.txt"), tr("文本文件 (*.txt *.log *.tsv)"));
        if (m_logPath.isEmpty()) {
            // 用户取消 = 不开启；阻断信号复位勾选，避免显示与实际状态不一致。
            const QSignalBlocker blocker(m_recordAction);
            m_recordAction->setChecked(false);
            return;
        }
    }
    m_logFile = new QFile(m_logPath, this);
    if (!m_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        // 错误提示经摘要信号顶替状态栏文本；界面状态没有其他变化。
        reportSummary(tr("日志文件无法打开：%1").arg(m_logFile->errorString()));
        delete m_logFile;
        m_logFile = nullptr;
        const QSignalBlocker blocker(m_recordAction);
        m_recordAction->setChecked(false);
        return;
    }
}

void TrafficPanel::writeLog(const QList<Traffic::Message> &messages)
{
    if (!m_logFile || messages.isEmpty()) return;
    // 一条一行，与监视区同源渲染：方向是事实字段，内容由当前显示格式决定。
    const auto format = currentDisplayFormat();
    QTextStream out(m_logFile);
    for (const auto &r : messages) {
        out << r.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))
            << '\t' << endpointLabel(r.source.endpointIndex, r.source.endpointKind)
            << '\t' << (r.source.hasPeer() ? r.source.peerAddress : QStringLiteral("-"))
            << '\t' << Traffic::directionName(r.direction)
            << '\t' << PayloadCodec::render(r.raw, format)
            << '\n';
    }
    out.flush();
    if (out.status() != QTextStream::Ok) {
        reportSummary(tr("日志写入失败，记录已停止：%1").arg(m_logFile->errorString()));
        m_logFile->close();
        delete m_logFile;
        m_logFile = nullptr;
        const QSignalBlocker blocker(m_recordAction);
        m_recordAction->setChecked(false);
    }
}

void TrafficPanel::closeLogFile()
{
    if (m_logFile) {
        m_logFile->close();
        delete m_logFile;
        m_logFile = nullptr;
    }
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
    FilterFlagsState flags;
    flags.rx = m_preferences->value("filter/rx", m_preferences->value("traffic/rx", true)).toBool();
    flags.tx = m_preferences->value("filter/tx", m_preferences->value("traffic/tx", true)).toBool();
    flags.states = m_preferences->value("filter/states", true).toBool();
    const auto legacyEvents = m_preferences->value("traffic/errors", true);
    flags.warnings = m_preferences->value("filter/warnings", legacyEvents).toBool();
    flags.errors = m_preferences->value("filter/errors", legacyEvents).toBool();
    m_flags->restore(flags);
    FilterBarState filter;
    filter.caseSensitive = m_preferences->value("filter/caseSensitive", false).toBool();
    filter.regex = m_preferences->value("filter/regex", false).toBool();
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
    // 折叠状态先落位（此时仍在抑制保存），再统一应用一次。
    m_filterToggle->setChecked(m_preferences->value("filter/expanded", true).toBool());
    const bool recordOn = m_preferences->value("traffic/record", false).toBool();
    if (recordOn) {
        // 上次开着记录：沿用原文件继续追加；文件不可用则复位勾选（含提示）。
        m_recordAction->setChecked(true);
    }
    m_loading = false;
    setFilterExpanded(m_filterToggle->isChecked());
    updatePortVisibility();
    updateFilter(); // 恢复完成后统一刷新一次过滤
}

void TrafficPanel::savePreferences()
{
    if (!m_preferences || m_loading) return;
    // 过滤条件分散在两个控件里：顶栏的常驻开关 + 条件条的内容条件。
    // 两者都只是"收集条件"，落盘时一并搬运。
    const auto flags = m_flags->state();
    const auto filter = m_filterBar->state();
    m_preferences->setValue("filter/expanded", m_filterExpanded);
    m_preferences->setValue("filter/rx", flags.rx);
    m_preferences->setValue("filter/tx", flags.tx);
    m_preferences->setValue("filter/states", flags.states);
    m_preferences->setValue("filter/warnings", flags.warnings);
    m_preferences->setValue("filter/errors", flags.errors);
    m_preferences->setValue("filter/caseSensitive", filter.caseSensitive);
    m_preferences->setValue("filter/regex", filter.regex);
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
    savePreferences();
    closeLogFile();
}

void TrafficPanel::updatePortVisibility()
{
    // A/B 为独立开关：勾选集合即当前显示范围，两个都勾相当于看全部。
    m_visibleEndpoints.clear();
    for (auto *button : {m_portAButton, m_portBButton})
        if (button->isChecked()) m_visibleEndpoints.insert(button->property("endpoint").toUuid());
    updateFilter();
}

void TrafficPanel::updateFilter()
{
    // 条件只有三个来源：顶栏的端点范围、顶栏的方向/事件开关、第二行的条件条。
    // 三者都只是"收集条件"，判定在 Matcher 一处。端点不重复设条件——
    // A/B 按钮就是端点维度本身（设计稿 §3.1）。
    const auto flags = m_flags->state();
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
    options.regex = filter.regex;
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
    // "折叠"是显示行为，不是"关闭过滤"；顶栏的方向/事件开关也不受影响。
    m_filterBar->setVisible(expanded);
    refreshFilterToggle();
    savePreferences();
}

void TrafficPanel::refreshFilterToggle()
{
    // 生效条件数是两处的合计：常驻顶栏的开关 + 条件条的内容条件。
    // 只统计条件条会漏掉"只关掉 RX"这种最常见的情形。
    const int active = m_flags->activeCount() + m_filterBar->activeConditionCount();
    const QString caret = m_filterExpanded ? QStringLiteral("▾") : QStringLiteral("▸");
    // 生效条件数写在按钮上：条子收起时，这个数字是"内容条件里还有几条在用"的唯一出口。
    m_filterToggle->setText(active > 0 ? tr("过滤 · %1 %2").arg(active).arg(caret)
                                      : tr("过滤 %1").arg(caret));
    // 重置必须能清掉整组条件，因此它关心的也是这个合计数。
    m_filterBar->setResetAvailable(m_flags->activeCount() > 0);
}

void TrafficPanel::reprojectView(bool announce)
{
    // 视图与条件取得一致的唯一入口。它同时承担两件事：
    //   ① 显示设置 / 编码格式变化 —— 每行长什么样（用户要求：必须立刻作用于已收到的内容）
    //   ② 过滤条件变化 —— 哪些行进来（追溯过去要靠它，增量路径只管新行）
    // 数据源是模型的完整缓冲，所以它覆盖的是**全部已到达的数据**，而不是"此后的新行"。
    //
    // 投影成本实测约 6.9 µs/条（见 tests/perf/traffic_perf.cpp）：
    // 关掉那个编辑块优化会让它退回 360 µs/条，5 万条就是 18 秒——这里的每一次调用
    // 都建立在 TrafficTextView::appendMessages 那个 beginEditBlock 之上。
    const int rows = m_model->rowCount();
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
        if (m_matcher.accepts(message)) accepted.append(message);
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
    markReprojected();
    if (follow) m_text->scrollToBottom();
    else if (scrollFraction > 0.0)
        scroll->setValue(qRound(scrollFraction * scroll->maximum()));
    updateSummary();
    // 摘要要说明"这是重投影结果"，否则用户会以为视图一直是这个样子。
    if (announce && rows > 0)
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

void TrafficPanel::markReprojected()
{
    m_projectedOptions = m_filterOptions;
    m_projectedFormat = currentDisplayFormat();
    m_projectedDisplay = m_displayOptions;
    m_everProjected = true;
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
    const bool filtering = m_flags->activeCount() + m_filterBar->activeConditionCount() > 0;
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
    reportSummary(m_filteredOut > 0
            ? tr("共 %1 条 · 过滤掉 %2 条").arg(m_totalAccepted).arg(m_filteredOut)
            : tr("共 %1 条").arg(m_totalAccepted));
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
    const auto path = QFileDialog::getSaveFileName(this, tr("导出记录"), "easyDebug-preview.tsv", tr("制表符文件 (*.tsv)"));
    if (path.isEmpty()) return;
    // 导出列与显示列解耦：导出的是完整的结构化事实。
    // 时间用 ISO 格式带毫秒，方向/边界/来源用稳定名称，HEX 用大写无分隔。
    QByteArray output("# easyDebug UI preview - sample data only\n"
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

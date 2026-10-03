#include "trafficpanel.h"
#include "traffictextview.h"
#include "ui/common/combometrics.h"
#include "ui/common/endpointformat.h"
#include "ui/common/tintedicon.h"
#include "persistence/preferencesstore.h"

#include <QActionGroup>
#include <QColor>
#include <QComboBox>
#include <QFileDialog>
#include <QFile>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// 顶栏按钮与菜单条目的字形尺寸，与动作按钮一致。
constexpr int BarIconSize = 16;

// 单条消息最多保留的预览字节数。原始长度另存于 rawLength，显示的是"本条实际有多大"，
// 而不是"我们留下了多少"——两者不等即说明发生了截断。
constexpr int PreviewLimit = 4096;

// 整个模型保留的原始字节预算，与 ARCHITECTURE.md §7 的 GUI 历史模型上限一致。
constexpr qint64 HistoryByteBudget = 4 * 1024 * 1024;

// 历史上的条数上限：与字节预算先到者为准，对应 ARCHITECTURE.md §7.1 的
// "16 MiB 或 20000 条"。只限字节是不够的：串口遥测常见几十字节的小帧，
// 光靠 4 MiB 预算能堆到六万条以上，而每条 Message（时间、来源、三个 QUuid、
// 两个 QString）的元数据开销远大于它那 64 字节的载荷——"限了字节"不等于"限了内存"。
constexpr int HistoryRowBudget = 20000;

// 真实消息到达的攒批窗口：一帧左右的时间足够把同一次 readyRead 的多条消息
// 合成一个插入通知；再长会让人感觉记录"迟一拍"。
constexpr int FlushIntervalMs = 16;

// 渲染缓存条数上限。命中率只需覆盖"当前可见的一屏"，上限之外整体清空让热点自然重填；
// 单条缓存约 1KB 量级，512 条把额外内存压在 1MB 以内。
constexpr int RenderCacheLimit = 512;

// 丢弃最旧记录直到回到预算内。逐条移除而不是整体截断，保留最新的连续上下文。
// 字节与条数两个上限先到者为准；返回丢弃的条数，由调用方决定用哪种模型通知。
int trimToBudget(QList<Traffic::Message> &records, qint64 &bytes)
{
    int dropped = 0;
    while (!records.isEmpty()
           && (bytes > HistoryByteBudget || records.size() > HistoryRowBudget)) {
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

TrafficModel::TrafficModel(QObject *parent) : QAbstractTableModel(parent) {}
int TrafficModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_records.size(); }
int TrafficModel::columnCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : ColumnCount; }
const Traffic::Message &TrafficModel::record(int row) const { return m_records.at(row); }

QString TrafficModel::renderDataColumn(int row) const
{
    const auto &r = m_records.at(row);

    // 按消息 id 查缓存：id 在会话内唯一，淘汰旧记录造成的行号平移不会让它失效，
    // 因此滚动与追加都不会因为缓存错位而重复渲染。
    const auto cached = m_renderCache.constFind(r.id);
    if (cached != m_renderCache.constEnd())
        return *cached;

    auto text = PayloadCodec::render(r.raw, m_format);
    if (r.truncated) text += QObject::tr(" …");
    if (r.isFragment() && r.hasMoreFragments) text += QObject::tr(" ⏎");

    if (m_renderCache.size() >= RenderCacheLimit)
        m_renderCache.clear();
    m_renderCache.insert(r.id, text);
    return text;
}

void TrafficModel::clearRenderCache()
{
    m_renderCache.clear();
}

QVariant TrafficModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_records.size()) return {};
    const auto &r = m_records.at(index.row());
    if (role == Qt::ForegroundRole && index.column() == DirectionColumn)
        return QColor(r.direction == Traffic::Direction::Transmit ? "#3565ce" : "#008577");
    // 对端地址用等宽字体：IP:port 的数字要能上下对齐着扫，比例字体会让 52344 和 52345 看着一样长。
    if (role == Qt::FontRole && (index.column() == TimeColumn || index.column() == BytesColumn
            || index.column() == PeerColumn)) {
        auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        font.setPointSize(10);
        return font;
    }
    if (role == Qt::TextAlignmentRole && (index.column() == DirectionColumn || index.column() == BytesColumn))
        return int(Qt::AlignCenter);
    if (role == Qt::ToolTipRole) {
        // 悬停展示结构化字段本身：真实长度、边界语义、关联 id，都不是显示列里的内容。
        QStringList lines{QObject::tr("原始字节（HEX）：%1")
                              .arg(PayloadCodec::render(r.raw, PayloadCodec::Format::Hex))};
        if (r.truncated)
            lines.prepend(QObject::tr("已截断：共 %1 字节，仅保留前 %2 字节").arg(r.rawLength).arg(r.raw.size()));
        lines << QObject::tr("边界：%1").arg(Traffic::framingName(r.framing));
        if (r.isFragment())
            lines << QObject::tr("片段 %1%2").arg(r.fragmentIndex).arg(r.hasMoreFragments ? QObject::tr("，后续仍有片段") : QString());
        if (r.state != Traffic::State::None)
            lines << QObject::tr("发送状态：%1%2").arg(Traffic::stateName(r.state),
                                 r.statusText.isEmpty() ? QString() : QObject::tr("（%1）").arg(r.statusText));
        if (r.durationUs >= 0) lines << QObject::tr("本地耗时：%1 µs").arg(r.durationUs);
        if (!r.detail.isEmpty()) lines << r.detail;
        if (!r.requestId.isNull()) lines << QObject::tr("请求：%1").arg(r.requestId.toString());
        if (!r.routeId.isNull()) lines << QObject::tr("路由：%1").arg(r.routeId.toString());
        if (!r.frameId.isNull()) lines << QObject::tr("来源帧：%1").arg(r.frameId.toString());
        return lines.join(QLatin1Char('\n'));
    }
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
    case TimeColumn: return r.timestamp.toString("HH:mm:ss.zzz");
    // 行标识由来源的行号与类型派生，不随消息落库：改了端点类型，旧记录的标签也跟着更新。
    case SourceColumn: return endpointLabel(r.source.endpointIndex, r.source.endpointKind);
    // 对端独占一列。挤进端口列的第二行会被 33px 的行高裁掉，而 TCP Server 的
    // "哪个客户端发来的"恰恰是排障时最先要看的信息，不能因为放不下就不显示。
    case PeerColumn: return r.source.hasPeer() ? r.source.peerAddress : QStringLiteral("—");
    case DirectionColumn: {
        // 方向是主语义。来源只在"与方向不绑定"的场合才有信息量（手动/定时/转发/
        // 示例），以复合标记挂在方向上；RX 恒为自动来源，"接收"不重复播报——
        // 单独的来源列在转发落地前与方向高度绑定，只是屏幕宽度的净浪费。
        auto name = Traffic::directionName(r.direction);
        if (r.isData() && r.origin != Traffic::Origin::Automatic)
            name += QStringLiteral("·") + Traffic::originName(r.origin);
        if (r.state == Traffic::State::None || r.state == Traffic::State::LocalCompleted) return name;
        return QStringLiteral("%1 %2").arg(name, Traffic::stateName(r.state));
    }
    // 显示真实长度，截断标记交给悬停与状态列，不在数字里掺信息。
    case BytesColumn: return r.displayBytes();
    case DataColumn: return renderDataColumn(index.row());
    default: return {};
    }
}

QVariant TrafficModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return QStringList{tr("时间"), tr("端口"), tr("对端"), tr("方向"), tr("字节"), tr("数据内容")}.value(section);
}

void TrafficModel::replaceRecords(QList<Traffic::Message> records)
{
    // This preview model has a hard cap; live batching belongs to the I/O milestone.
    if (records.size() > 2000) records = records.last(2000);
    for (auto &r : records)
        applyPreviewLimit(r);
    enforceBudget(records);
    m_rawBytes = 0;
    for (const auto &r : records)
        m_rawBytes += r.raw.size();
    beginResetModel();
    m_records = std::move(records);
    clearRenderCache();  // 记录全部替换，缓存失效
    endResetModel();
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
           && (keptBytes + incomingBytes > HistoryByteBudget
               || (m_records.size() - drop) + added > HistoryRowBudget)) {
        keptBytes -= m_records.at(drop).raw.size();
        ++drop;
    }

    // 增量的移除 + 插入，而不是整表 beginResetModel：reset 会让视图丢弃选中项、
    // 滚动位置与全部行状态，是"日志写满之后越来越卡"的直接来源。
    // 渲染缓存按消息 id 索引，淘汰造成的行号平移不会让它失效。
    if (drop > 0) {
        beginRemoveRows({}, 0, drop - 1);
        m_records.remove(0, drop);
        endRemoveRows();
    }
    m_rawBytes = keptBytes + incomingBytes;
    const int first = m_records.size();
    beginInsertRows({}, first, first + added - 1);
    m_records += records;
    endInsertRows();
}

void TrafficModel::setFormat(PayloadCodec::Format format)
{
    // 渲染格式只影响显示列，原始字节始终是事实依据。
    m_format = format;
    clearRenderCache();
    if (!m_records.isEmpty())
        emit dataChanged(index(0, DataColumn), index(m_records.size() - 1, DataColumn));
}

TrafficFilter::TrafficFilter(QObject *parent) : QSortFilterProxyModel(parent) {}
void TrafficFilter::setDisplayFormat(PayloadCodec::Format format)
{
    m_format = format;
}
void TrafficFilter::setOptions(const Options &options)
{
    beginFilterChange();
    m_options = options;
    m_options.search = options.search.trimmed();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool TrafficFilter::accepts(const Traffic::Message &r, const Options &m_options, PayloadCodec::Format m_format)
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
    default:
        // 状态与错误不属于任何一个方向开关，跟随 RX 一起显示。
        if (!m_options.rx && !m_options.tx) return false;
        break;
    }
    // 示例数据单独开关：调试时最怕把预览数据和真实收发混在一起看。
    if (r.origin == Traffic::Origin::Sample && !m_options.sample) return false;
    if (m_options.textOnly && r.isFragment()) return false;
    // 长度用真实字节数过滤，不用预览后剩下的长度，否则大消息会被误判为小。
    if (r.displayBytes() < m_options.minBytes) return false;
    if (m_options.search.isEmpty()) return true;

    // 搜索匹配当前显示格式的渲染文本、UTF-8 文本与两种 HEX 写法，所见即可搜；
    // 也匹配来源的对端地址和自由说明文本。逐项短路，命中即返回，
    // 避免对每行做无谓的渲染与 HEX 展开。
    const auto needle = m_options.search;
    if (r.source.peerAddress.contains(needle, Qt::CaseInsensitive)) return true;
    if (r.source.endpointName.contains(needle, Qt::CaseInsensitive)) return true;
    if (r.statusText.contains(needle, Qt::CaseInsensitive)) return true;
    if (r.detail.contains(needle, Qt::CaseInsensitive)) return true;
    if (PayloadCodec::render(r.raw, m_format).contains(needle, Qt::CaseInsensitive)) return true;
    if (PayloadCodec::render(r.raw, PayloadCodec::Format::Utf8).contains(needle, Qt::CaseInsensitive)) return true;
    if (QString(r.raw.toHex()).contains(needle, Qt::CaseInsensitive)) return true;
    return QString::fromLatin1(r.raw.toHex(' ')).contains(needle, Qt::CaseInsensitive);
}

bool TrafficFilter::filterAcceptsRow(int row, const QModelIndex &) const
{
    return accepts(static_cast<TrafficModel *>(sourceModel())->record(row), m_options, m_format);
}

TrafficPanel::TrafficPanel(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("Surface");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(12);

    // 单行顶栏：示例数据标记、A/B 端点开关、编码切换、过滤关键词、显示设置、导出、
    // 滚动到底、清空。
    auto *top = new QHBoxLayout;
    top->setSpacing(6);
    m_exampleBadge = new QLabel(tr("等待数据"));
    m_exampleBadge->setObjectName("Muted");
    top->addWidget(m_exampleBadge);

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

    m_search = new QLineEdit;
    m_search->setObjectName("trafficSearch");
    m_search->setPlaceholderText(tr("搜索文本 / HEX / 对端地址"));
    m_search->setClearButtonEnabled(true);
    top->addWidget(m_search, 1);

    // 日志显示设置：方向、时间戳列与暂停收进菜单，顶栏只留一个入口。
    auto *settings = new QPushButton(tr("显示设置"));
    settings->setObjectName("displaySettings");
    settings->setIcon(mutedIcon(QStringLiteral(":/icons/cog.svg"), BarIconSize));
    settings->setToolTip(tr("日志显示选项：RX/TX、暂停显示"));
    auto *displayMenu = new QMenu(settings);
    m_rxAction = displayMenu->addAction(tr("显示 RX"));
    m_rxAction->setObjectName("showRx");
    m_rxAction->setCheckable(true);
    m_rxAction->setChecked(true);
    connect(m_rxAction, &QAction::toggled, this, &TrafficPanel::updateFilter);
    m_txAction = displayMenu->addAction(tr("显示 TX"));
    m_txAction->setObjectName("showTx");
    m_txAction->setCheckable(true);
    m_txAction->setChecked(true);
    connect(m_txAction, &QAction::toggled, this, &TrafficPanel::updateFilter);
    m_sampleAction = displayMenu->addAction(tr("显示示例数据"));
    m_sampleAction->setCheckable(true);
    m_sampleAction->setChecked(true);
    connect(m_sampleAction, &QAction::toggled, this, &TrafficPanel::updateFilter);
    m_wholeAction = displayMenu->addAction(tr("只看完整消息"));
    m_wholeAction->setObjectName("wholeTraffic");
    m_wholeAction->setCheckable(true);
    // 默认关：字节流片段在真实抓包调试中是常态，默认滤掉会让人以为设备没发数据。
    m_wholeAction->setToolTip(tr("滤掉字节流片段，只显示按数据报或协议边界完整的记录"));
    connect(m_wholeAction, &QAction::toggled, this, &TrafficPanel::updateFilter);
    displayMenu->addSeparator();
    // 最小长度用预设档位而不是输入框：调试时要么"不要噪声心跳"要么"只看大块数据"，
    // 很少需要精确到个位数。互斥用 QActionGroup，和顶栏 A/B 开关同一套勾选样式。
    auto *lengthMenu = displayMenu->addMenu(tr("最小字节数"));
    auto *lengthGroup = new QActionGroup(lengthMenu);
    lengthGroup->setExclusive(true);
    for (const int bytes : {0, 1, 8, 16, 64}) {
        auto *action = lengthMenu->addAction(bytes == 0 ? tr("不限") : tr("≥ %1 字节").arg(bytes));
        action->setCheckable(true);
        action->setChecked(bytes == 0);
        action->setData(bytes);
        connect(action, &QAction::toggled, this, &TrafficPanel::updateFilter);
        lengthGroup->addAction(action);
    }
    m_lengthMenu = lengthMenu;
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
    auto *pauseAction = displayMenu->addAction(tr("暂停显示"));
    pauseAction->setCheckable(true);
    connect(pauseAction, &QAction::toggled, this, [this](bool checked) {
        m_paused = checked;
        m_droppedWhilePaused = 0;
        updateSummary();
    });
    settings->setMenu(displayMenu);
    top->addWidget(settings);

    auto *exportButton = new QPushButton(tr("导出"));
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
    layout->addLayout(top);

    m_model = new TrafficModel(this);
    // 攒批入模：16ms 一拍把同批消息合成一次插入通知，事件开销与消息速率解耦。
    m_flushTimer = new QTimer(this);
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(FlushIntervalMs);
    connect(m_flushTimer, &QTimer::timeout, this, &TrafficPanel::flushPending);
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

    auto *bottom = new QHBoxLayout;
    m_summary = new QLabel;
    m_summary->setObjectName("Muted");
    bottom->addWidget(m_summary, 1);
    layout->addLayout(bottom);

    // RX/TX 的过滤连接在创建菜单动作时已完成，这里不再重复 connect。
    connect(m_search, &QLineEdit::textChanged, this, &TrafficPanel::updateFilter);
    connect(m_format, &QComboBox::currentIndexChanged, this, [this](int) {
        m_model->setFormat(currentDisplayFormat());
        savePreferences();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        m_pending.clear();
        m_model->replaceRecords({});
        m_text->clearAll();
        m_totalAccepted = 0;
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
    if (m_paused || ports.isEmpty()) return;
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
    m_text->clearAll();
    // 示例数据与 replaceRecords 同语义：整批直达模型，不经显示过滤。
    // 文本行按当前显示格式固化。
    m_totalAccepted = records.size();
    m_text->appendMessages(records, currentDisplayFormat());
    updateSummary();
    if (m_follow->isChecked()) m_text->scrollToBottom();
}

void TrafficPanel::appendMessages(const QList<Traffic::Message> &messages)
{
    if (messages.isEmpty()) return;
    if (m_paused) {
        // 暂停只冻结显示，不无限积压：直接丢弃并累计缺口，恢复时计数清零。
        m_droppedWhilePaused += messages.size();
        updateSummary();
        return;
    }
    // 逐条 beginInsertRows + scrollToBottom 的事件开销与消息速率成正比；
    // 先攒进缓冲，16ms 一拍整批入模，插入通知次数与消息速率解耦。
    m_pending += messages;
    if (!m_flushTimer->isActive()) m_flushTimer->start();
}

void TrafficPanel::flushPending()
{
    if (m_pending.isEmpty()) return;
    const auto batch = m_pending;
    m_pending.clear();
    m_model->appendRecords(batch);
    writeLog(batch); // 记录与显示同流：进模型的批次才落日志，暂停即同停
    // 文本视图按到达时的过滤条件逐条判定（TrafficFilter::accepts 的唯一显示出口），
    // 行按当前显示格式固化后不再重渲染——控制台语义，格式切换只影响新行。
    const auto format = currentDisplayFormat();
    QList<Traffic::Message> accepted;
    accepted.reserve(batch.size());
    for (const auto &message : batch)
        if (TrafficFilter::accepts(message, m_textOptions, format)) accepted.append(message);
    m_totalAccepted += accepted.size();
    m_text->appendMessages(accepted, format);
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
        // 错误提示直接顶替摘要文本；界面状态没有其他变化，不需要走 updateSummary。
        m_summary->setText(tr("日志文件无法打开：%1").arg(m_logFile->errorString()));
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
        m_summary->setText(tr("日志写入失败，记录已停止：%1").arg(m_logFile->errorString()));
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
    m_rxAction->setChecked(m_preferences->value("traffic/rx", true).toBool());
    m_txAction->setChecked(m_preferences->value("traffic/tx", true).toBool());
    m_sampleAction->setChecked(m_preferences->value("traffic/sample", true).toBool());
    m_wholeAction->setChecked(m_preferences->value("traffic/whole", false).toBool());
    m_follow->setChecked(m_preferences->value("traffic/follow", true).toBool());
    m_portAButton->setChecked(m_preferences->value("traffic/portA", true).toBool());
    m_portBButton->setChecked(m_preferences->value("traffic/portB", true).toBool());
    m_logPath = m_preferences->value("traffic/recordPath").toString();
    const bool recordOn = m_preferences->value("traffic/record", false).toBool();
    if (recordOn) {
        // 上次开着记录：沿用原文件继续追加；文件不可用则复位勾选（含提示）。
        m_recordAction->setChecked(true);
    }
    const auto minBytes = m_preferences->value("traffic/minBytes", 0).toInt();
    for (auto *action : m_lengthMenu->actions())
        if (action->data().toInt() == minBytes) action->setChecked(true);
    m_loading = false;
    updatePortVisibility();
    updateFilter(); // 恢复完成后统一刷新一次过滤
}

void TrafficPanel::savePreferences()
{
    if (!m_preferences || m_loading) return;
    m_preferences->setValue("traffic/format", m_format->currentText());
    m_preferences->setValue("traffic/rx", m_rxAction->isChecked());
    m_preferences->setValue("traffic/tx", m_txAction->isChecked());
    m_preferences->setValue("traffic/sample", m_sampleAction->isChecked());
    m_preferences->setValue("traffic/whole", m_wholeAction->isChecked());
    m_preferences->setValue("traffic/follow", m_follow->isChecked());
    m_preferences->setValue("traffic/portA", m_portAButton->isChecked());
    m_preferences->setValue("traffic/portB", m_portBButton->isChecked());
    m_preferences->setValue("traffic/record", m_recordAction->isChecked());
    if (!m_logPath.isEmpty()) m_preferences->setValue("traffic/recordPath", m_logPath);
    for (auto *action : m_lengthMenu->actions())
        if (action->isChecked()) m_preferences->setValue("traffic/minBytes", action->data().toInt());
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
    TrafficFilter::Options options;
    options.endpoints = m_visibleEndpoints;
    options.rx = m_rxAction->isChecked();
    options.tx = m_txAction->isChecked();
    options.sample = m_sampleAction->isChecked();
    options.textOnly = m_wholeAction->isChecked();
    options.search = m_search->text();
    // 长度档位读的是被选中的动作，不缓存上次值，避免与菜单实际状态失步。
    for (const auto *action : m_lengthMenu->actions())
        if (action->isChecked()) options.minBytes = action->data().toInt();
    // 文本视图的到达时过滤用这一份条件的快照；写入点只有这里。
    m_textOptions = options;
    updateSummary();
    savePreferences(); // 过滤开关的汇聚点：任一开关变化即落盘
}

void TrafficPanel::updateSummary()
{
    // 模型是导出用的结构化存储（有上限），屏幕上的历史在文本视图里（上限高得多），
    // 摘要按"会话累计接收条数"播报，不随任何一方的淘汰缩水。
    m_stack->setCurrentWidget(m_totalAccepted == 0 ? static_cast<QWidget *>(m_empty)
                                                   : static_cast<QWidget *>(m_text));
    // 空态提示由内容区的空态文案承担；顶栏标记只提醒示例数据不计入统计。
    const bool sampleVisible = m_sampleAction->isChecked();
    // 文案没变就不 setText：QLabel::setText 每次都重算尺寸并请求重绘，
    // 16ms 一拍的刷新节奏下，恒定文案的重复赋值会变成持续的无效重排。
    const auto badge = sampleVisible ? tr("示例数据 · 不计入收发统计") : tr("仅显示真实收发");
    if (m_exampleBadge->text() != badge) m_exampleBadge->setText(badge);
    m_exampleBadge->setVisible(m_totalAccepted > 0);
    const auto summary = m_paused ? tr("显示已暂停 · 期间丢弃 %1 条").arg(m_droppedWhilePaused)
                                 : tr("共 %1 条").arg(m_totalAccepted);
    if (m_summary->text() != summary) m_summary->setText(summary);
}

void TrafficPanel::exportVisible()
{
    if (!m_model->rowCount()) { m_summary->setText(tr("当前没有可导出的记录")); return; }
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
    m_summary->setText(ok ? tr("记录已导出") : tr("导出失败：%1").arg(file.errorString()));
}

#include "trafficpanel.h"
#include "ui/common/combometrics.h"
#include "ui/common/endpointformat.h"
#include "ui/common/tintedicon.h"
#include "persistence/preferencesstore.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QFileDialog>
#include <QFile>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QStackedWidget>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableView>
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

// 真实消息到达的攒批窗口：一帧左右的时间足够把同一次 readyRead 的多条消息
// 合成一个插入通知；再长会让人感觉记录"迟一拍"。
constexpr int FlushIntervalMs = 16;

// 换行查看模式下数据内容列的最大行高（以文本行数计）。
constexpr int WrappedRowLines = 6;

// 钳制行高在文本高度之外保留的绘制余量：交替行底色、焦点框与上下留白。
constexpr int WrappedRowPaddingPx = 8;

// 行高钳制委托。换行开启时 QTableView 会按内容给出行高，4096 字节的 HEX
// 折行能有上百行，直接把滚动体验顶穿；这里把单行钳在几行文本以内，
// 更长的内容走悬停提示——换行是查看模式，不是排版模式。
// 关闭换行时单行高度远小于钳制值，常驻无副作用。
class TrafficRowDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        if (index.column() == TrafficModel::DataColumn)
            size.setHeight(qMin(size.height(), option.fontMetrics.height() * WrappedRowLines + WrappedRowPaddingPx));
        return size;
    }
};

// 丢弃最旧记录直到回到预算内。逐条移除而不是整体截断，保留最新的连续上下文。
void enforceBudget(QList<Traffic::Message> &records){
    qint64 bytes = 0;
    for (const auto &record : records)
        bytes += record.raw.size();
    while (bytes > HistoryByteBudget && !records.isEmpty())
        bytes -= records.takeFirst().raw.size();
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
    case DataColumn: {
        auto text = PayloadCodec::render(r.raw, m_format);
        if (r.truncated) text += QObject::tr(" …");
        if (r.isFragment() && r.hasMoreFragments) text += QObject::tr(" ⏎");
        return text;
    }
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
    beginResetModel();
    m_records = std::move(records);
    endResetModel();
}

void TrafficModel::appendRecords(QList<Traffic::Message> records)
{
    if (records.isEmpty()) return;
    for (auto &r : records)
        applyPreviewLimit(r);
    const int added = records.size();
    m_records += records;
    const int before = m_records.size();
    enforceBudget(m_records);
    if (m_records.size() != before) {
        // 超出预算时丢的是最旧记录，行号整体平移，用 reset 让视图和索引重新对齐。
        beginResetModel();
        endResetModel();
        return;
    }
    beginInsertRows({}, before - added, before - 1);
    endInsertRows();
}

void TrafficModel::setFormat(PayloadCodec::Format format)
{
    // 渲染格式只影响显示列，原始字节始终是事实依据。
    m_format = format;
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

bool TrafficFilter::filterAcceptsRow(int row, const QModelIndex &) const
{
    const auto &r = static_cast<TrafficModel *>(sourceModel())->record(row);
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
    settings->setToolTip(tr("日志显示选项：RX/TX、时间戳列、暂停显示"));
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
    auto *timeAction = displayMenu->addAction(tr("时间戳列"));
    timeAction->setCheckable(true);
    timeAction->setChecked(true);
    connect(timeAction, &QAction::toggled, this, [this](bool checked) {
        m_table->setColumnHidden(TrafficModel::TimeColumn, !checked);
        savePreferences();
    });
    // 自动换行默认关：单行省略是扫描模式，折行是阅读模式，两者切换是明确的用户意图。
    m_wrapAction = displayMenu->addAction(tr("自动换行"));
    m_wrapAction->setObjectName("wrapTraffic");
    m_wrapAction->setCheckable(true);
    m_wrapAction->setToolTip(tr("数据内容超出列宽时折行显示（行高最多 6 行，更多内容见悬停提示）；大流量时按内容计算行高的开销较大，建议关闭"));
    connect(m_wrapAction, &QAction::toggled, this, [this](bool checked) {
        applyWrapMode(checked);
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
    m_filter = new TrafficFilter(this);
    // 真实通信时消息可能成百条/秒，而 fitFixedColumns 要遍历全部可见行；
    // 防抖到 200ms 一拍，宽度仍然及时，代价却与消息速率无关。
    m_fitDebounce = new QTimer(this);
    m_fitDebounce->setSingleShot(true);
    m_fitDebounce->setInterval(200);
    connect(m_fitDebounce, &QTimer::timeout, this, &TrafficPanel::fitFixedColumns);
    // 攒批入模：16ms 一拍把同批消息合成一次插入通知，事件开销与消息速率解耦。
    m_flushTimer = new QTimer(this);
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(FlushIntervalMs);
    connect(m_flushTimer, &QTimer::timeout, this, &TrafficPanel::flushPending);
    m_filter->setSourceModel(m_model);
    m_table = new QTableView;
    m_table->setObjectName("trafficTable");
    m_table->setModel(m_filter);
    m_table->setItemDelegate(new TrafficRowDelegate(m_table));
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(33);
    auto *header = m_table->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    // 列宽一律按枚举写死值。裸下标在上次加"对端"列时就悄悄错位过一次，
    // 枚举让"宽度跟着哪一列走"这件事在代码里是显式的。
    header->setSectionResizeMode(TrafficModel::DataColumn, QHeaderView::Stretch);
    m_table->setColumnWidth(TrafficModel::TimeColumn, 116);
    // 方向列要容得下复合标记"TX·定时"（转发/示例同理），64px 是纯 RX/TX 时代的宽度。
    m_table->setColumnWidth(TrafficModel::DirectionColumn, 88);
    m_table->setColumnWidth(TrafficModel::BytesColumn, 52);
    // 端口与对端两列由 fitFixedColumns() 按实际内容定宽，构造时先给个下限。
    m_table->setColumnWidth(TrafficModel::SourceColumn, 96);
    m_table->setColumnWidth(TrafficModel::PeerColumn, 120);
    fitFixedColumns();
    m_stack = new QStackedWidget;
    m_stack->addWidget(m_table);
    auto *empty = new QLabel(tr("等待接收数据\n\n在上方配置 A、B 端点，收发记录将在这里显示。"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setObjectName("EmptyState");
    empty->setWordWrap(true);
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
        const auto format = currentDisplayFormat();
        m_model->setFormat(format);
        m_filter->setDisplayFormat(format);
        updateFilter(); // 搜索匹配当前格式的渲染文本，切格式后需要重新过滤。
        savePreferences();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        m_pending.clear();
        m_model->replaceRecords({});
        updateSummary();
    });
    connect(exportButton, &QPushButton::clicked, this, &TrafficPanel::exportVisible);
    connect(m_follow, &QPushButton::toggled, this, [this](bool follow) {
        if (follow) m_table->scrollToBottom();
        savePreferences();
    });
    // 任何形式的向上翻阅（拖动、滚轮、翻页）都解除“滚动到底”，与悬停提示的承诺一致。
    connect(m_table->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int position) {
        if (position < m_table->verticalScrollBar()->maximum()) m_follow->setChecked(false);
    });
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested, this, [this](const QPoint &point) {
        const auto index = m_table->indexAt(point);
        if (!index.isValid()) return;
        const auto &record = m_model->record(m_filter->mapToSource(index).row());
        QMenu menu;
        auto *copy = menu.addAction(tr("复制 HEX"));
        auto *reuse = menu.addAction(tr("填入发送区"));
        menu.addSeparator();
        // 边界是消息的事实属性，不是显示设置：把"只显示完整消息"的开关反应用到这一行，
        // 比让用户回到顶栏菜单里找同一个条件更直接。
        auto *onlyWhole = menu.addAction(tr("只看完整消息"));
        onlyWhole->setCheckable(true);
        onlyWhole->setChecked(m_wholeAction->isChecked());
        menu.addSeparator();
        // 关联 id 可直接复制，用来和文件记录、路由规则对照。
        auto *copyIds = menu.addAction(record.requestId.isNull() && record.routeId.isNull() && record.frameId.isNull()
                ? tr("复制行标识")
                : tr("复制关联 ID"));
        const auto *chosen = menu.exec(m_table->viewport()->mapToGlobal(point));
        if (chosen == copy) QApplication::clipboard()->setText(PayloadCodec::render(record.raw, PayloadCodec::Format::Hex));
        if (chosen == reuse) emit reusePayload(record.raw);
        if (chosen == onlyWhole) m_wholeAction->setChecked(!m_wholeAction->isChecked());
        if (chosen == copyIds) {
            QStringList ids;
            if (!record.frameId.isNull()) ids << tr("帧 %1").arg(record.frameId.toString());
            if (!record.routeId.isNull()) ids << tr("路由 %1").arg(record.routeId.toString());
            if (!record.requestId.isNull()) ids << tr("请求 %1").arg(record.requestId.toString());
            if (ids.isEmpty())
                ids << QStringLiteral("%1 · %2").arg(endpointLabel(record.source.endpointIndex, record.source.endpointKind),
                                        record.timestamp.toString(Qt::ISODateWithMs));
            QApplication::clipboard()->setText(ids.join(QLatin1Char('\n')));
        }
    });
    updateSummary();
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
    fitFixedColumns(); // 端点类型和对端地址是刚定下来的，列宽要跟着量一次
    updateSummary();
    if (m_follow->isChecked()) m_table->scrollToBottom();
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
    m_fitDebounce->start(); // 列宽自适应限流执行，见构造处的说明
    updateSummary();
    writeLog(batch); // 记录与显示同流：进模型的批次才落日志，暂停即同停
    if (m_follow->isChecked()) m_table->scrollToBottom();
}

void TrafficPanel::applyWrapMode(bool wrap)
{
    m_table->setWordWrap(wrap);
    auto *header = m_table->verticalHeader();
    header->setSectionResizeMode(wrap ? QHeaderView::ResizeToContents : QHeaderView::Interactive);
    if (!wrap) header->setDefaultSectionSize(33);
    // 数据不动，只让行高按当前模式整体重算：开启后长内容折行（委托钳到 6 行），
    // 关闭后回到统一的 33px 单行省略。行高由 ResizeToContents 模式切换与本次
    // 模型重置各自触发重算，无需再手动 resizeRowsToContents 重复算一遍。
    m_model->refreshLayout();
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
    // 若不抑制，"恢复了 rx 才轮到 wrap"的中间状态会把已存的 wrap 值覆盖掉。
    m_loading = true;
    // 只在值能对上现有格式时恢复，配置里残留的无效文本不动当前选择。
    const auto format = m_preferences->value("traffic/format").toString();
    if (m_format->findText(format) >= 0)
        m_format->setCurrentText(format);
    m_rxAction->setChecked(m_preferences->value("traffic/rx", true).toBool());
    m_txAction->setChecked(m_preferences->value("traffic/tx", true).toBool());
    m_sampleAction->setChecked(m_preferences->value("traffic/sample", true).toBool());
    m_wholeAction->setChecked(m_preferences->value("traffic/whole", false).toBool());
    m_wrapAction->setChecked(m_preferences->value("traffic/wrap", false).toBool());
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
    // 列宽与隐藏列（含时间戳列开关的实际效果）一起恢复；Stretch 模式也在状态里。
    const auto headerState = m_preferences->value("traffic/header").toByteArray();
    if (!headerState.isEmpty()) m_table->horizontalHeader()->restoreState(headerState);
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
    m_preferences->setValue("traffic/wrap", m_wrapAction->isChecked());
    m_preferences->setValue("traffic/follow", m_follow->isChecked());
    m_preferences->setValue("traffic/portA", m_portAButton->isChecked());
    m_preferences->setValue("traffic/portB", m_portBButton->isChecked());
    m_preferences->setValue("traffic/record", m_recordAction->isChecked());
    if (!m_logPath.isEmpty()) m_preferences->setValue("traffic/recordPath", m_logPath);
    for (auto *action : m_lengthMenu->actions())
        if (action->isChecked()) m_preferences->setValue("traffic/minBytes", action->data().toInt());
    m_preferences->setValue("traffic/header", m_table->horizontalHeader()->saveState());
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

void TrafficPanel::fitFixedColumns()
{
    // 遍历可见行实测文字宽度，取最大值再加样式表带来的开销。只增不减：
    // 数据滚动时列宽来回跳比稍微宽一点更难用。固定列变宽的代价由 Stretch 的数据内容列吸收。
    //
    // 开销不能拍一个常数：workbench.qss 给 QTableView::item 加了 padding，Qt 绘制时
    // 还会留出焦点框和文档边距。用 QStyle::subElementRect(SE_ItemViewItemText) 问
    // 样式本身"文本区到底有多宽"，比在这里猜 padding 数值可靠——它和实际绘制同源。
    const QFontMetrics metrics = m_table->fontMetrics();
    // 量样式开销要用 QStyleOption 载体：subElementRect 会把 option 拷一份再改，
    // 传裸矩形拿不到真实的文本区。SE_ItemViewItemText 与实际绘制同源。
    QStyleOptionViewItem itemOption;
    itemOption.initFrom(m_table);
    itemOption.rect = QRect(0, 0, 400, metrics.height());
    const int overhead = qMax(0, itemOption.rect.width()
        - m_table->style()->subElementRect(QStyle::SE_ItemViewItemText, &itemOption, m_table).width());
    int portWidth = 0;
    int peerWidth = 0;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const auto &record = m_model->record(row);
        portWidth = qMax(portWidth, metrics.horizontalAdvance(endpointLabel(record.source.endpointIndex, record.source.endpointKind)));
        if (record.source.hasPeer())
            peerWidth = qMax(peerWidth, metrics.horizontalAdvance(record.source.peerAddress));
    }
    // 表头有自己的 padding，单独按表头样式算，不能和数据行共用一个开销值。
    QStyleOptionViewItem headerOption;
    headerOption.initFrom(m_table);
    headerOption.rect = QRect(0, 0, 400, 20);
    const int headerOverhead = qMax(0, headerOption.rect.width()
        - m_table->style()->subElementRect(QStyle::SE_HeaderLabel, &headerOption, m_table).width());
    const int sourceNeeded = qMax(portWidth + overhead,
        metrics.horizontalAdvance(m_model->headerData(TrafficModel::SourceColumn, Qt::Horizontal).toString()) + headerOverhead);
    const int peerNeeded = qMax(peerWidth + overhead,
        metrics.horizontalAdvance(m_model->headerData(TrafficModel::PeerColumn, Qt::Horizontal).toString()) + headerOverhead);
    m_table->setColumnWidth(TrafficModel::SourceColumn, qMax(m_table->columnWidth(TrafficModel::SourceColumn), sourceNeeded));
    m_table->setColumnWidth(TrafficModel::PeerColumn, qMax(m_table->columnWidth(TrafficModel::PeerColumn), peerNeeded));
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
    m_filter->setOptions(options);
    updateSummary();
    savePreferences(); // 过滤开关的汇聚点：任一开关变化即落盘
}

void TrafficPanel::updateSummary()
{
    const int count = m_model->rowCount();
    m_stack->setCurrentIndex(count == 0 ? 1 : 0);
    // 空态提示由表格区的空态文案承担；顶栏标记只提醒示例数据不计入统计。
    const bool sampleVisible = m_sampleAction->isChecked();
    m_exampleBadge->setText(sampleVisible ? tr("示例数据 · 不计入收发统计") : tr("仅显示真实收发"));
    m_exampleBadge->setVisible(count > 0);
    m_summary->setText(m_paused ? tr("显示已暂停 · 期间丢弃 %1 条").arg(m_droppedWhilePaused)
                              : tr("显示 %1 / %2 条  ·  右键记录可复制或填入发送区").arg(m_filter->rowCount()).arg(count));
}

void TrafficPanel::exportVisible()
{
    if (!m_filter->rowCount()) { m_summary->setText(tr("当前没有可导出的记录")); return; }
    const auto path = QFileDialog::getSaveFileName(this, tr("导出记录"), "easyDebug-preview.tsv", tr("制表符文件 (*.tsv)"));
    if (path.isEmpty()) return;
    // 导出列与显示列解耦：即使界面上隐藏了某列，导出仍是完整的结构化事实。
    // 时间用 ISO 格式带毫秒，方向/边界/来源用稳定名称，HEX 用大写无分隔。
    QByteArray output("# easyDebug UI preview - sample data only\n"
                      "Time\tPort\tPeer\tDirection\tOrigin\tFraming\tState\tBytes\tTruncated\tHEX\n");
    for (int i = 0; i < m_filter->rowCount(); ++i) {
        const auto &r = m_model->record(m_filter->mapToSource(m_filter->index(i, 0)).row());
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

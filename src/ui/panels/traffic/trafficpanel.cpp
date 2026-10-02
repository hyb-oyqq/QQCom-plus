#include "trafficpanel.h"
#include "../../common/tintedicon.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QComboBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTableView>
#include <QVBoxLayout>

namespace {

// 顶栏按钮与菜单条目的字形尺寸，与动作按钮一致。
constexpr int BarIconSize = 16;

} // namespace

TrafficModel::TrafficModel(QObject *parent) : QAbstractTableModel(parent) {}
int TrafficModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : m_records.size(); }
int TrafficModel::columnCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : 6; }
const TrafficRecord &TrafficModel::record(int row) const { return m_records.at(row); }

QVariant TrafficModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_records.size()) return {};
    const auto &r = m_records.at(index.row());
    if (role == Qt::ForegroundRole && index.column() == 2)
        return QColor(r.tx ? "#3565ce" : "#008577");
    if (role == Qt::ForegroundRole && index.column() == 5)
        return QColor("#8591a2");
    if (role == Qt::FontRole && (index.column() == 0 || index.column() == 4)) {
        auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        font.setPointSize(10);
        return font;
    }
    if (role == Qt::TextAlignmentRole && (index.column() == 2 || index.column() == 3))
        return int(Qt::AlignCenter);
    if (role == Qt::ToolTipRole)
        return tr("示例数据，仅用于界面预览\n原始字节：%1").arg(PayloadCodec::render(r.payload, PayloadCodec::Format::Hex));
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
    case 0: return r.time.toString("HH:mm:ss.zzz");
    case 1: return r.portLabel;
    case 2: return r.tx ? QStringLiteral("↑ TX") : QStringLiteral("↓ RX");
    case 3: return r.payload.size();
    case 4: return PayloadCodec::render(r.payload, m_format);
    case 5: return tr("示例");
    default: return {};
    }
}

QVariant TrafficModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    return QStringList{tr("时间"), tr("端口 / 对端"), tr("方向"), tr("字节"), tr("数据内容"), tr("来源")}.value(section);
}

void TrafficModel::replaceRecords(QList<TrafficRecord> records)
{
    // This preview model has a hard cap; live batching belongs to the I/O milestone.
    if (records.size() > 2000) records = records.last(2000);
    qsizetype bytes = 0;
    for (auto &r : records) {
        r.payload = r.payload.left(4096);
        bytes += r.payload.size();
    }
    while (bytes > 4 * 1024 * 1024 && !records.isEmpty())
        bytes -= records.takeFirst().payload.size();
    beginResetModel();
    m_records = std::move(records);
    endResetModel();
}

void TrafficModel::setFormat(PayloadCodec::Format format)
{
    // 渲染格式只影响显示列，原始字节始终是事实依据。
    m_format = format;
    if (!m_records.isEmpty())
        emit dataChanged(index(0, 4), index(m_records.size() - 1, 4));
}

TrafficFilter::TrafficFilter(QObject *parent) : QSortFilterProxyModel(parent) {}
void TrafficFilter::setDisplayFormat(PayloadCodec::Format format)
{
    m_format = format;
}
void TrafficFilter::setOptions(const QSet<QUuid> &endpoints, bool rx, bool tx, const QString &search)
{
    beginFilterChange();
    m_endpoints = endpoints;
    m_rx = rx;
    m_tx = tx;
    m_search = search.trimmed();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool TrafficFilter::filterAcceptsRow(int row, const QModelIndex &) const
{
    const auto &r = static_cast<TrafficModel *>(sourceModel())->record(row);
    // 一个端点都不勾选时不显示任何记录。
    if (m_endpoints.isEmpty() || !m_endpoints.contains(r.endpoint)) return false;
    if (r.tx ? !m_tx : !m_rx) return false;
    if (m_search.isEmpty()) return true;
    // 搜索匹配当前显示格式的渲染文本、UTF-8 文本与两种 HEX 写法，所见即可搜。
    // 逐项短路，命中即返回，避免对每行做无谓的渲染与 HEX 展开。
    if (r.portLabel.contains(m_search, Qt::CaseInsensitive)) return true;
    if (PayloadCodec::render(r.payload, m_format).contains(m_search, Qt::CaseInsensitive)) return true;
    if (PayloadCodec::render(r.payload, PayloadCodec::Format::Utf8).contains(m_search, Qt::CaseInsensitive)) return true;
    if (QString(r.payload.toHex()).contains(m_search, Qt::CaseInsensitive)) return true;
    return QString::fromLatin1(r.payload.toHex(' ')).contains(m_search, Qt::CaseInsensitive);
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
    m_search->setPlaceholderText(tr("搜索文本 / HEX / 端口"));
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
    auto *timeAction = displayMenu->addAction(tr("时间戳列"));
    timeAction->setCheckable(true);
    timeAction->setChecked(true);
    connect(timeAction, &QAction::toggled, this,
            [this](bool checked) { m_table->setColumnHidden(0, !checked); });
    displayMenu->addSeparator();
    auto *pauseAction = displayMenu->addAction(tr("暂停显示"));
    pauseAction->setCheckable(true);
    connect(pauseAction, &QAction::toggled, this, [this](bool checked) {
        m_paused = checked;
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
    m_filter->setSourceModel(m_model);
    m_table = new QTableView;
    m_table->setObjectName("trafficTable");
    m_table->setModel(m_filter);
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
    header->setSectionResizeMode(4, QHeaderView::Stretch);
    m_table->setColumnWidth(0, 116);
    m_table->setColumnWidth(1, 158);
    m_table->setColumnWidth(2, 64);
    m_table->setColumnWidth(3, 52);
    m_table->setColumnWidth(5, 58);
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
    connect(m_format, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto format = PayloadCodec::formats().value(index, PayloadCodec::Format::Hex);
        m_model->setFormat(format);
        m_filter->setDisplayFormat(format);
        updateFilter(); // 搜索匹配当前格式的渲染文本，切格式后需要重新过滤。
    });
    connect(clear, &QPushButton::clicked, this, [this] { m_model->replaceRecords({}); updateSummary(); });
    connect(exportButton, &QPushButton::clicked, this, &TrafficPanel::exportVisible);
    connect(m_follow, &QPushButton::toggled, this, [this](bool follow) { if (follow) m_table->scrollToBottom(); });
    // 任何形式的向上翻阅（拖动、滚轮、翻页）都解除“滚动到底”，与悬停提示的承诺一致。
    connect(m_table->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int position) {
        if (position < m_table->verticalScrollBar()->maximum()) m_follow->setChecked(false);
    });
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_table, &QTableView::customContextMenuRequested, this, [this](const QPoint &point) {
        const auto index = m_table->indexAt(point);
        if (!index.isValid()) return;
        const auto bytes = m_model->record(m_filter->mapToSource(index).row()).payload;
        QMenu menu;
        auto *copy = menu.addAction(tr("复制 HEX"));
        auto *reuse = menu.addAction(tr("填入发送区"));
        const auto *chosen = menu.exec(m_table->viewport()->mapToGlobal(point));
        if (chosen == copy) QApplication::clipboard()->setText(PayloadCodec::render(bytes, PayloadCodec::Format::Hex));
        if (chosen == reuse) emit reusePayload(bytes);
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
    QList<TrafficRecord> records;
    const auto start = QDateTime::currentDateTime().addSecs(-2);
    const QList<QByteArray> data{QByteArray::fromHex("010300000002c40b"), QByteArray::fromHex("010304001900286bf6"),
        QByteArray("device.ready\r\n"), QByteArray("status: OK\r\n"),
        QByteArray::fromHex("aa550601100020003f0d0a"), QByteArray("ping\r\n"), QByteArray("pong\r\n"),
        QByteArray::fromHex("01060001000119ca"), QByteArray::fromHex("01060001000119ca"),
        QStringLiteral("温度: 25.0 C\r\n").toUtf8(), QByteArray("heartbeat: 128\r\n"), QByteArray("OK\r\n")};
    for (int i = 0; i < data.size(); ++i) {
        const int index = (i / 3) % ports.size();
        const auto &port = ports.at(index);
        records.append({start.addMSecs(i * 137), port.id, endpointLabel(index, port.kind),
                        i % 3 == 0, data.at(i)});
    }
    m_model->replaceRecords(records);
    updateSummary();
    if (m_follow->isChecked()) m_table->scrollToBottom();
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
    m_filter->setOptions(m_visibleEndpoints, m_rxAction->isChecked(), m_txAction->isChecked(), m_search->text());
    updateSummary();
}

void TrafficPanel::updateSummary()
{
    const int count = m_model->rowCount();
    m_stack->setCurrentIndex(count == 0 ? 1 : 0);
    // 空态提示由表格区的空态文案承担；顶栏标记只提醒示例数据不计入统计。
    m_exampleBadge->setText(tr("示例数据 · 不计入收发统计"));
    m_exampleBadge->setVisible(count > 0);
    m_summary->setText(m_paused ? tr("显示已暂停 · 恢复后可重新载入示例")
                              : tr("显示 %1 / %2 条  ·  右键记录可复制或填入发送区").arg(m_filter->rowCount()).arg(count));
}

void TrafficPanel::exportVisible()
{
    if (!m_filter->rowCount()) { m_summary->setText(tr("当前没有可导出的记录")); return; }
    const auto path = QFileDialog::getSaveFileName(this, tr("导出示例记录"), "easyDebug-preview.tsv", tr("制表符文件 (*.tsv)"));
    if (path.isEmpty()) return;
    QByteArray output("# easyDebug UI preview - sample data only\nTime\tPort\tDirection\tBytes\tHEX\n");
    for (int i = 0; i < m_filter->rowCount(); ++i) {
        const auto &r = m_model->record(m_filter->mapToSource(m_filter->index(i, 0)).row());
        QString label = r.portLabel;
        label.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
        output += QString("%1\t%2\t%3\t%4\t%5\n").arg(r.time.toString(Qt::ISODateWithMs), label,
            r.tx ? "TX" : "RX", QString::number(r.payload.size()), PayloadCodec::render(r.payload, PayloadCodec::Format::Hex)).toUtf8();
    }
    QSaveFile file(path);
    const bool ok = file.open(QIODevice::WriteOnly) && file.write(output) == output.size() && file.commit();
    m_summary->setText(ok ? tr("示例记录已导出") : tr("导出失败：%1").arg(file.errorString()));
}

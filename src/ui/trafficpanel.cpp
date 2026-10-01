#include "trafficpanel.h"

#include <QApplication>
#include <QCheckBox>
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
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTableView>
#include <QVBoxLayout>

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
        return tr("示例数据，仅用于界面预览\n原始字节：%1").arg(QString(r.payload.toHex(' ').toUpper()));
    if (role != Qt::DisplayRole) return {};
    switch (index.column()) {
    case 0: return r.time.toString("HH:mm:ss.zzz");
    case 1: return r.name;
    case 2: return r.tx ? QStringLiteral("↑ TX") : QStringLiteral("↓ RX");
    case 3: return r.payload.size();
    case 4: {
        if (m_hex) return QString(r.payload.toHex(' ').toUpper());
        QString text;
        for (const QChar c : QString::fromUtf8(r.payload)) {
            if (c == '\r') text += "\\r";
            else if (c == '\n') text += "\\n";
            else if (c == '\t') text += "\\t";
            else if (c.unicode() < 32 || c.unicode() == 127)
                text += QString("\\x%1").arg(int(c.unicode()), 2, 16, QLatin1Char('0')).toUpper();
            else text += c;
        }
        return text;
    }
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

void TrafficModel::setHex(bool hex)
{
    m_hex = hex;
    if (!m_records.isEmpty()) emit dataChanged(index(0, 4), index(m_records.size() - 1, 4));
}

TrafficFilter::TrafficFilter(QObject *parent) : QSortFilterProxyModel(parent) {}
void TrafficFilter::setOptions(const QUuid &endpoint, bool rx, bool tx, const QString &search)
{
    beginFilterChange();
    m_endpoint = endpoint;
    m_rx = rx;
    m_tx = tx;
    m_search = search.trimmed();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool TrafficFilter::filterAcceptsRow(int row, const QModelIndex &) const
{
    const auto &r = static_cast<TrafficModel *>(sourceModel())->record(row);
    if ((!m_endpoint.isNull() && m_endpoint != r.endpoint) || (r.tx ? !m_tx : !m_rx)) return false;
    const QString hex = r.payload.toHex(' ');
    return m_search.isEmpty() || r.name.contains(m_search, Qt::CaseInsensitive)
           || QString::fromUtf8(r.payload).contains(m_search, Qt::CaseInsensitive)
           || hex.contains(m_search, Qt::CaseInsensitive)
           || QString(r.payload.toHex()).contains(m_search, Qt::CaseInsensitive);
}

TrafficPanel::TrafficPanel(QWidget *parent) : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setObjectName("Surface");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->setSpacing(12);
    auto *top = new QHBoxLayout;
    auto *title = new QLabel(tr("接收监视"));
    title->setObjectName("SectionTitle");
    top->addWidget(title);
    m_exampleBadge = new QLabel(tr("等待数据"));
    m_exampleBadge->setObjectName("Muted");
    top->addWidget(m_exampleBadge);
    top->addStretch();
    auto *pause = new QPushButton(tr("暂停显示"));
    pause->setObjectName("pauseDisplay");
    pause->setCheckable(true);
    auto *clear = new QPushButton(tr("清空"));
    clear->setObjectName("clearTraffic");
    auto *exportButton = new QPushButton(tr("导出"));
    top->addWidget(pause);
    top->addWidget(clear);
    top->addWidget(exportButton);
    layout->addLayout(top);

    auto *filters = new QHBoxLayout;
    m_source = new QComboBox;
    m_source->setObjectName("trafficSource");
    m_source->setMinimumWidth(150);
    m_rx = new QCheckBox("RX");
    m_rx->setObjectName("showRx");
    m_tx = new QCheckBox("TX");
    m_tx->setObjectName("showTx");
    m_rx->setChecked(true);
    m_tx->setChecked(true);
    m_format = new QComboBox;
    m_format->setObjectName("trafficFormat");
    m_format->addItems({tr("HEX"), tr("文本 · UTF-8")});
    auto *time = new QCheckBox(tr("时间戳"));
    time->setChecked(true);
    m_search = new QLineEdit;
    m_search->setObjectName("trafficSearch");
    m_search->setPlaceholderText(tr("搜索文本 / HEX / 端口"));
    m_search->setClearButtonEnabled(true);
    filters->addWidget(m_source);
    filters->addSpacing(4);
    filters->addWidget(m_rx);
    filters->addWidget(m_tx);
    filters->addSpacing(4);
    filters->addWidget(m_format);
    filters->addWidget(time);
    filters->addWidget(m_search, 1);
    layout->addLayout(filters);

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
    auto *empty = new QLabel(tr("等待接收数据\n\n选择并配置左侧端口，收发记录将在这里显示。\n可点击顶部“载入示例”预览数据布局。"));
    empty->setAlignment(Qt::AlignCenter);
    empty->setObjectName("EmptyState");
    m_stack->addWidget(empty);
    layout->addWidget(m_stack, 1);

    auto *bottom = new QHBoxLayout;
    m_summary = new QLabel;
    m_summary->setObjectName("Muted");
    bottom->addWidget(m_summary, 1);
    m_follow = new QCheckBox(tr("跟随最新"));
    m_follow->setChecked(true);
    bottom->addWidget(m_follow);
    layout->addLayout(bottom);

    connect(m_source, &QComboBox::currentIndexChanged, this, &TrafficPanel::updateFilter);
    connect(m_rx, &QCheckBox::toggled, this, &TrafficPanel::updateFilter);
    connect(m_tx, &QCheckBox::toggled, this, &TrafficPanel::updateFilter);
    connect(m_search, &QLineEdit::textChanged, this, &TrafficPanel::updateFilter);
    connect(m_format, &QComboBox::currentIndexChanged, this, [this](int index) { m_model->setHex(index == 0); });
    connect(time, &QCheckBox::toggled, this, [this](bool checked) { m_table->setColumnHidden(0, !checked); });
    connect(pause, &QPushButton::toggled, this, [this, pause](bool checked) {
        m_paused = checked;
        pause->setText(checked ? tr("恢复显示") : tr("暂停显示"));
        updateSummary();
    });
    connect(clear, &QPushButton::clicked, this, [this] { m_model->replaceRecords({}); updateSummary(); });
    connect(exportButton, &QPushButton::clicked, this, &TrafficPanel::exportVisible);
    connect(m_follow, &QCheckBox::toggled, this, [this](bool follow) { if (follow) m_table->scrollToBottom(); });
    connect(m_table->verticalScrollBar(), &QScrollBar::sliderMoved, this, [this](int position) {
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
        if (chosen == copy) QApplication::clipboard()->setText(bytes.toHex(' ').toUpper());
        if (chosen == reuse) emit reusePayload(bytes);
    });
    updateSummary();
}

void TrafficPanel::setPorts(const QList<PortDraft> &ports)
{
    const auto selected = m_source->currentData().toUuid();
    const QSignalBlocker blocker(m_source);
    m_source->clear();
    m_source->addItem(tr("所有端口"), QUuid());
    for (const auto &port : ports) m_source->addItem(port.name, port.id);
    m_source->setCurrentIndex(qMax(0, m_source->findData(selected)));
    updateFilter();
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
        const auto &port = ports.at((i / 3) % ports.size());
        records.append({start.addMSecs(i * 137), port.id, port.name, i % 3 == 0, data.at(i)});
    }
    m_model->replaceRecords(records);
    updateSummary();
    if (m_follow->isChecked()) m_table->scrollToBottom();
}

void TrafficPanel::updateFilter()
{
    m_filter->setOptions(m_source->currentData().toUuid(), m_rx->isChecked(), m_tx->isChecked(), m_search->text());
    updateSummary();
}

void TrafficPanel::updateSummary()
{
    const int count = m_model->rowCount();
    m_stack->setCurrentIndex(count == 0 ? 1 : 0);
    m_exampleBadge->setText(count ? tr("示例数据 · 不计入收发统计") : tr("等待数据"));
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
        QString name = r.name;
        name.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
        output += QString("%1\t%2\t%3\t%4\t%5\n").arg(r.time.toString(Qt::ISODateWithMs), name,
            r.tx ? "TX" : "RX", QString::number(r.payload.size()), QString(r.payload.toHex(' ').toUpper())).toUtf8();
    }
    QSaveFile file(path);
    const bool ok = file.open(QIODevice::WriteOnly) && file.write(output) == output.size() && file.commit();
    m_summary->setText(ok ? tr("示例记录已导出") : tr("导出失败：%1").arg(file.errorString()));
}

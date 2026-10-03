#include "trafficlogview.h"
#include "trafficpanel.h"

#include <QAbstractItemModel>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHeaderView>
#include <QHelpEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSortFilterProxyModel>
#include <QToolTip>

#include <algorithm>

namespace {

// 与旧的单行表格一致：关闭换行时每行 33px，测试和样式都按这个高度认。
constexpr int PlainRowHeight = 33;
// 单元格内边距。垂直方向两侧加起来就是换行行高里的绘制余量。
constexpr int CellPadX = 8;
constexpr int CellPadY = 4;

const QColor RowBase(255, 255, 255);
const QColor RowLine(0xEE, 0xF2, 0xF6);
const QColor MetaColor(0x5C, 0x6B, 0x7A);
const QColor BodyColor(0x1C, 0x24, 0x30);

TrafficModel *trafficModelOf(QAbstractItemModel *model)
{
    if (!model) return nullptr;
    if (auto *traffic = qobject_cast<TrafficModel *>(model)) return traffic;
    if (auto *proxy = qobject_cast<QSortFilterProxyModel *>(model))
        return qobject_cast<TrafficModel *>(proxy->sourceModel());
    return nullptr;
}

} // namespace

TrafficLogView::TrafficLogView(QWidget *parent) : QAbstractScrollArea(parent)
{
    setObjectName(QStringLiteral("trafficTable"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFrameShape(QFrame::NoFrame);
    // 整行等宽，字段才能上下对齐，看起来才是命令行而不是比例字体的表格。
    QFont console = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    console.setPointSize(10);
    setFont(console);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
    viewport()->installEventFilter(this);

    // 表头不显示。它只保存字段宽度和时间戳开关，绘制按这些宽度把一行排开。
    m_header = new QHeaderView(Qt::Horizontal, this);
    m_header->hide();
    m_header->setHighlightSections(false);
    m_header->setSectionsClickable(false);
    m_header->setSectionsMovable(false);
    connect(m_header, &QHeaderView::sectionResized, this, [this](int, int, int) {
        if (m_header) m_header->setOffset(horizontalScrollBar()->value());
        m_adjusting = true;
        updateBars();
        m_adjusting = false;
        viewport()->update();
    });
}

void TrafficLogView::setModel(QAbstractItemModel *model)
{
    if (m_model == model) return;
    if (m_model) QObject::disconnect(m_model, nullptr, this, nullptr);
    m_model = model;
    m_header->setModel(model);
    if (m_model) {
        connect(m_model, &QAbstractItemModel::modelReset, this, &TrafficLogView::relayoutRows);
        connect(m_model, &QAbstractItemModel::layoutChanged, this, &TrafficLogView::relayoutRows);
        connect(m_model, &QAbstractItemModel::rowsInserted, this, &TrafficLogView::onRowsInserted);
        connect(m_model, &QAbstractItemModel::rowsRemoved, this, &TrafficLogView::onRowsRemoved);
        connect(m_model, &QAbstractItemModel::dataChanged, this, &TrafficLogView::onDataChanged);
    }
    relayoutRows();
}

int TrafficLogView::columnWidth(int column) const
{
    return m_header ? m_header->sectionSize(column) : 0;
}

void TrafficLogView::setColumnWidth(int column, int width)
{
    if (m_header) m_header->resizeSection(column, width);
}

void TrafficLogView::setColumnHidden(int column, bool hidden)
{
    if (!m_header) return;
    m_header->setSectionHidden(column, hidden);
    m_adjusting = true;
    updateBars();
    m_adjusting = false;
    viewport()->update();
    emit contentWidthChanged();
}

int TrafficLogView::fieldWidth(int column) const
{
    if (!m_header || m_header->isSectionHidden(column)) return 0;
    return qMax(0, m_header->sectionSize(column));
}

int TrafficLogView::payloadAreaWidth() const
{
    if (viewport()->width() <= 0 || !m_header) return 0;
    int used = 0;
    for (int column = 0; column < m_header->count(); ++column) {
        if (column == TrafficModel::DataColumn) continue;
        used += fieldWidth(column);
    }
    return qMax(0, viewport()->width() - used);
}

void TrafficLogView::setWordWrap(bool wrap)
{
    if (m_wrap == wrap) return;
    m_wrap = wrap;
    relayoutRows();
}

int TrafficLogView::lineHeight() const
{
    return qMax(1, fontMetrics().height());
}

int TrafficLogView::defaultRowHeight() const
{
    return m_wrap ? lineHeight() + CellPadY * 2 : PlainRowHeight;
}

int TrafficLogView::rowHeight(int row) const
{
    if (row < 0 || row >= m_heights.size()) return defaultRowHeight();
    return m_heights.at(row);
}

QMargins TrafficLogView::cellPadding() const
{
    return QMargins(CellPadX, CellPadY, CellPadX, CellPadY);
}

int TrafficLogView::heightForRow(int viewRow) const
{
    if (!m_wrap) return PlainRowHeight;
    auto *traffic = trafficModelOf(m_model);
    int lines = 1;
    if (traffic && m_model) {
        const QModelIndex idx = m_model->index(viewRow, 0);
        int sourceRow = viewRow;
        if (auto *proxy = qobject_cast<QSortFilterProxyModel *>(m_model))
            sourceRow = proxy->mapToSource(idx).row();
        if (sourceRow >= 0) lines = qMax(1, traffic->wrappedLines(sourceRow));
    }
    return lines * lineHeight() + CellPadY * 2;
}

void TrafficLogView::rebuildPrefix()
{
    m_prefix.resize(m_heights.size() + 1);
    int y = 0;
    for (int i = 0; i < m_heights.size(); ++i) {
        m_prefix[i] = y;
        y += m_heights[i];
    }
    m_prefix[m_heights.size()] = y;
}

void TrafficLogView::updateBars()
{
    const int totalH = m_prefix.isEmpty() ? 0 : m_prefix.constLast();
    const int viewH = viewport()->height();
    auto *vbar = verticalScrollBar();
    vbar->setPageStep(qMax(1, viewH));
    vbar->setSingleStep(lineHeight());
    vbar->setRange(0, qMax(0, totalH - viewH));

    // 载荷占用字段右边剩下的宽度，一行不会比视口更宽。
    auto *hbar = horizontalScrollBar();
    hbar->setRange(0, 0);
}

void TrafficLogView::relayoutRows()
{
    const int rows = m_model ? m_model->rowCount() : 0;
    const bool atEnd = verticalScrollBar()->maximum() <= 0
        || verticalScrollBar()->value() >= verticalScrollBar()->maximum();
    m_heights.resize(rows);
    for (int row = 0; row < rows; ++row)
        m_heights[row] = heightForRow(row);
    rebuildPrefix();
    m_adjusting = true;
    updateBars();
    if (atEnd) verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    m_adjusting = false;
    viewport()->update();
}

void TrafficLogView::onRowsInserted(const QModelIndex &, int first, int last)
{
    const int count = last - first + 1;
    if (count <= 0 || !m_model || m_heights.size() + count != m_model->rowCount()) {
        relayoutRows();
        return;
    }
    m_heights.insert(first, count, PlainRowHeight);
    for (int row = first; row <= last; ++row)
        m_heights[row] = heightForRow(row);
    rebuildPrefix();
    m_adjusting = true;
    updateBars();
    m_adjusting = false;
    viewport()->update();
}

void TrafficLogView::onRowsRemoved(const QModelIndex &, int first, int last)
{
    const int count = last - first + 1;
    if (count <= 0 || !m_model || first < 0 || last >= m_heights.size()
        || m_heights.size() - count != m_model->rowCount()) {
        relayoutRows();
        return;
    }
    const int oldValue = verticalScrollBar()->value();
    const int drop = m_prefix.value(last + 1) - m_prefix.value(first);
    m_heights.remove(first, count);
    rebuildPrefix();
    m_adjusting = true;
    updateBars();
    // 头部淘汰时减掉被删高度，用户正在看的那一段不会跟着往上跳。
    if (first == 0)
        verticalScrollBar()->setValue(qBound(0, oldValue - drop, verticalScrollBar()->maximum()));
    m_adjusting = false;
    viewport()->update();
}

void TrafficLogView::onDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles)
{
    if (!m_model) return;
    if (!roles.isEmpty() && !roles.contains(Qt::DisplayRole)) {
        viewport()->update();
        return;
    }
    if (!m_wrap || m_heights.size() != m_model->rowCount()
        || bottomRight.column() < TrafficModel::DataColumn || topLeft.column() > TrafficModel::DataColumn) {
        viewport()->update();
        return;
    }
    const int last = qMin(bottomRight.row(), m_heights.size() - 1);
    bool changed = false;
    for (int row = qMax(0, topLeft.row()); row <= last; ++row) {
        const int height = heightForRow(row);
        if (m_heights[row] != height) {
            m_heights[row] = height;
            changed = true;
        }
    }
    if (changed) {
        rebuildPrefix();
        m_adjusting = true;
        updateBars();
        m_adjusting = false;
    }
    viewport()->update();
}

void TrafficLogView::scrollToBottom()
{
    m_adjusting = true;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
    m_adjusting = false;
}

void TrafficLogView::layoutChrome()
{
    if (m_header) m_header->hide();
    if (viewportMargins().top() != 0)
        setViewportMargins(0, 0, 0, 0);
}

void TrafficLogView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    layoutChrome();
    m_adjusting = true;
    updateBars();
    m_adjusting = false;
    if (viewport()->width() != m_viewportWidth) {
        m_viewportWidth = viewport()->width();
        emit contentWidthChanged();
    }
}

void TrafficLogView::showEvent(QShowEvent *event)
{
    QAbstractScrollArea::showEvent(event);
    layoutChrome();
}

void TrafficLogView::scrollContentsBy(int, int)
{
    // 行高不统一，位块搬移会对不齐。可见行很少，整视口重画比纠错便宜。
    viewport()->update();
}

int TrafficLogView::rowAtContentY(int y) const
{
    if (m_heights.isEmpty() || m_prefix.size() != m_heights.size() + 1) return -1;
    if (y >= m_prefix.constLast()) return -1;
    if (y <= 0) return 0;
    if (!m_wrap) {
        const int height = qMax(1, m_heights.first());
        return qBound(0, y / height, m_heights.size() - 1);
    }
    const auto it = std::upper_bound(m_prefix.cbegin(), m_prefix.cbegin() + m_heights.size(), y);
    return qBound(0, int(it - m_prefix.cbegin()) - 1, m_heights.size() - 1);
}

bool TrafficLogView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::ToolTip && m_model) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const int row = rowAtContentY(verticalScrollBar()->value() + help->pos().y());
        if (row >= 0) {
            const auto tip = m_model->index(row, TrafficModel::DataColumn).data(Qt::ToolTipRole).toString();
            if (!tip.isEmpty()) {
                QToolTip::showText(help->globalPos(), tip, viewport());
                return true;
            }
        }
        QToolTip::hideText();
        return true;
    }
    return QAbstractScrollArea::viewportEvent(event);
}

bool TrafficLogView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == viewport() && event->type() == QEvent::Paint) {
        QPainter painter(viewport());
        painter.setClipRegion(static_cast<QPaintEvent *>(event)->region());
        paintViewport(&painter);
        return true;
    }
    return false;
}

void TrafficLogView::paintViewport(QPainter *painter)
{
    painter->fillRect(viewport()->rect(), RowBase);
    if (!m_model || m_heights.isEmpty() || !m_header) return;

    const int scrollY = verticalScrollBar()->value();
    const int viewBottom = viewport()->height();
    int row = rowAtContentY(scrollY);
    if (row < 0) return;
    int top = m_prefix[row] - scrollY;

    const auto *traffic = trafficModelOf(m_model);
    const int wrapCols = traffic ? traffic->wrapColumns() : 0;
    const int textLine = lineHeight();
    const QFontMetrics metrics(font());
    painter->setFont(font());

    for (; row < m_heights.size() && top < viewBottom; ++row) {
        const int height = m_heights[row];
        const int textTop = m_wrap ? top + CellPadY : top + qMax(0, (height - textLine) / 2);
        int x = 0;
        for (int column = 0; column < m_header->count(); ++column) {
            if (column == TrafficModel::DataColumn) continue;
            const int width = fieldWidth(column);
            if (width <= 0) continue;
            const QModelIndex index = m_model->index(row, column);
            const QString text = index.data(Qt::DisplayRole).toString();
            QColor color = MetaColor;
            if (column == TrafficModel::DirectionColumn) {
                const QVariant foreground = index.data(Qt::ForegroundRole);
                color = foreground.isValid() ? foreground.value<QColor>() : BodyColor;
            }
            painter->setPen(color);
            const QRect textRect(x + CellPadX, textTop, qMax(0, width - CellPadX * 2), textLine);
            const int align = column == TrafficModel::BytesColumn
                ? Qt::AlignRight | Qt::AlignVCenter
                : Qt::AlignLeft | Qt::AlignVCenter;
            painter->drawText(textRect, align,
                metrics.elidedText(text, Qt::ElideRight, textRect.width()));
            x += width;
        }

        const int payloadWidth = qMax(0, viewport()->width() - x);
        const QString payload = m_model->index(row, TrafficModel::DataColumn).data(Qt::DisplayRole).toString();
        painter->setPen(BodyColor);
        const QRect payloadRect(x + CellPadX, textTop, qMax(0, payloadWidth - CellPadX * 2), textLine);
        if (!(m_wrap && wrapCols > 0)) {
            painter->drawText(payloadRect, Qt::AlignLeft | Qt::AlignVCenter,
                metrics.elidedText(payload, Qt::ElideRight, payloadRect.width()));
        } else {
            // 按字符切开，和行高用的是同一套列数。续行对齐到载荷起点。
            const int maxLines = qMax(1, (height - CellPadY * 2) / textLine);
            int y = textTop;
            int pos = 0;
            for (int line = 0; line < maxLines && pos < payload.size(); ++line) {
                const bool last = line == maxLines - 1;
                const int count = last ? payload.size() - pos : qMin(wrapCols, payload.size() - pos);
                QString chunk = payload.mid(pos, count);
                if (metrics.horizontalAdvance(chunk) > payloadRect.width())
                    chunk = metrics.elidedText(chunk, Qt::ElideRight, qMax(0, payloadRect.width()));
                painter->drawText(QRect(payloadRect.left(), y, payloadRect.width(), textLine),
                    Qt::AlignLeft | Qt::AlignVCenter, chunk);
                pos += count;
                y += textLine;
            }
        }
        painter->setPen(RowLine);
        painter->drawLine(0, top + height - 1, viewport()->width(), top + height - 1);
        top += height;
    }
}

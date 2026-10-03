#ifndef TRAFFICLOGVIEW_H
#define TRAFFICLOGVIEW_H

#include <QAbstractScrollArea>
#include <QMargins>
#include <QVector>

class QAbstractItemModel;
class QHeaderView;
class QPainter;

// 监视区的绘制层。结构化记录仍在 TrafficModel / TrafficFilter 里。
// 屏幕上是一行一条的文本，不是表格：时间、端口、对端、方向、字节、载荷排成一行，
// 折行只折载荷并缩进。列宽仍记在隐藏的表头里，用来对齐这些字段和记住时间戳开关。
//
// 不用 QTableView：它把每个行高存在 QHeaderView 的 section 里，行高一不统一
// 就离开等高快路径；高速追加再从头部淘汰时，section 还会和行对不齐，
// 折行看起来像"忽然失效"。行高改由本视图用整数前缀和自己维护，绘制只碰可见行。
class TrafficLogView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit TrafficLogView(QWidget *parent = nullptr);

    void setModel(QAbstractItemModel *model);
    QAbstractItemModel *model() const { return m_model; }

    QHeaderView *horizontalHeader() const { return m_header; }
    int columnWidth(int column) const;
    void setColumnWidth(int column, int width);
    void setColumnHidden(int column, bool hidden);
    // 载荷真正能画进去的宽度。字符预算按它算，不按已经隐藏的表头拉伸列。
    int payloadAreaWidth() const;

    bool wordWrap() const { return m_wrap; }
    void setWordWrap(bool wrap);

    // 换行开启时是"一行文本 + 绘制余量"，关闭时是统一的单行行高。
    int defaultRowHeight() const;
    int rowHeight(int row) const;
    QMargins cellPadding() const;

    void scrollToBottom();
    // 折行参数或全文显示变化后重算全部行高。只做整数运算，不测量文本。
    void relayoutRows();

    // 视图在改滚动范围时为真。这段时间里滚动条数值变化不是用户翻阅。
    bool isScrollAdjusting() const { return m_adjusting; }

signals:
    // 视口或字段宽度变了，载荷一行能放下的字符数要重算。
    void contentWidthChanged();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;
    void showEvent(QShowEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void onRowsInserted(const QModelIndex &parent, int first, int last);
    void onRowsRemoved(const QModelIndex &parent, int first, int last);
    void onDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles);
    void layoutChrome();
    void updateBars();
    void rebuildPrefix();
    int lineHeight() const;
    int heightForRow(int viewRow) const;
    int rowAtContentY(int y) const;
    void paintViewport(QPainter *painter);
    int fieldWidth(int column) const;

    QAbstractItemModel *m_model = nullptr;
    QHeaderView *m_header = nullptr;
    QVector<int> m_heights;
    QVector<int> m_prefix; // prefix[i] 是第 i 行的 y，最后一个是内容总高
    int m_viewportWidth = -1;
    bool m_wrap = false;
    bool m_adjusting = false;
};

#endif

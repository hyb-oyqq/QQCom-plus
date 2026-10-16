#ifndef COMMANDTABLEWIDGET_H
#define COMMANDTABLEWIDGET_H

#include "domain/commandset.h"
#include <QSet>
#include <QStringList>
#include <QWidget>

class QAbstractItemDelegate;
class QTableWidget;
class QPushButton;
class QTimer;
class QSpinBox;
class QComboBox;
class QCheckBox;

// 指令表格：显示和编辑单个指令集的所有指令。
// 支持添加、删除、编辑、拖拽排序、单条发送、批量发送。
class CommandTableWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CommandTableWidget(QWidget *parent = nullptr);

    // 加载指令集
    void setCommandSet(const CommandSet &set);
    CommandSet commandSet() const;

    // 获取选中的指令
    QList<CommandItem> selectedCommands() const;
    QList<CommandItem> enabledCommands() const;
    // 当前行数。只读查询，和 commandSet() 一样属于表格的自然对外接口——
    // 保持私有会逼测试去数 child 数量来推断行数，那是错的判据。
    int rowCount() const;

    // 发送框折行。关：单行，放不下就末尾省略（全文在提示里）。
    // 开：按这一列的当前宽度折行，行高跟着长高。没有空格的 AT 指令也折。
    void setContentWordWrap(bool wrap);
    bool contentWordWrap() const { return m_contentWordWrap; }

    // ---- 批量发送进度（用户钦定：要能看出"发到哪一条了"）----
    // 三个入口由**唯一持有发送时序的那一方**（CommandPanel，它按累加延时排 QTimer）
    // 驱动。表格只负责把状态画出来，不自己算时间——时序有两份就一定会在某次改动后
    // 互相错开，而"高亮跟实际发送不同步"比没有高亮更让人困惑。
    //   开始一轮：清掉上一轮的痕迹。
    //   游标落在某行：该行的勾选框转琥珀（正在发），此前发过的留绿色勾（已发完）。
    //   一轮结束：游标收掉，已发送标记留着，让用户能看清这一轮覆盖了哪几条。
    void beginSendRun();
    void setSendCursor(int row);
    void endSendRun();
    // 清掉全部发送痕迹（重建表格、用户主动取消时用）。
    void clearSendRun();
    // 某一行当前的发送进度："sending" 正在发 / "sent" 本轮已发 / 空 无标记。
    // 读的是勾选框上的实际属性，与上面几个写入口对称。
    QString sendStateOf(int row) const;

signals:
    void commandsChanged();
    // 不带目标：目标唯一来源是发送区当前的选择（SendPanel::targetMask），
    // 指令本身不存"发去哪个口"。
    void sendCommand(const CommandItem &cmd);
    // rows 与 commands 一一对应且同序：面板按累加延时逐条发时，用行号把进度回报给表格
    // （只带 commands 的话面板不知道该点亮哪一行——行号只有表格自己知道）。
    // loop 为真时发完最后一条回到第一条，直到用户再点一次按钮。
    void batchSendRequested(const QList<int> &rows, const QList<CommandItem> &commands, bool loop);
    // 循环进行中再点按钮：停在当前轮，不再排下一轮。
    void batchSendStopRequested();

private slots:
    void addCommand();
    void removeCommand();
    void onCellChanged(int row, int column);
    void onSendClicked();
    void onCustomContextMenuRequested(const QPoint &pos);

public:
    void refreshColumnWidths();
    // 表格内容铺开所需的最小宽度：各固定列宽之和 + 发送框的可用下限。
    // 上层（CommandPanel）用它决定面板最小宽度——窗口不够宽时宁可撑开窗口，
    // 也不要把末尾的「≡」手柄列和发送按钮裁掉（用户反馈）。
    int minimumContentWidth() const;

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupUi();
    void refreshTable();
    void updateRow(int row, const CommandItem &cmd);
    CommandItem commandFromRow(int row) const;

    // 行操作
    bool isRowChecked(int row) const;
    QList<int> checkedRows() const;
    void copyRows(const QList<int> &rows);
    void removeRows(const QList<int> &rows);
    void moveRow(int from, int to);
    bool confirmRemoval(int count);
    void renameRow(int row);

    // 列宽管理
    void updateSendColumnWidth();
    void updateTailColumnWidth();
    // 折行开着时，列宽一变行高就得重算，否则后半段还停在旧的行高里被裁掉。
    void scheduleContentRowRefit();
    void refitContentRowHeights();
    // 结尾符列的所需宽度：与行内容无关，用一次性探针量（见实现里的说明）。
    int measureTailColumnWidth() const;

    // 几何同步：把 cellWidget 摆进 Qt 算出的 cell 矩形（默认整表；给了区间只同步那几行）。
    // 整表遍历是 O(行×列)，所以**绝不能在逐行填充循环里被触发**——那是 O(n²)；
    // 批量填充期间由 m_geometrySyncSuspended 挂起，结束后统一做一次。
    void syncCellWidgetGeometry(int fromRow = 0, int toRow = -1);
    QWidget *wrapCentered(QWidget *widget);
    // 发送按钮要撑满列宽。勾选框、延时、结尾符仍按自身宽度居中。
    QWidget *wrapFilled(QWidget *widget);

    // 拖拽指示器
    void showDropIndicator(int insertAt);
    void hideDropIndicator();
    void setDragRowDimmed(int row, bool dimmed);

    // 辅助获取控件
    QSpinBox *delaySpinBoxAt(int row) const;
    QComboBox *tailComboBoxAt(int row) const;
    QPushButton *sendButtonAt(int row) const;
    // 只找**勾选列**那个 QCheckBox。Hex 列也是 QCheckBox，按类型泛查会拿错，
    // 所以这里显式限定列——发送进度要染的是"参与批量发送"的那个勾。
    QCheckBox *checkBoxAt(int row) const;
    // 把某一行的发送痕迹画出来（勾选框状态色 + 该行发送按钮的强调底）。
    // 只碰两个 cellWidget 的动态属性，**绝不重建表格**——见"编辑行内控件不得
    // 触发整表重建"那条铁律：进度每 50ms 动一次，重建就等于把卡顿固定成常态。
    void applySendVisual(int row);

    QTableWidget *m_table = nullptr;
    // 只代理「发送框」这一列。具体类型留在 cpp 里：表头不需要知道它怎么画。
    QAbstractItemDelegate *m_contentDelegate = nullptr;
    QTimer *m_rowRefitTimer = nullptr;
    bool m_contentWordWrap = false;
    bool m_refittingRows = false;
    // 重算行高本身会改列宽（竖向滚动条出现/消失），这次重算用的是旧宽度。
    // 记一笔，算完再补一次，避免最后一行仍按旧列宽折行。
    bool m_refitAgain = false;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QCheckBox *m_loopSend = nullptr;
    QPushButton *m_batchSendButton = nullptr;
    QWidget *m_dropIndicator = nullptr;

    CommandSet m_currentSet;
    bool m_updating = false; // 防止递归更新
    bool m_syncingGeometry = false; // 防止递归同步几何
    // 批量填充期间挂起"行高变化 → 全表几何同步"的联动：那是 O(n²) 的根源
    // （600 行一次刷新 20 秒，探针 command_refresh_perf 实测）。
    bool m_geometrySyncSuspended = false;
    // 结尾符下拉的标签集合与所需列宽：refreshTable 开头算一次，updateRow 逐行复用。
    // 它们只依赖字体/DPI/翻译，不依赖行内容。
    QStringList m_tailLabels;
    int m_tailWidthHint = 0;

    // 拖拽状态
    int m_dragRow = -1;
    int m_dropAt = -1;

    // 批量发送进度：正在发送的行（-1 = 无）与本轮已发出的行。
    // 用行号而不是指令 id：游标是"屏幕上哪一行亮着"，而重建后 id 还在、行号会变——
    // 所以 refreshTable 必须清掉这两个状态（见实现）。
    int m_sendCursor = -1;
    QSet<int> m_sentRows;
};

#endif

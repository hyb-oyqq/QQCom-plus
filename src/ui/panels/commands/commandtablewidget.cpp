#include "commandtablewidget.h"
#include "ui/common/combometrics.h"
#include "ui/common/stylehelpers.h"
#include "ui/common/styledialogs.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace {

// 列序。发送框列会 stretch，其它列按内容给固定宽度，整体不出现横向滚动条。
enum Column
{
    CheckColumn = 0,
    NumberColumn = 1,
    ContentColumn = 2,
    HexColumn = 3,
    DelayColumn = 4,
    TailColumn = 5,
    SendColumn = 6,
    HandleColumn = 7,
    ColumnCount = 8
};

// 发送框（stretch）列至少要留出的可用宽度：面板被压到最窄时，
// 这一列还得放得下 "+CR,TURNANGLE=10;" 这类中等长度指令（与 setupUi 里的列宽目标一致）。
constexpr int ContentColumnMinWidth = 188;

// 发送列的宽度由表内最长指令名决定，上下都封顶：下限保证短名称时按钮也不至于挤成
// 一团，上限防止一条超长名称把整个发送框列吃掉（超出的部分仍走 tooltip）。
constexpr int SendColumnMinWidth = 100;
constexpr int SendColumnMaxWidth = 200;  // 增加到 200px，显示更多按钮文字

// 结尾符列不写死宽度：按 combo 的 sizeHint 量（见 updateTailColumnWidth）。
// 这个下限只在空表探针也量不出时兜底，确保最长的 "+\\r\\n" 在任何字体下都完整可见。
constexpr int TailColumnMinWidth = 80;

// 拖拽落点指示线：一条贴在视口上的高亮横线。WA_TransparentForMouseEvents 保证它
// 不抢走拖拽用的鼠标事件，WA_NoSystemBackground 省掉一次无谓的背景擦除。
class DropIndicator : public QWidget
{
public:
    explicit DropIndicator(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(0x1D, 0x4E, 0xD8));
    }
};

// 批量填充表格期间统一持有 m_updating 的 RAII 守卫。
// 旧实现让 updateRow() 自己 m_updating = true/false，结果 refreshTable() 填完第一行后
// 保护就被打回 false，后续行的 setItem() 会 emit cellChanged → commandsChanged →
// 面板回写模型。改由调用方持有，保证整段填充期间保护一直有效，并支持嵌套调用。
class UpdateGuard
{
public:
    explicit UpdateGuard(bool &flag) : m_flag(flag), m_oldValue(flag) { m_flag = true; }
    ~UpdateGuard() { m_flag = m_oldValue; }

    UpdateGuard(const UpdateGuard &) = delete;
    UpdateGuard &operator=(const UpdateGuard &) = delete;

private:
    bool &m_flag;
    bool m_oldValue;
};

// 结尾符下拉的显示名。顺序与 CommandTail 的取值（0/1/2/3）一一对应，
// 所以存/取都用 currentIndex，不依赖翻译后的文本。
QStringList tailLabels()
{
    return {QObject::tr("无"), QObject::tr("+\\r\\n"), QObject::tr("+\\r"), QObject::tr("+\\n")};
}

} // namespace

CommandTableWidget::CommandTableWidget(QWidget *parent) : QWidget(parent)
{
    setupUi();
}

void CommandTableWidget::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // 表格
    m_table = new QTableWidget(this);
    m_table->setColumnCount(ColumnCount);
    m_table->setHorizontalHeaderLabels({tr("☑"), tr("No."), tr("发送框"),
                                        tr("Hex"), tr("延时"), tr("结尾符"),
                                        tr("发送"), tr("≡")});

    // 列宽照 JCom 的界面参考分配：辅助列一律压到刚够用，省下的空间全给发送框
    // （stretch）。除发送框外的固定列合计 322px，实测 520px 面板下发送框 188px，
    // 放得下 "+CR,TURNANGLE=10;" 这类长指令；面板拖宽时增量也全进发送框。
    // 注：辅助列比 JCom 参考值再各收 2px 左右，因为表格自身还有 1px 边框要吃掉
    // 视口宽度——不收的话发送框只有 172px，达不到 ≥175px 的目标。
    m_table->setColumnWidth(CheckColumn, 30);
    m_table->setColumnWidth(NumberColumn, 34);
    m_table->setColumnWidth(ContentColumn, ContentColumnMinWidth);
    m_table->setColumnWidth(HexColumn, 38);
    m_table->setColumnWidth(DelayColumn, 52);
    // 结尾符列宽不写死：setColumnWidth 先给下限占位，refreshTable/refreshColumnWidths
    // 会按 combo 的 sizeHint 量出真实需要值（字体/DPI/翻译变化时自动跟随）。
    m_table->setColumnWidth(TailColumn, TailColumnMinWidth);
    m_table->setColumnWidth(SendColumn, SendColumnMinWidth);
    m_table->setColumnWidth(HandleColumn, 26);

    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(ContentColumn, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    // 行高自适应：行内放的是真控件（勾选框 / SpinBox / ComboBox / 带 padding 的发送按钮），
    // 它们的 sizeHint 比默认行高（30px）高，QSS 又给控件加了 padding + border，于是控件会
    // 溢出行的上下边界（用户截图报告）。ResizeToContents 让每行按最高的那个 cellWidget 撑开
    // ——字体 / DPI / 翻译变化时自动跟随，不写死行高。用例 `commandRowsFitTheirWidgets` 守住。
    // minimumSectionSize 只兜底：无控件行（理论上不会出现）也别被压得比勾选框还矮。
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setMinimumSectionSize(24);
    m_table->setSelectionBehavior(QTableWidget::SelectRows);
    m_table->setEditTriggers(QTableWidget::DoubleClicked | QTableWidget::EditKeyPressed);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    // 手柄列的按下/移动/释放要在视口上拦：QTableWidget 自带的 InternalMove 只搬 item，
    // Hex/延时/结尾符/发送这些 cellWidget 会被留在原行，越拖越乱。
    m_table->viewport()->installEventFilter(this);
    m_dropIndicator = new DropIndicator(m_table->viewport());

    // 行高/列宽一变，Qt 重新算完行区间后并不会把 cellWidget 摆进 cell 矩形（见
    // syncCellWidgetGeometry 的病根说明），所以要在表格请求重排布局之后统一把几何刷回去。
    // 接两个 header 的 sectionResized 而不是自己猜"什么时候几何变了"：
    //   纵向 —— ResizeToContents 算出新行高（增删行、字号/DPI 变化都走这里）
    //   横向 —— updateSendColumnWidth / updateTailColumnWidth 改列宽
    // 两者都是 Qt 自己发的"这块几何变了"通知，是唯一事实来源。
    // **只同步那一段**（信号带 first/last）：纵向一次性 resize 所有行时 last-first 可能很大，
    // 但这时只同步真正受影响的行段，远比整表遍历便宜；批量填充期间由挂起标志整个跳过。
    connect(m_table->verticalHeader(), &QHeaderView::sectionResized, this,
            [this](int first, int last, int) { syncCellWidgetGeometry(first, last); });
    connect(m_table->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this](int, int, int) { syncCellWidgetGeometry(); });

    connect(m_table, &QTableWidget::cellChanged, this, &CommandTableWidget::onCellChanged);
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &CommandTableWidget::onCustomContextMenuRequested);

    layout->addWidget(m_table);

    // 底部按钮（单行铺开：左侧操作类，右侧主操作类）
    auto *buttonLayout = new QHBoxLayout();

    m_addButton = new QPushButton(tr("添加指令"), this);
    m_addButton->setObjectName("addCommand");
    m_removeButton = new QPushButton(tr("删除勾选"), this);
    m_removeButton->setObjectName("removeChecked");
    m_removeButton->setToolTip(tr("删除所有已勾选的指令，删除前会先确认"));
    m_batchSendButton = new QPushButton(tr("按序发送勾选项"), this);
    // objectName 保持 primaryButton：QSS 按它画主操作按钮的强调底，改名字会静默丢样式。
    // （测试要定位它时也用这个名字，别再另起一个。）
    m_batchSendButton->setObjectName("primaryButton");
    m_batchSendButton->setToolTip(tr("按列表顺序依次发送所有勾选的指令，每条按其延时等待后发出"));

    connect(m_addButton, &QPushButton::clicked, this, &CommandTableWidget::addCommand);
    connect(m_removeButton, &QPushButton::clicked, this, &CommandTableWidget::removeCommand);
    connect(m_batchSendButton, &QPushButton::clicked, this, [this] {
        // 勾选行号与指令一起交给面板：面板负责时序，表格负责"点亮哪一行"，
        // 而行号只有这里知道（见 batchSendRequested 的说明）。
        const QList<int> rows = checkedRows();
        QList<CommandItem> commands;
        commands.reserve(rows.size());
        for (int row : rows) commands.append(commandFromRow(row));
        if (!commands.isEmpty()) {
            emit batchSendRequested(rows, commands);
        }
    });

    buttonLayout->addWidget(m_addButton);
    buttonLayout->addWidget(m_removeButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_batchSendButton);

    layout->addLayout(buttonLayout);
}

void CommandTableWidget::setCommandSet(const CommandSet &set)
{
    m_currentSet = set;
    refreshTable();
}

CommandSet CommandTableWidget::commandSet() const
{
    // 从表格收集当前状态
    CommandSet set = m_currentSet;
    set.commands.clear();

    for (int row = 0; row < m_table->rowCount(); ++row) {
        set.commands.append(commandFromRow(row));
    }

    return set;
}

QList<CommandItem> CommandTableWidget::enabledCommands() const
{
    QList<CommandItem> commands;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        CommandItem cmd = commandFromRow(row);
        if (cmd.enabled) {
            commands.append(cmd);
        }
    }
    return commands;
}

int CommandTableWidget::rowCount() const
{
    return m_table ? m_table->rowCount() : 0;
}

// 勾选列现在是一个 QCheckBox 控件（QSS 覆盖得到），取勾选状态要走控件而不是 item。
bool CommandTableWidget::isRowChecked(int row) const
{
    if (!m_table || row < 0 || row >= m_table->rowCount()) return false;
    if (auto *widget = m_table->cellWidget(row, CheckColumn)) {
        if (auto *checkBox = widget->findChild<QCheckBox *>()) {
            return checkBox->isChecked();
        }
    }
    return false;
}

QList<int> CommandTableWidget::checkedRows() const
{
    QList<int> rows;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (isRowChecked(row)) rows.append(row);
    }
    return rows;
}

void CommandTableWidget::refreshTable()
{
    // 守卫覆盖整段填充：期间 setItem/setCellWidget 产生的 cellChanged 一律不回写模型
    const UpdateGuard guard(m_updating);
    // 逐行建 cellWidget 会连续改动行高 → 触发多轮重排/重绘。整段填充期间关掉更新，
    // 只在最后一次性刷新；否则每次 refreshTable 都要走 N 轮布局（大表格下就是卡顿来源）。
    const QSignalBlocker headerBlocker(m_table->horizontalHeader());
    m_table->setUpdatesEnabled(false);
    // 逐行填充期间挂起几何同步：否则每行 setCellWidget → 行高变化 → sectionResized →
    // 全表遍历一遍，整体退化成 O(n²)（见 syncCellWidgetGeometry 的说明）。
    // 填充结束后下面自己调一次完整同步，结果一样。
    const QScopedValueRollback<bool> geometryGuard(m_geometrySyncSuspended, true);

    // 先清空旧内容再重建，而不是直接在原表上覆写。
    //
    // 两个必须同时满足的约束，前两版各踩了一边：
    //
    // ① **不能把旧控件留在表格的控件树里等 deleteLater**（这是 `clearContents()` /
    //    直接 `setCellWidget` 覆写的默认行为，QAbstractItemView 就是这么排的）。
    //    刷新是同步路径，不返回事件循环 → 延迟删除永远排不上队 → 控件逐轮累积在
    //    视口下，下一轮还要额外背上一整屏旧控件的代价。探针 command_refresh_perf
    //    实测 300 行连刷从 330ms 一路涨到 5.7 秒，正是用户报的"列表越用越卡"。
    //
    // ② **也不能当场 delete**。重建往往发生在"行内控件自己的信号"里：用户点选
    //    结尾符下拉框 → activated → commandsChanged → 落库；拖拽手柄 → 鼠标释放
    //    → moveRow → 重建。此时事件链上那个控件（下拉框本体 / 手柄的宿主容器）
    //    正处在自己的事件或信号派发中，立即析构它 = use-after-free，
    //    下一句回到它内部就崩——用户报的"选换行符崩""拖动崩"都是这个。
    //
    // 两者兼顾的做法：**立刻把它从控件树里摘出去**（解掉①），**把析构留给 Qt**
    // （解掉②）。`removeCellWidget()` 内部已经为旧控件排好了 deleteLater，我们
    // 只负责让它同时离开父子树 —— 树的规模因此不再随刷新轮数增长，而真正的
    // 析构发生在本轮事件派发退栈之后，那时代码已经不在被销毁对象的栈帧里。
    // 先 hide() 再脱父：setParent(nullptr) 会让控件变成顶层窗口，不先隐藏会闪窗。
    for (int row = 0; row < m_table->rowCount(); ++row) {
        for (int col = 0; col < m_table->columnCount(); ++col) {
            QWidget *stale = m_table->cellWidget(row, col);
            if (!stale) continue;
            stale->hide();                        // 先藏：脱父后它就是顶层窗口
            m_table->removeCellWidget(row, col);  // 摘索引 + 内部排 deleteLater
            stale->setParent(nullptr);            // 立刻脱离控件树：这一句才治得住累积
        }
    }
    m_table->clearContents();
    m_table->setRowCount(0);
    m_table->setRowCount(m_currentSet.commands.size());

    // 行号被重新排过了，上一轮的发送痕迹跟着作废——不清的话，高亮会落在
    // "碰巧同号"的另一条指令上，比没有高亮更误导。注意这里**不能**调
    // clearSendRun()：它要按旧行号擦控件，而此刻控件已经全被换掉了，
    // 直接丢状态即可（新控件本来就没有痕迹）。
    m_sendCursor = -1;
    m_sentRows.clear();

    // 结尾符列需要的宽度与行内容无关（每行的 combo 项完全一样），量一次就够——
    // 旧实现每行都调 sizeHint()，等于把同一个答案重算 N 遍（行数越多越慢）。
    m_tailLabels = tailLabels();
    m_tailWidthHint = measureTailColumnWidth();

    for (int i = 0; i < m_currentSet.commands.size(); ++i) {
        updateRow(i, m_currentSet.commands.at(i));
    }

    m_table->setUpdatesEnabled(true);
    // 行建完才知道最长指令名有多宽：发送列按它自适应，避免长名称被截断
    updateSendColumnWidth();
    // 结尾符列同样按内容量——否则 "+\\r\\n" 会被裁成 "+\\"（用户实测）
    updateTailColumnWidth();
    // 行数/行高/列宽都定下来了，此时才拿到最终 visualRect，统一重排一次 cellWidget。
    // 必须在这里做——上面几步每改一步 Qt 都会重算行高，早跑等于按旧的行高摆控件。
    syncCellWidgetGeometry();
    m_table->viewport()->update();
}

int CommandTableWidget::minimumContentWidth() const
{
    if (!m_table) return 0;
    // 固定列按当前列宽累加，发送框列按它的**下限**算（发送列是自适应的，不该因为它
    // 当前较宽就把面板最小宽度撑大），末尾再加表格边框的 1~2px 余量。
    int total = 0;
    for (int col = 0; col < ColumnCount; ++col) {
        if (col == ContentColumn) continue; // stretch 列：面板有多宽它占多宽
        total += col == SendColumn ? SendColumnMinWidth : m_table->columnWidth(col);
    }
    total += ContentColumnMinWidth; // 发送框（stretch）必须留出的可用宽度
    return total + m_table->frameWidth() * 2 + 2;
}

void CommandTableWidget::refreshColumnWidths()
{
    // 内容列是 stretch：面板变宽时 Qt 自己会把增量给它，不必插手。
    // 发送列是固定宽（按最长指令名自适应），宽度变化后要重量一次，让封顶值跟着变。
    updateSendColumnWidth();
    updateTailColumnWidth();
    // 列宽变了 → 每列的 visualRect 变了 → 单元格里的控件要跟着重新居中
    syncCellWidgetGeometry();
}

QWidget *CommandTableWidget::wrapCentered(QWidget *content)
{
    // 移除了 QSS 的 padding 后，cellWidget 能正确定位在 cell 中心。
    // 这里只需要一个简单的容器，让控件在其中居中即可。
    auto *host = new QWidget;
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    // **必须显式 reparent 到宿主**：QWidget 加进 layout 时不会改变它的 parent，
    // 而所有内容控件建的时候 parent 是本表格（为了走样式表），于是宿主被销毁时
    // 它们并不随之释放 —— 每次 refreshTable 都漏掉整整一屏控件，刷新一轮比一轮慢
    // （探针实测 300 行单次刷新后子控件数 23 → 6323，多出来的就是漏掉的旧控件）。
    content->setParent(host);
    layout->addWidget(content, 0, Qt::AlignCenter);
    return host;
}

void CommandTableWidget::syncCellWidgetGeometry(int fromRow, int toRow)
{
    if (!m_table || m_syncingGeometry) return;

    // 频繁重排期间（refreshTable 的逐行填充）关掉这项同步：每次 setCellWidget 都会让行高
    // 重算 → 触发 sectionResized → 这里再遍历全部行 × 全部列。在填充循环里逐行触发就是
    // O(n²)，600 行时一次刷新 20 秒（探针 command_refresh_perf 实测）。
    // 填充结束后 refreshTable 会自己调一次完整同步，几何一样是对的。
    if (m_geometrySyncSuspended) return;

    // 重排期间 setGeometry 本身可能再触发 header 的 sectionResized
    // （改列宽的行高重算），加个自锁避免递归重入。
    const QScopedValueRollback<bool> guard(m_syncingGeometry, true);

    // 默认整表同步；给了区间就只同步那几行——刷新时"变动的行"往往是已知的一小段。
    const int rows = m_table->rowCount();
    const int first = qBound(0, fromRow, rows);
    const int last = qBound(first, toRow < 0 ? rows : toRow + 1, rows);
    for (int row = first; row < last; ++row) {
        for (int col = 0; col < m_table->columnCount(); ++col) {
            QWidget *widget = m_table->cellWidget(row, col);
            if (!widget) continue; // No. / 发送框 是 item，没有 cellWidget

            // 几何一律取 Qt 自己算出来的 visualRect —— 不写死任何坐标或尺寸。
            // 移除了 QSS 的 padding 后，cellWidget 能正确居中。
            const QRect cellRect = m_table->visualRect(m_table->model()->index(row, col));
            if (widget->geometry() != cellRect) {
                widget->setGeometry(cellRect);
            }
        }
    }
}

void CommandTableWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    syncCellWidgetGeometry();
}

QSpinBox *CommandTableWidget::delaySpinBoxAt(int row) const
{
    if (!m_table || row < 0 || row >= m_table->rowCount()) return nullptr;
    QWidget *host = m_table->cellWidget(row, DelayColumn);
    return host ? host->findChild<QSpinBox *>() : nullptr;
}

QComboBox *CommandTableWidget::tailComboBoxAt(int row) const
{
    if (!m_table || row < 0 || row >= m_table->rowCount()) return nullptr;
    QWidget *host = m_table->cellWidget(row, TailColumn);
    return host ? host->findChild<QComboBox *>() : nullptr;
}

QPushButton *CommandTableWidget::sendButtonAt(int row) const
{
    if (!m_table || row < 0 || row >= m_table->rowCount()) return nullptr;
    QWidget *host = m_table->cellWidget(row, SendColumn);
    return host ? host->findChild<QPushButton *>() : nullptr;
}

QCheckBox *CommandTableWidget::checkBoxAt(int row) const
{
    if (!m_table || row < 0 || row >= m_table->rowCount()) return nullptr;
    QWidget *host = m_table->cellWidget(row, CheckColumn);
    return host ? host->findChild<QCheckBox *>() : nullptr;
}

void CommandTableWidget::beginSendRun()
{
    clearSendRun();
}

void CommandTableWidget::setSendCursor(int row)
{
    const int previous = m_sendCursor;
    if (previous == row) return;

    // 上一次的游标位置就此成为"已发送"：用户据此看出这一轮已经走过哪几条。
    if (previous >= 0) m_sentRows.insert(previous);
    m_sendCursor = row;

    // 只重画受影响的两行（旧游标 + 新游标）。整表扫一遍是 O(n)，而这个函数
    // 每发一条就被调一次——批量发 100 条就是 100 次全表扫描，白白拖慢发送本身。
    if (previous >= 0) applySendVisual(previous);
    if (row >= 0) applySendVisual(row);
}

void CommandTableWidget::endSendRun()
{
    const int previous = m_sendCursor;
    m_sendCursor = -1;
    if (previous >= 0) {
        m_sentRows.insert(previous);
        applySendVisual(previous);
    }
}

void CommandTableWidget::clearSendRun()
{
    // **先清状态、再刷视觉**，顺序不能反：applySendVisual 的状态是读 m_sendCursor /
    // m_sentRows 现算出来的，若先刷后清，它会把旧状态原样再染一遍，等于白擦
    // （控件上的属性反而被"确认"了一次）。
    const QSet<int> touched = m_sentRows;
    const int cursor = m_sendCursor;
    m_sentRows.clear();
    m_sendCursor = -1;

    for (int row : touched) applySendVisual(row);
    if (cursor >= 0) applySendVisual(cursor);
}

void CommandTableWidget::applySendVisual(int row)
{
    if (!m_table || row < 0 || row >= m_table->rowCount()) return;

    // 两个状态写在动态属性上由 QSS 上色，而不是在这里手写颜色：
    // 浅色/深色主题各有一份 QSS，把颜色写在代码里会让深色主题永远差一截。
    // **不碰行背景**：行背景是拖拽灰显的地盘，两套状态抢同一块底色必然互相擦掉。
    const QVariant state = row == m_sendCursor
        ? QVariant(QStringLiteral("sending"))
        : (m_sentRows.contains(row) ? QVariant(QStringLiteral("sent")) : QVariant());
    setStyledProperty(checkBoxAt(row), "sendState", state);
    setStyledProperty(sendButtonAt(row), "sending",
        row == m_sendCursor ? QVariant(true) : QVariant());
}

QString CommandTableWidget::sendStateOf(int row) const
{
    // 与 setSendCursor/endSendRun 对称的读接口：值就是从勾选框上读回来的实际状态
    // （而不是另记一份成员），所以它证明的是"界面真的被染上了色"，不是"我们打算染"。
    auto *box = checkBoxAt(row);
    return box ? box->property("sendState").toString() : QString();
}

void CommandTableWidget::updateSendColumnWidth()
{
    if (!m_table) return;

    int widest = 0;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (auto *button = sendButtonAt(row)) {
            widest = qMax(widest, button->sizeHint().width());
        }
    }
    // 空表时回落到下限，别把列压没
    const int wanted = widest > 0 ? widest + 12 : SendColumnMinWidth;
    const int width = qBound(SendColumnMinWidth, wanted, SendColumnMaxWidth);
    if (m_table->columnWidth(SendColumn) != width) {
        m_table->setColumnWidth(SendColumn, width);
    }
}

int CommandTableWidget::measureTailColumnWidth() const
{
    // 结尾符下拉项的文本会随字体/DPI/翻译变化，写死列宽必然在某个环境下把
    // "+\\r\\n" 裁成 "+\\"（用户实测报告）。改成按 combo 自己的 sizeHint 量：
    // fitComboToItems 已让它把箭头（画在 24px 右内边距里）也算进去。
    // 所有行的下拉框项完全相同，所以用一次性探针量即可——**不去遍历每一行**：
    // 逐行取 sizeHint() 在多行下是纯浪费（每行答案都一样），也是表格刷新卡顿的来源之一。
    QComboBox probe;
    probe.addItems(tailLabels());
    fitComboToItems(&probe);
    return qMax(TailColumnMinWidth, probe.sizeHint().width() + 6);
}

void CommandTableWidget::updateTailColumnWidth()
{
    if (!m_table) return;

    // 宽度已在 refreshTable 里量过一次（m_tailWidthHint）；这里只在没量过时补量，
    // 避免每次 resize / 列宽刷新都重新构造探针 combo。
    if (m_tailWidthHint <= 0) m_tailWidthHint = measureTailColumnWidth();
    const int width = m_tailWidthHint;
    if (m_table->columnWidth(TailColumn) != width) {
        m_table->setColumnWidth(TailColumn, width);
    }
}

void CommandTableWidget::updateRow(int row, const CommandItem &cmd)
{
    // 注意：这里不再自己复位 m_updating，由调用方通过 UpdateGuard 统一管理

    // 凡是放进 cell 的控件一律走 wrapCentered()：宿主（cellWidget）铺满 cell 矩形，
    // 控件在宿主里按布局居中。六个 cellWidget（☑/Hex/延时/结尾符/发送/≡）统一走这一条路，
    // 不给任何一个控件写死边距——字号、DPI、QSS padding、翻译变化时自动跟随。
    // 见 syncCellWidgetGeometry() 的病根说明：不铺满 cell 矩形，任何 AlignCenter 都是假的。

    // 列0: 勾选框。用真正的 QCheckBox 控件而不是 QTableWidgetItem 的 checkState——
    // 后者的指示器由 QStyle 自绘，QSS 的 QCheckBox::indicator 规则完全覆盖不到，
    // 用户看到的只是一个没有勾的小方块。控件化之后样式统一、也能拿到 16px 的勾。
    auto *checkBox = new QCheckBox(this);
    checkBox->setChecked(cmd.enabled);
    checkBox->setToolTip(tr("勾选后参与「按序发送勾选项」批量发送"));
    connect(checkBox, &QCheckBox::toggled, this, [this] {
        if (!m_updating) emit commandsChanged();
    });
    m_table->setCellWidget(row, CheckColumn, wrapCentered(checkBox));

    // 列1: No.
    auto *noItem = new QTableWidgetItem(QString::number(row));
    noItem->setFlags(noItem->flags() & ~Qt::ItemIsEditable);
    noItem->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, NumberColumn, noItem);

    // 列2: 发送框（可编辑）
    auto *contentItem = new QTableWidgetItem(cmd.content);
    m_table->setItem(row, ContentColumn, contentItem);

    // 列3: Hex（复选框控件）
    auto *hexCheckBox = new QCheckBox(this);
    hexCheckBox->setChecked(cmd.isHex);
    connect(hexCheckBox, &QCheckBox::toggled, this, [this] {
        if (!m_updating) emit commandsChanged();
    });
    m_table->setCellWidget(row, HexColumn, wrapCentered(hexCheckBox));

    // 列4: 延时（SpinBox）
    auto *delaySpinBox = new QSpinBox(this);
    delaySpinBox->setRange(0, 10000);
    // 列宽只有 52px：带 " ms" 后缀时连 "50" 都会被裁成 "5C"（用户实测反馈），所以
    // 后缀与上下箭头都去掉，整列留给五位数字，单位放进 tooltip。滚轮和键盘仍可调整。
    delaySpinBox->setButtonSymbols(QAbstractSpinBox::NoButtons);
    delaySpinBox->setAlignment(Qt::AlignCenter);
    delaySpinBox->setToolTip(tr("发送前延时（毫秒）"));
    delaySpinBox->setValue(cmd.delayMs);
    delaySpinBox->setMinimumWidth(40);
    connect(delaySpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
        if (!m_updating) emit commandsChanged();
    });
    m_table->setCellWidget(row, DelayColumn, wrapCentered(delaySpinBox));

    // 列5: 结尾符（ComboBox）
    // 所有行的下拉项完全一样，但**不能共用一个 QStandardItemModel**：item 带 parent，
    // 同一个 item 出现在多个 combo 里，选中标记会在行间串位。这里改为把标签集合
    // 预存在成员里（refreshTable 开头算一次），逐行仍建自己的 model —— 建 model 是
    // 每行一次的 O(4)，真正的开销在重复调用 tailLabels()（内含 tr()）与重复量列宽。
    auto *tailComboBox = new QComboBox(this);
    tailComboBox->addItems(m_tailLabels);
    // 将字符串转换为下拉框索引
    int tailIndex = 0;
    if (cmd.tail == "CR") tailIndex = 1;
    else if (cmd.tail == "LF") tailIndex = 2;
    else if (cmd.tail == "CR+LF") tailIndex = 3;
    tailComboBox->setCurrentIndex(tailIndex);
    // 按项目统一约定让样式表自己算宽（箭头画在 combo 的右侧内边距里，只按文本量会偏窄）。
    // 之前它没有走这一步，列宽只有 62px 时 "+\\r\\n" 这类文本把可视区挤到几乎为零，
    // 用户看起来就像"点不中、选不了"。
    fitComboToItems(tailComboBox);
    tailComboBox->setToolTip(tr("发送时附加的结尾符"));
    // 性能优化：使用 activated 信号而不是 currentIndexChanged
    // activated 只在用户主动选择时触发，不会在程序设置 setCurrentIndex 时触发
    // 减少不必要的信号处理，提升响应速度
    connect(tailComboBox, QOverload<int>::of(&QComboBox::activated), this, [this] {
        if (!m_updating) emit commandsChanged();
    });
    m_table->setCellWidget(row, TailColumn, wrapCentered(tailComboBox));

    // 列6: 发送按钮（文本即指令名）
    auto *sendButton = new QPushButton(cmd.description.isEmpty() ? tr("发送") : cmd.description, this);
    // 描述另存一份到属性：空名称时按钮显示占位"发送"，直接读按钮文本会把占位当成名字
    sendButton->setProperty("description", cmd.description);
    sendButton->setProperty("row", row);
    // 发送列只有 92px，长名称必然被截断：完整名字进 tooltip，不为它加宽列
    sendButton->setToolTip(cmd.description.isEmpty()
                               ? tr("左键发送，右键重命名")
                               : cmd.description + QLatin1Char('\n') + tr("左键发送，右键重命名"));
    connect(sendButton, &QPushButton::clicked, this, &CommandTableWidget::onSendClicked);
    m_table->setCellWidget(row, SendColumn, wrapCentered(sendButton));

    // 列7: 拖动手柄。做成撑满行高的把手（与行内按钮等高、带浅底），让人一眼看出这里
    // 可以按着拖——之前只是 22px 里一个纯文本 "≡"，用户反馈"太小、不像能拖"。
    auto *handle = new QLabel(tr("≡"), this);
    handle->setAlignment(Qt::AlignCenter);
    handle->setToolTip(tr("按住上下拖动可调整顺序"));
    handle->setStyleSheet(QStringLiteral("QLabel { color: #8a94a6; background: #f1f5f9; }"));
    // 让鼠标事件直接穿透到视口：拖拽是在 viewport 的 eventFilter 里统一拦的，
    // 手柄控件自己吃掉事件的话就再也拖不动了。
    handle->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_table->setCellWidget(row, HandleColumn, handle);
}

// 辅助函数：将下拉框索引转换为结尾符字符串
static QString commandTailFromInt(int index)
{
    switch (index) {
        case 0: return "无";
        case 1: return "CR";
        case 2: return "LF";
        case 3: return "CR+LF";
        default: return "无";
    }
}

CommandItem CommandTableWidget::commandFromRow(int row) const
{
    if (row < 0 || row >= m_table->rowCount()) return CommandItem();

    CommandItem cmd;
    cmd.id = m_currentSet.commands.value(row).id; // 行内无法编辑 id，沿用模型里的

    // 列0: 勾选框（控件化）
    cmd.enabled = isRowChecked(row);

    // 列2: 内容
    if (m_table->item(row, ContentColumn)) {
        cmd.content = m_table->item(row, ContentColumn)->text();
    }

    // 列3: Hex
    if (m_table->cellWidget(row, HexColumn)) {
        auto *hexWidget = m_table->cellWidget(row, HexColumn);
        auto *hexCheckBox = hexWidget->findChild<QCheckBox*>();
        if (hexCheckBox) {
            cmd.isHex = hexCheckBox->isChecked();
        }
    }

    // 列4: 延时
    if (auto *delaySpinBox = delaySpinBoxAt(row)) {
        cmd.delayMs = delaySpinBox->value();
    }

    // 列5: 结尾符
    if (auto *tailComboBox = tailComboBoxAt(row)) {
        cmd.tail = commandTailFromInt(tailComboBox->currentIndex());
    }

    // 列6: 描述（优先取按钮上的属性，取不到再退回按钮文本）
    if (auto *sendButton = sendButtonAt(row)) {
        cmd.description = sendButton->property("description").toString();
        if (cmd.description.isEmpty()) {
            cmd.description = sendButton->text();
        }
    }

    return cmd;
}

void CommandTableWidget::addCommand()
{
    if (!m_table) return;

    CommandItem newCmd;
    newCmd.content = tr("+CR,TEST;");
    newCmd.description = tr("测试指令");

    m_currentSet.commands.append(newCmd);

    {
        const UpdateGuard guard(m_updating);
        int row = m_table->rowCount();
        m_table->insertRow(row);
        updateRow(row, newCmd);
    }

    emit commandsChanged();
}

void CommandTableWidget::copyRows(const QList<int> &rows)
{
    if (!m_table || rows.isEmpty()) return;

    // 先按当前控件快照出副本：refreshTable 之后原行控件就被换掉了
    QList<CommandItem> copies;
    copies.reserve(rows.size());
    for (int row : rows) {
        if (row < 0 || row >= m_table->rowCount()) continue;
        CommandItem copy = commandFromRow(row);
        copy.id = QUuid::createUuid(); // 副本是新指令，绝不共用原 id
        copies.append(copy);
    }
    if (copies.isEmpty()) return;

    // 从后往前插：前面的插入不会挤掉后面行的下标，多份副本的相对顺序也保持原样
    for (int i = copies.size() - 1; i >= 0; --i) {
        const int at = qMin(rows.at(i) + 1, m_currentSet.commands.size());
        m_currentSet.commands.insert(at, copies.at(i));
    }

    {
        const UpdateGuard guard(m_updating);
        refreshTable();
    }

    emit commandsChanged();
}

bool CommandTableWidget::confirmRemoval(int count)
{
    // 删除是破坏性且不可撤销的（快照栈没有对外入口），必须先问一次。
    // 文案给出条数而不是"确定吗"：用户常常勾了好几行才想起来点，条数是他
    // 确认"我勾的确实是这些"的唯一依据。
    return StyleDialogs::question(this, tr("删除确认"),
               tr("确定要删除勾选的 %1 条指令吗？此操作不可撤销。").arg(count));
}

void CommandTableWidget::removeCommand()
{
    const QList<int> rows = checkedRows();
    if (rows.isEmpty()) return; // 没勾任何行就没什么可问的，直接不动作
    if (!confirmRemoval(rows.size())) return;
    removeRows(rows);
}

void CommandTableWidget::removeRows(const QList<int> &rows)
{
    if (!m_table || rows.isEmpty()) return;

    // 先定出待删行号的去重有序集合，再在模型上一次成型，最后整体重建表格。
    //
    // 旧写法是"从后往前逐个 removeRow + removeAt"：每次 removeRow 都要让
    // QTableWidget 搬动其后所有行的 item 与 cellWidget（O(n)），因此删 k 行是
    // O(k·n)，勾选一半删一半就退化成 O(n²)——实测 4000 行删 2000 行要 15 秒（卡死）。
    // 模型先行 + 一次 refreshTable 把它压回 O(n)（moveRow 早就用的同一套路）。
    QList<int> ordered;
    ordered.reserve(rows.size());
    for (int row : rows) {
        if (row >= 0 && row < m_currentSet.commands.size()) ordered.append(row);
    }
    if (ordered.isEmpty()) return;
    std::sort(ordered.begin(), ordered.end());
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());

    // 从后往前摘除（QList::removeAt 自身的搬移是小的 O(n) 单次，总共 O(k·n) 但只搬
    // 指针不碰控件——真正贵的是 QTableWidget::removeRow 的 item/cellWidget 搬移与
    // 布局重算，那部分被下面的整体 refreshTable 一次做完）。
    for (int i = ordered.size() - 1; i >= 0; --i) {
        m_currentSet.commands.removeAt(ordered.at(i));
    }

    {
        const UpdateGuard guard(m_updating);
        refreshTable();
    }

    emit commandsChanged();
}

void CommandTableWidget::moveRow(int from, int to)
{
    const int total = m_currentSet.commands.size();
    if (from < 0 || from >= total || to < 0 || to >= total || from == to) return;

    // 先搬模型，再整体重建表格。
    // QTableWidget 的自有 InternalMove 只搬 item，Hex/延时/结尾符/发送按钮这些
    // cellWidget 会留在原行；逐个摘挂控件又要踩"setCellWidget 会销毁旧控件"的所有权
    // 坑。全表重建由 UpdateGuard 兜住不会引发 commandsChanged 风暴，而且 No. 列、
    // 发送按钮的 row 属性、commands 顺序都从同一份数据重建，天然一致。
    m_currentSet.commands.move(from, to);

    {
        const UpdateGuard guard(m_updating);
        refreshTable();
    }

    emit commandsChanged();
}

bool CommandTableWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != m_table->viewport()) {
        return QWidget::eventFilter(watched, event);
    }

    // 落点换算成"插入到第几行之前"：命中行的上半 → 插到它前面，下半 → 插到它后面；
    // 拖到表格空白处按末尾处理。插入语义比"目标行号"直观，指示线也是按它画的。
    const auto insertAtFor = [this](const QPointF &pos) {
        const int rows = m_table->rowCount();
        const QModelIndex index = m_table->indexAt(pos.toPoint());
        if (!index.isValid()) return rows;
        const int row = index.row();
        const int top = m_table->rowViewportPosition(row);
        const bool after = pos.y() > top + m_table->rowHeight(row) / 2;
        return after ? row + 1 : row;
    };

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton) break;
        const QModelIndex index = m_table->indexAt(mouse->position().toPoint());
        if (!index.isValid() || index.column() != HandleColumn) break;
        // 吃掉按下：手柄不参与选中/编辑，只用于拖拽
        m_dragRow = index.row();
        m_table->selectRow(m_dragRow);
        setDragRowDimmed(m_dragRow, true);
        showDropIndicator(m_dragRow);
        m_table->viewport()->setCursor(Qt::ClosedHandCursor);
        return true;
    }
    case QEvent::MouseMove: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (m_dragRow < 0) break;
        // 只在落点真正变化时重画指示线，不是每个 mouseMove 都动一次几何
        const int insertAt = insertAtFor(mouse->position());
        if (insertAt != m_dropAt) showDropIndicator(insertAt);
        return true;
    }
    case QEvent::MouseButtonRelease: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (m_dragRow < 0) break;
        const int from = m_dragRow;
        const int insertAt = insertAtFor(mouse->position());
        m_dragRow = -1;
        hideDropIndicator();
        m_table->viewport()->unsetCursor();

        // 插入位置 → moveRow 的目标下标：QList::move(from,to) 的 to 是移动后的最终位置
        const int to = insertAt > from ? insertAt - 1 : insertAt;
        const int rows = m_table->rowCount();
        if (to >= 0 && to < rows && to != from) {
            // **不在这个事件派发里重建表格。** 此刻正在派发的是"手柄宿主控件冒泡
            // 上来的鼠标释放"——整条链上那个宿主容器还是活的对象，moveRow 里的
            // refreshTable 会把整行控件（含它）换掉。虽然新实现已把析构推迟到
            // 事件循环，但把它推到本轮派发结束之后再执行更干净：
            // 重建时机与"哪个控件正在处理事件"彻底解耦，不依赖销毁时机的巧合。
            QTimer::singleShot(0, this, [this, from, to] { moveRow(from, to); });
        } else {
            setDragRowDimmed(from, false);
        }
        return true;
    }
    case QEvent::Leave: {
        // 拖出视口就放弃这次拖拽：不留下一条孤零零的指示线
        if (m_dragRow < 0) break;
        setDragRowDimmed(m_dragRow, false);
        m_dragRow = -1;
        hideDropIndicator();
        m_table->viewport()->unsetCursor();
        break;
    }
    default:
        break;
    }

    return QWidget::eventFilter(watched, event);
}

void CommandTableWidget::showDropIndicator(int insertAt)
{
    if (!m_table || !m_dropIndicator) return;

    const int rows = m_table->rowCount();
    if (rows == 0) {
        hideDropIndicator();
        return;
    }

    const int clamped = qBound(0, insertAt, rows);
    int y = 0;
    if (clamped < rows) {
        y = m_table->rowViewportPosition(clamped); // 行顶：插到这一行之前
    } else {
        y = m_table->rowViewportPosition(rows - 1) + m_table->rowHeight(rows - 1); // 末尾
    }

    m_dropAt = clamped;
    m_dropIndicator->setGeometry(0, qMax(0, y - 1), m_table->viewport()->width(), 3);
    m_dropIndicator->raise();
    m_dropIndicator->show();
}

void CommandTableWidget::hideDropIndicator()
{
    m_dropAt = -1;
    if (m_dropIndicator) m_dropIndicator->hide();
}

void CommandTableWidget::setDragRowDimmed(int row, bool dimmed)
{
    if (!m_table || row < 0 || row >= m_table->rowCount()) return;

    // 空 QBrush = 恢复默认绘制，取消灰显时不需要知道原色是什么
    const QBrush brush = dimmed ? QBrush(QColor(0xE8, 0xEE, 0xF7)) : QBrush();
    for (int column = 0; column < ColumnCount; ++column) {
        if (auto *item = m_table->item(row, column)) {
            item->setBackground(brush);
        }
    }
}

void CommandTableWidget::onCellChanged(int row, int column)
{
    if (m_updating) return;

    Q_UNUSED(row);

    // 勾选列已控件化（QCheckBox::toggled 直接接命令），cellChanged 不会再为它触发；
    // 这里只剩内容列。
    if (column == ContentColumn) {
        emit commandsChanged();
    }
}

void CommandTableWidget::onSendClicked()
{
    auto *button = qobject_cast<QPushButton*>(sender());
    if (!button) return;

    int row = button->property("row").toInt();
    CommandItem cmd = commandFromRow(row);

    if (cmd.isValid()) {
        emit sendCommand(cmd);
    }
}

void CommandTableWidget::onCustomContextMenuRequested(const QPoint &pos)
{
    const QModelIndex index = m_table->indexAt(pos);

    QMenu menu(this);

    if (index.isValid()) {
        const int row = index.row();
        auto *renameAction = menu.addAction(tr("重命名"));
        connect(renameAction, &QAction::triggered, this, [this, row] { renameRow(row); });

        auto *copyAction = menu.addAction(tr("复制该指令"));
        connect(copyAction, &QAction::triggered, this, [this, row] { copyRows({row}); });

        auto *removeAction = menu.addAction(tr("删除该指令"));
        connect(removeAction, &QAction::triggered, this, [this, row] { removeRows({row}); });
    } else {
        // 空白区域右键：直接补一条指令
        auto *addAction = menu.addAction(tr("添加指令"));
        connect(addAction, &QAction::triggered, this, &CommandTableWidget::addCommand);
    }

    menu.exec(m_table->viewport()->mapToGlobal(pos));
}

void CommandTableWidget::renameRow(int row)
{
    if (row < 0 || row >= m_table->rowCount()) return;

    const CommandItem current = commandFromRow(row);

    bool ok = false;
    // 对话框一律统一使用 StyleDialogs 封装，确保父窗口正确且样式一致
    const QString name = StyleDialogs::getText(this, tr("重命名指令"),
                                               tr("指令名称:"),
                                               current.description, &ok);
    if (!ok) return;

    if (auto *sendButton = sendButtonAt(row)) {
        sendButton->setText(name.isEmpty() ? tr("发送") : name);
        sendButton->setProperty("description", name);
        sendButton->setToolTip(name.isEmpty()
                                   ? tr("左键发送，右键重命名")
                                   : name + QLatin1Char('\n') + tr("左键发送，右键重命名"));
    }
    if (row < m_currentSet.commands.size()) {
        m_currentSet.commands[row].description = name;
    }

    // 名字变长/变短都要让发送列跟着重新量一次
    updateSendColumnWidth();

    emit commandsChanged();
}

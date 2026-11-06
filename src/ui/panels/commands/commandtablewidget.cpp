#include "commandtablewidget.h"
#include "ui/common/combometrics.h"
#include "ui/common/stylehelpers.h"
#include "ui/common/apptheme.h"
#include "ui/common/styledialogs.h"

#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QSpinBox>
#include <QStyle>
#include <QStyledItemDelegate>
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

// 显示名与 tailValues 的实际值同序，存取使用选项 data，不依赖翻译文本。
QStringList tailLabels()
{
    return {QObject::tr("无"), QObject::tr("+\\r"), QObject::tr("+\\n"), QObject::tr("+\\r\\n")};
}

const QStringList &tailValues()
{
    static const QStringList values{QStringLiteral("无"), QStringLiteral("CR"),
                                    QStringLiteral("LF"), QStringLiteral("CR+LF")};
    return values;
}

// Qt 表格的 WordWrap 只在空白处折。AT 指令几乎没有空格，开了折行也还是一行，
// 超长的就在格里被裁掉。每个非空白字符后插一个零宽断点，样式仍按 WordWrap 画
// （颜色、选中、省略都走样式表），断点只存在于绘制和量高，不写进单元格文本。
QString withWrapOpportunities(const QString &text)
{
    QString out;
    out.reserve(text.size() * 2);
    for (const QChar ch : text) {
        out += ch;
        if (!ch.isSpace()) out += QChar(0x200B);
    }
    return out;
}

// 只代理发送框这一列。关：单行 + 末尾省略。开：按列宽折行，编辑时也是折行的文本框，
// 回车仍是提交（跟原来的单行编辑一样），Shift+回车才插入真正的换行。
class ContentColumnDelegate : public QStyledItemDelegate
{
public:
    explicit ContentColumnDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void setWrap(bool wrap) { m_wrap = wrap; }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        if (m_wrap) {
            opt.features |= QStyleOptionViewItem::WrapText;
            opt.text = withWrapOpportunities(opt.text);
        } else {
            opt.features &= ~QStyleOptionViewItem::WrapText;
        }
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (!m_wrap)
            return QStyledItemDelegate::sizeHint(option, index);

        // resizeRowsToContents 传进来的 option.rect 经常是空的。空宽度会让折行
        // 被当成「放得下」，行高就停在单行。列宽才是这段文字实际能用的宽度。
        int width = option.rect.width();
        if (const auto *view = qobject_cast<const QTableView *>(option.widget))
            width = qMax(width, view->columnWidth(index.column()));
        if (width < 32)
            return QStyledItemDelegate::sizeHint(option, index);

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        opt.features |= QStyleOptionViewItem::WrapText;
        opt.rect.setWidth(width);
        opt.text = withWrapOpportunities(index.data(Qt::DisplayRole).toString());
        const QWidget *widget = opt.widget;
        QStyle *style = widget ? widget->style() : QApplication::style();
        QSize styled = style->sizeFromContents(QStyle::CT_ItemViewItem, &opt, QSize(), widget);

        // 样式有时按「不限宽」把折行文本量成一行。再用字体自己量一遍，取更高的那个。
        const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, widget) + 1;
        const int textWidth = qMax(1, width - margin * 2);
        const QRect bound = opt.fontMetrics.boundingRect(
            QRect(0, 0, textWidth, 100000),
            int(Qt::AlignLeft | Qt::AlignTop | Qt::TextWrapAnywhere),
            index.data(Qt::DisplayRole).toString());
        styled.setHeight(qMax(styled.height(), bound.height() + 6));
        return styled;
    }

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override
    {
        if (!m_wrap) return QStyledItemDelegate::createEditor(parent, option, index);

        auto *edit = new QPlainTextEdit(parent);
        edit->setObjectName(QStringLiteral("commandContentEditor"));
        edit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
        edit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        edit->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        edit->setTabChangesFocus(true);
        edit->setFrameShape(QFrame::NoFrame);
        edit->document()->setDocumentMargin(2);
        return edit;
    }

    void setEditorData(QWidget *editor, const QModelIndex &index) const override
    {
        if (auto *plain = qobject_cast<QPlainTextEdit *>(editor)) {
            plain->setPlainText(index.data(Qt::EditRole).toString());
            return;
        }
        QStyledItemDelegate::setEditorData(editor, index);
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override
    {
        if (auto *plain = qobject_cast<QPlainTextEdit *>(editor)) {
            model->setData(index, plain->toPlainText(), Qt::EditRole);
            return;
        }
        QStyledItemDelegate::setModelData(editor, model, index);
    }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<const QKeyEvent *>(event);
            if (qobject_cast<QPlainTextEdit *>(object)
                && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
                && !(key->modifiers() & Qt::ShiftModifier)) {
                auto *editor = static_cast<QWidget *>(object);
                emit commitData(editor);
                emit closeEditor(editor, QAbstractItemDelegate::SubmitModelCache);
                return true;
            }
        }
        return QStyledItemDelegate::eventFilter(object, event);
    }

private:
    bool m_wrap = false;
};

} // namespace

CommandTableWidget::CommandTableWidget(QWidget *parent) : QWidget(parent)
{
    setupUi();
    connect(this, &CommandTableWidget::structureAboutToChange, this, [this] {
        ++m_structureRevision;
        m_dragRow = -1;
        hideDropIndicator();
        m_table->viewport()->unsetCursor();
    });
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
    // 发送框默认单行。折行由顶栏开关打开，并且不能用表格自带的 WordWrap：
    // 那只在空格处折，AT 指令折不动。绘制交给这一列自己的代理。
    m_table->setWordWrap(false);
    m_table->setTextElideMode(Qt::ElideRight);
    m_contentDelegate = new ContentColumnDelegate(m_table);
    m_table->setItemDelegateForColumn(ContentColumn, m_contentDelegate);
    m_table->verticalHeader()->setVisible(false);
    // 行高自适应：行内放的是真控件（勾选框 / SpinBox / ComboBox / 带 padding 的发送按钮），
    // 它们的 sizeHint 比默认行高（30px）高，QSS 又给控件加了 padding + border，于是控件会
    // 溢出行的上下边界（用户截图报告）。ResizeToContents 让每行按最高的那个 cellWidget 撑开
    // ——字体 / DPI / 翻译变化时自动跟随，不写死行高。用例 `commandRowsFitTheirWidgets` 守住。
    // minimumSectionSize 只兜底：无控件行（理论上不会出现）也别被压得比勾选框还矮。
    m_table->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setMinimumSectionSize(24);
    m_table->setSelectionBehavior(QTableWidget::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
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
    // sectionResized 的参数是行号、旧高度、新高度；只同步该行。
    // 批量调整行高时挂起联动，结束后统一同步。
    connect(m_table->verticalHeader(), &QHeaderView::sectionResized, this,
            [this](int row, int, int) { syncCellWidgetGeometry(row, row); });
    connect(m_table->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this](int logical, int oldSize, int newSize) {
                syncCellWidgetGeometry();
                // 只有发送框的宽度变化才影响折行。重算过程中滚动条会再改一次列宽，
                // 那一次不能在这里同步重入（见 scheduleContentRowRefit）。
                if (m_contentWordWrap && logical == ContentColumn && oldSize != newSize)
                    scheduleContentRowRefit();
            });

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
    m_moveCheckedButton = new QPushButton(tr("移动勾选到"), this);
    m_moveCheckedButton->setObjectName(QStringLiteral("moveCheckedCommands"));
    auto *moveMenu = new QMenu(m_moveCheckedButton);
    m_moveCheckedButton->setMenu(moveMenu);
    connect(moveMenu, &QMenu::aboutToShow, this, [this, moveMenu] {
        moveMenu->clear();
        populateMoveMenu(moveMenu);
        moveMenu->setProperty("structureRevision", m_structureRevision);
    });
    connect(moveMenu, &QMenu::triggered, this, [this, moveMenu](QAction *action) {
        if (moveMenu->property("structureRevision").toULongLong() == m_structureRevision)
            emit moveCommandsRequested(checkedRows(), action->data().toUuid());
    });
    updateMoveAvailability();
    m_loopSend = new QCheckBox(tr("循环发送"), this);
    m_loopSend->setObjectName(QStringLiteral("loopBatchSend"));
    m_loopSend->setToolTip(tr("勾选后「按序发送勾选项」发完最后一条回到第一条，再点一次按钮停止"));
    m_batchSendButton = new QPushButton(tr("按序发送勾选项"), this);
    // objectName 保持 primaryButton：QSS 按它画主操作按钮的强调底，改名字会静默丢样式。
    // （测试要定位它时也用这个名字，别再另起一个。）
    m_batchSendButton->setObjectName("primaryButton");
    m_batchSendButton->setToolTip(tr("按列表顺序依次发送所有勾选的指令，每条按其延时等待后发出"));

    connect(m_addButton, &QPushButton::clicked, this, &CommandTableWidget::addCommand);
    connect(m_removeButton, &QPushButton::clicked, this, &CommandTableWidget::removeCommand);
    connect(m_batchSendButton, &QPushButton::clicked, this, [this] {
        // 循环进行中按钮文案是「停止循环」。再点一次只停，不重新开一轮。
        if (m_batchLooping) {
            emit batchSendStopRequested();
            return;
        }
        // 勾选行号与指令一起交给面板：面板负责时序，表格负责"点亮哪一行"，
        // 而行号只有这里知道（见 batchSendRequested 的说明）。
        const QList<int> rows = checkedRows();
        QList<CommandItem> commands;
        commands.reserve(rows.size());
        for (int row : rows) commands.append(m_currentSet.commands.at(row));
        if (!commands.isEmpty())
            emit batchSendRequested(rows, commands, m_loopSend && m_loopSend->isChecked());
    });

    buttonLayout->addWidget(m_addButton);
    buttonLayout->addWidget(m_removeButton);
    buttonLayout->addWidget(m_moveCheckedButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_loopSend);
    buttonLayout->addWidget(m_batchSendButton);

    layout->addLayout(buttonLayout);
}

void CommandTableWidget::setCommandSet(const CommandSet &set)
{
    const bool contentChanged = set.id != m_currentSet.id || set.commands != m_currentSet.commands;
    if (contentChanged) emit structureAboutToChange();
    m_currentSet = set;
    if (contentChanged) refreshTable();
    else updateMoveAvailability();
}

void CommandTableWidget::setMoveTargetSets(const QList<CommandSet> &sets)
{
    // 只保留身份和名称；菜单打开时再生成，不在控件发信号期间销毁菜单项。
    m_moveTargetSets.clear();
    for (const auto &set : sets) m_moveTargetSets.append({set.id, set.name});
    updateMoveAvailability();
}

void CommandTableWidget::populateMoveMenu(QMenu *menu)
{
    for (const auto &target : m_moveTargetSets) {
        if (target.first == m_currentSet.id) continue;
        auto *action = menu->addAction(QString(target.second).replace(QLatin1Char('&'), QStringLiteral("&&")));
        action->setData(target.first);
        action->setToolTip(target.second);
    }
}

void CommandTableWidget::updateMoveAvailability()
{
    const bool hasTarget = std::any_of(m_moveTargetSets.cbegin(), m_moveTargetSets.cend(),
        [this](const auto &target) { return target.first != m_currentSet.id; });
    m_moveCheckedButton->setEnabled(hasTarget && m_checkedCount > 0);
    m_moveCheckedButton->setToolTip(!hasTarget
        ? tr("请先添加其他分组，再移动勾选的指令")
        : tr("将所有已勾选的指令按列表顺序移到目标分组末尾"));
}

CommandSet CommandTableWidget::commandSet() const
{
    // 每次编辑只同步对应行；快照利用 Qt 隐式共享，不再扫描控件树。
    return m_currentSet;
}

int CommandTableWidget::rowCount() const
{
    return m_table ? m_table->rowCount() : 0;
}

bool CommandTableWidget::hasActiveEditor() const
{
    // Delegate editors live directly on the viewport. Cell widgets (checkboxes,
    // delay spin boxes, suffix combos) have their own wrappers instead.
    for (auto *child : m_table->viewport()->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly))
        if (!child->isHidden() && (qobject_cast<QLineEdit *>(child) || qobject_cast<QPlainTextEdit *>(child)))
            return true;
    return false;
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
    rows.reserve(m_checkedCount);
    for (int row = 0; row < m_currentSet.commands.size(); ++row) {
        if (m_currentSet.commands.at(row).enabled) rows.append(row);
    }
    return rows;
}

void CommandTableWidget::refreshTable()
{
    const QScopedValueRollback<bool> updating(m_updating, true);
    m_table->setUpdatesEnabled(false);
    {
        const QSignalBlocker headerBlocker(m_table->horizontalHeader());
        const QScopedValueRollback<bool> geometryGuard(m_geometrySyncSuspended, true);
        // 立即脱离控件树，延迟析构：既避免连续重建积累子控件，也不销毁信号派发者。
        for (int row = 0; row < m_table->rowCount(); ++row) {
            for (int col = 0; col < ColumnCount; ++col) {
                auto *stale = m_table->cellWidget(row, col);
                if (!stale) continue;
                stale->hide();
                m_table->removeCellWidget(row, col);
                stale->setParent(nullptr);
            }
        }
        m_table->clearContents();
        m_table->setRowCount(0);
        m_table->setRowCount(m_currentSet.commands.size());
        m_sendCursor = -1;
        m_sentRows.clear();
        m_checkedCount = 0;
        m_tailLabels = tailLabels();
        m_tailWidthHint = measureTailColumnWidth();
        for (int row = 0; row < m_currentSet.commands.size(); ++row) {
            const auto &command = m_currentSet.commands.at(row);
            updateRow(row, command);
            if (command.enabled) ++m_checkedCount;
        }
        updateSendColumnWidth();
        updateTailColumnWidth();
    }
    // 批量建行期间只挂起几何联动；完成后统一同步一次，避免 O(n²) 布局。
    if (m_contentWordWrap) refitContentRowHeights();
    else syncCellWidgetGeometry();
    m_table->setUpdatesEnabled(true);
    m_table->viewport()->update();
    updateMoveAvailability();
}

void CommandTableWidget::setContentWordWrap(bool wrap)
{
    if (m_contentWordWrap == wrap) return;
    m_contentWordWrap = wrap;
    if (auto *delegate = static_cast<ContentColumnDelegate *>(m_contentDelegate))
        delegate->setWrap(wrap);
    // 关掉时也要重算：把刚才撑高的行收回去，否则单行模式还留着多行的空白。
    refitContentRowHeights();
    if (m_table) m_table->viewport()->update();
}

void CommandTableWidget::scheduleContentRowRefit()
{
    if (!m_contentWordWrap || !m_table || m_geometrySyncSuspended) return;
    if (m_refittingRows) {
        m_refitAgain = true;
        return;
    }
    if (!m_rowRefitTimer) {
        m_rowRefitTimer = new QTimer(this);
        m_rowRefitTimer->setSingleShot(true);
        connect(m_rowRefitTimer, &QTimer::timeout, this, &CommandTableWidget::refitContentRowHeights);
    }
    m_rowRefitTimer->start(0);
}

void CommandTableWidget::refitContentRowHeights()
{
    if (!m_table || m_geometrySyncSuspended || m_refittingRows) return;
    {
        const QScopedValueRollback<bool> guard(m_refittingRows, true);
        m_refitAgain = false;
        // ResizeToContents 不接受程序写进去的行高，折行量出来的高度贴不上去。
        // 折行时改成 Interactive，按内容写好再留在这个模式；关掉折行再交回
        // ResizeToContents，行高重新跟着行内控件走。
        m_table->verticalHeader()->setSectionResizeMode(
            m_contentWordWrap ? QHeaderView::Interactive : QHeaderView::ResizeToContents);
        {
            const QScopedValueRollback<bool> geometryGuard(m_geometrySyncSuspended, true);
            m_table->resizeRowsToContents();
        }
        syncCellWidgetGeometry();
    }
    if (m_refitAgain && m_contentWordWrap) scheduleContentRowRefit();
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
    // 底栏新增移动菜单后也要计入展开宽度，保证操作按钮完整可见。
    return qMax(total + m_table->frameWidth() * 2 + 2, layout()->minimumSize().width());
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

QWidget *CommandTableWidget::wrapFilled(QWidget *content)
{
    // 发送按钮按列宽铺开。居中会让按钮停在文字宽度上，列更宽时两边露出格子底，
    // 点文字两侧等于点空。横向撑满，上下仍留一点空隙，避免贴住网格线。
    auto *host = new QWidget;
    auto *layout = new QHBoxLayout(host);
    layout->setContentsMargins(2, 1, 2, 1);
    layout->setSpacing(0);
    content->setParent(host);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(content, 1);
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

void CommandTableWidget::setBatchLooping(bool looping)
{
    m_batchLooping = looping;
    m_batchSendButton->setProperty("looping", looping);
    m_batchSendButton->setText(looping ? tr("停止循环") : tr("按序发送勾选项"));
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

    // 按钮已经铺满列宽，列宽不用再跟着最长名称长。短名称保持下限，
    // 长名称在上限内加宽，超出的部分仍由按钮省略，全文在提示里。
    int widest = 0;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (auto *button = sendButtonAt(row)) {
            widest = qMax(widest, button->sizeHint().width());
        }
    }
    const int wanted = widest > 0 ? widest + 6 : SendColumnMinWidth;
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
    // 注意：这里不再自己复位 m_updating，由调用方统一管理

    // 凡是放进 cell 的控件一律走 wrapCentered()：宿主（cellWidget）铺满 cell 矩形，
    // 控件在宿主里按布局居中。六个 cellWidget（☑/Hex/延时/结尾符/发送/≡）统一走这一条路，
    // 不给任何一个控件写死边距——字号、DPI、QSS padding、翻译变化时自动跟随。
    // 见 syncCellWidgetGeometry() 的病根说明：不铺满 cell 矩形，任何 AlignCenter 都是假的。

    // 列0: 勾选框。用真正的 QCheckBox 控件而不是 QTableWidgetItem 的 checkState——
    // 后者的指示器由 QStyle 自绘，QSS 的 QCheckBox::indicator 规则完全覆盖不到，
    // 用户看到的只是一个没有勾的小方块。控件化之后样式统一、也能拿到 16px 的勾。
    auto *checkBox = new QCheckBox(this);
    checkBox->setChecked(cmd.enabled);
    checkBox->setToolTip(tr("勾选后可批量发送、移动或删除指令"));
    connect(checkBox, &QCheckBox::toggled, this, [this, row] { onRowEdited(row); });
    m_table->setCellWidget(row, CheckColumn, wrapCentered(checkBox));

    // 列1: No.
    auto *noItem = new QTableWidgetItem(QString::number(row));
    noItem->setFlags(noItem->flags() & ~Qt::ItemIsEditable);
    noItem->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, NumberColumn, noItem);

    // 列2: 发送框（可编辑）。单行放不下时省略号后面还有全文，提示里始终是完整内容。
    auto *contentItem = new QTableWidgetItem(cmd.content);
    contentItem->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    contentItem->setToolTip(cmd.content);
    m_table->setItem(row, ContentColumn, contentItem);

    // 列3: Hex（复选框控件）
    auto *hexCheckBox = new QCheckBox(this);
    hexCheckBox->setChecked(cmd.isHex);
    connect(hexCheckBox, &QCheckBox::toggled, this, [this, row] { onRowEdited(row); });
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
    connect(delaySpinBox, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this, row] { onRowEdited(row); });
    m_table->setCellWidget(row, DelayColumn, wrapCentered(delaySpinBox));

    // 列5: 结尾符（ComboBox）
    // 所有行的下拉项完全一样，但**不能共用一个 QStandardItemModel**：item 带 parent，
    // 同一个 item 出现在多个 combo 里，选中标记会在行间串位。这里改为把标签集合
    // 预存在成员里（refreshTable 开头算一次），逐行仍建自己的 model —— 建 model 是
    // 每行一次的 O(4)，真正的开销在重复调用 tailLabels()（内含 tr()）与重复量列宽。
    auto *tailComboBox = new QComboBox(this);
    for (int i = 0; i < m_tailLabels.size(); ++i)
        tailComboBox->addItem(m_tailLabels.at(i), tailValues().at(i));
    tailComboBox->setCurrentIndex(qMax(0, tailComboBox->findData(cmd.tail)));
    // 按项目统一约定让样式表自己算宽（箭头画在 combo 的右侧内边距里，只按文本量会偏窄）。
    // 之前它没有走这一步，列宽只有 62px 时 "+\\r\\n" 这类文本把可视区挤到几乎为零，
    // 用户看起来就像"点不中、选不了"。
    fitComboToItems(tailComboBox);
    tailComboBox->setToolTip(tr("发送时附加的结尾符"));
    connect(tailComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this, row] { onRowEdited(row); });
    m_table->setCellWidget(row, TailColumn, wrapCentered(tailComboBox));

    // 列6: 发送按钮（文本即指令名）
    auto *sendButton = new QPushButton(cmd.description.isEmpty() ? tr("发送") : cmd.description, this);
    // 描述另存一份到属性：空名称时按钮显示占位"发送"，直接读按钮文本会把占位当成名字
    sendButton->setProperty("description", cmd.description);
    sendButton->setProperty("row", row);
    // 列宽按最长名称封顶。放不下的名字进 tooltip，按钮本身铺满这一列。
    sendButton->setToolTip(cmd.description.isEmpty()
                               ? tr("左键发送，右键重命名")
                               : cmd.description + QLatin1Char('\n') + tr("左键发送，右键重命名"));
    connect(sendButton, &QPushButton::clicked, this, &CommandTableWidget::onSendClicked);
    m_table->setCellWidget(row, SendColumn, wrapFilled(sendButton));

    // 列7: 拖动手柄。做成撑满行高的把手（与行内按钮等高、带浅底），让人一眼看出这里
    // 可以按着拖——之前只是 22px 里一个纯文本 "≡"，用户反馈"太小、不像能拖"。
    auto *handle = new QLabel(tr("≡"), this);
    handle->setObjectName(QStringLiteral("commandDragHandle"));
    handle->setAlignment(Qt::AlignCenter);
    handle->setToolTip(tr("按住上下拖动可调整顺序"));
    // 让鼠标事件直接穿透到视口：拖拽是在 viewport 的 eventFilter 里统一拦的，
    // 手柄控件自己吃掉事件的话就再也拖不动了。
    handle->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_table->setCellWidget(row, HandleColumn, handle);
}

CommandItem CommandTableWidget::commandFromRow(int row) const
{
    if (row < 0 || row >= m_table->rowCount()) return CommandItem();

    // Preserve fields without a table editor (identity and legacy pack mode).
    CommandItem cmd = m_currentSet.commands.value(row);

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
        cmd.tail = tailComboBox->currentData().toString();
    }

    // 列6: 描述（优先取按钮上的属性，取不到再退回按钮文本）
    if (auto *sendButton = sendButtonAt(row)) {
        const auto description = sendButton->property("description");
        cmd.description = description.isValid() ? description.toString() : sendButton->text();
    }

    return cmd;
}

void CommandTableWidget::onRowEdited(int row)
{
    if (m_updating || row < 0 || row >= m_currentSet.commands.size()) return;
    const auto command = commandFromRow(row);
    const auto &previous = m_currentSet.commands.at(row);
    if (command == previous) return;
    m_checkedCount += int(command.enabled) - int(previous.enabled);
    m_currentSet.commands[row] = command;
    updateMoveAvailability();
    emit commandsChanged();
}

void CommandTableWidget::addCommand()
{
    emit structureAboutToChange();
    if (!m_table) return;

    CommandItem newCmd;
    newCmd.content = tr("+CR,TEST;");
    newCmd.description = tr("测试指令");

    m_currentSet.commands.append(newCmd);
    if (newCmd.enabled) ++m_checkedCount;

    {
        const QScopedValueRollback<bool> guard(m_updating, true);
        int row = m_table->rowCount();
        m_table->insertRow(row);
        updateRow(row, newCmd);
    }

    updateSendColumnWidth();
    updateMoveAvailability();
    emit commandsChanged();
}

void CommandTableWidget::copyRows(const QList<int> &rows)
{
    QSet<int> copying;
    for (int row : rows)
        if (row >= 0 && row < m_currentSet.commands.size()) copying.insert(row);
    if (copying.isEmpty()) return;
    emit structureAboutToChange();
    QList<CommandItem> result;
    result.reserve(m_currentSet.commands.size() + copying.size());
    for (int row = 0; row < m_currentSet.commands.size(); ++row) {
        const auto &command = m_currentSet.commands.at(row);
        result.append(command);
        if (copying.contains(row)) {
            auto copy = command;
            copy.id = QUuid::createUuid();
            result.append(copy);
        }
    }
    m_currentSet.commands = std::move(result);
    refreshTable();
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
    const auto revision = m_structureRevision;
    if (rows.isEmpty()) return; // 没勾任何行就没什么可问的，直接不动作
    if (!confirmRemoval(rows.size()) || revision != m_structureRevision) return;
    removeRows(rows);
}

void CommandTableWidget::removeRows(const QList<int> &rows)
{
    QSet<int> removing;
    for (int row : rows)
        if (row >= 0 && row < m_currentSet.commands.size()) removing.insert(row);
    if (removing.isEmpty()) return;
    emit structureAboutToChange();
    QList<CommandItem> remaining;
    remaining.reserve(m_currentSet.commands.size() - removing.size());
    for (int row = 0; row < m_currentSet.commands.size(); ++row)
        if (!removing.contains(row)) remaining.append(m_currentSet.commands.at(row));
    m_currentSet.commands = std::move(remaining);
    refreshTable();
    emit commandsChanged();
}

void CommandTableWidget::moveRow(int from, int to)
{
    const int total = m_currentSet.commands.size();
    if (from < 0 || from >= total || to < 0 || to >= total || from == to) return;
    emit structureAboutToChange();

    // 先搬模型，再整体重建表格。
    // QTableWidget 的自有 InternalMove 只搬 item，Hex/延时/结尾符/发送按钮这些
    // cellWidget 会留在原行；逐个摘挂控件又要踩"setCellWidget 会销毁旧控件"的所有权
    // 坑。全表重建由更新守卫兜住不会引发 commandsChanged 风暴，而且 No. 列、
    // 发送按钮的 row 属性、commands 顺序都从同一份数据重建，天然一致。
    m_currentSet.commands.move(from, to);

    refreshTable();

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
            const auto revision = m_structureRevision;
            QTimer::singleShot(0, this, [this, from, to, revision] {
                if (revision == m_structureRevision) moveRow(from, to);
            });
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
    const QColor dim = AppTheme::isDark() ? QColor(0x2C, 0x35, 0x48) : QColor(0xE8, 0xEE, 0xF7);
    const QBrush brush = dimmed ? QBrush(dim) : QBrush();
    for (int column = 0; column < ColumnCount; ++column) {
        if (auto *item = m_table->item(row, column)) {
            item->setBackground(brush);
        }
    }
}

void CommandTableWidget::onCellChanged(int row, int column)
{
    if (m_updating) return;

    // 勾选列已控件化（QCheckBox::toggled 直接接命令），cellChanged 不会再为它触发；
    // 这里只剩内容列。
    if (column == ContentColumn) {
        if (auto *item = m_table->item(row, column)) {
            // setToolTip 会再发一次 cellChanged。守卫住，避免提示和正文互相触发。
            if (item->toolTip() != item->text()) {
                const QScopedValueRollback<bool> guard(m_updating, true);
                item->setToolTip(item->text());
            }
        }
        if (m_contentWordWrap) refitContentRowHeights();
        onRowEdited(row);
    }
}

void CommandTableWidget::onSendClicked()
{
    auto *button = qobject_cast<QPushButton*>(sender());
    if (!button) return;

    int row = button->property("row").toInt();
    const CommandItem cmd = m_currentSet.commands.value(row);

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
        const auto revision = m_structureRevision;
        // 右键已选行时保留多选；右键其他行时只操作该行，与勾选框无关。
        if (!m_table->selectionModel()->isRowSelected(row, QModelIndex()))
            m_table->selectionModel()->setCurrentIndex(index,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QList<int> selectedRows;
        for (const auto &selected : m_table->selectionModel()->selectedRows())
            selectedRows.append(selected.row());
        std::sort(selectedRows.begin(), selectedRows.end());

        auto *renameAction = menu.addAction(tr("重命名"));
        connect(renameAction, &QAction::triggered, this, [this, row, revision] {
            if (revision == m_structureRevision) renameRow(row);
        });

        auto *copyAction = menu.addAction(tr("复制该指令"));
        connect(copyAction, &QAction::triggered, this, [this, row, revision] {
            if (revision == m_structureRevision) copyRows({row});
        });

        auto *moveMenu = menu.addMenu(tr("移动选中到"));
        moveMenu->setObjectName(QStringLiteral("moveSelectedCommands"));
        populateMoveMenu(moveMenu);
        moveMenu->setEnabled(!moveMenu->isEmpty());
        connect(moveMenu, &QMenu::triggered, this, [this, selectedRows, revision](QAction *action) {
            if (revision == m_structureRevision)
                emit moveCommandsRequested(selectedRows, action->data().toUuid());
        });

        auto *removeAction = menu.addAction(tr("删除该指令"));
        connect(removeAction, &QAction::triggered, this, [this, row, revision] {
            if (revision == m_structureRevision) removeRows({row});
        });
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
    const auto revision = m_structureRevision;

    bool ok = false;
    // 对话框一律统一使用 StyleDialogs 封装，确保父窗口正确且样式一致
    const QString name = StyleDialogs::getText(this, tr("重命名指令"),
                                               tr("指令名称:"),
                                               current.description, &ok);
    if (!ok || revision != m_structureRevision) return;

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

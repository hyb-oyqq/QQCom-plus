#include "commandtablewidget.h"
#include "ui/common/combometrics.h"
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
    m_table->setColumnWidth(ContentColumn, 188);
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
    connect(m_table->verticalHeader(), &QHeaderView::sectionResized, this,
            [this](int, int, int) { syncCellWidgetGeometry(); });
    connect(m_table->horizontalHeader(), &QHeaderView::sectionResized, this,
            [this](int, int, int) { syncCellWidgetGeometry(); });

    connect(m_table, &QTableWidget::cellChanged, this, &CommandTableWidget::onCellChanged);
    connect(m_table, &QTableWidget::customContextMenuRequested,
            this, &CommandTableWidget::onCustomContextMenuRequested);

    layout->addWidget(m_table);

    // 底部按钮（单行铺开：左侧操作类，右侧主操作类）
    auto *buttonLayout = new QHBoxLayout();

    m_addButton = new QPushButton(tr("添加指令"), this);
    m_removeButton = new QPushButton(tr("删除勾选"), this);
    m_removeButton->setToolTip(tr("删除所有已勾选的指令，删除前会先确认"));
    m_batchSendButton = new QPushButton(tr("按序发送勾选项"), this);
    m_batchSendButton->setObjectName("primaryButton");
    m_batchSendButton->setToolTip(tr("按列表顺序依次发送所有勾选的指令，每条按其延时等待后发出"));

    connect(m_addButton, &QPushButton::clicked, this, &CommandTableWidget::addCommand);
    connect(m_removeButton, &QPushButton::clicked, this, &CommandTableWidget::removeCommand);
    connect(m_batchSendButton, &QPushButton::clicked, this, [this] {
        QList<CommandItem> commands = enabledCommands();
        if (!commands.isEmpty()) {
            emit batchSendRequested(commands, 0); // 默认发送到 A
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

    // 一次定行数，省掉 setRowCount(0) + 逐行 insertRow 的反复行变更
    m_table->setRowCount(m_currentSet.commands.size());

    for (int i = 0; i < m_currentSet.commands.size(); ++i) {
        updateRow(i, m_currentSet.commands.at(i));
    }

    // 行建完才知道最长指令名有多宽：发送列按它自适应，避免长名称被截断
    updateSendColumnWidth();
    // 结尾符列同样按内容量——否则 "+\\r\\n" 会被裁成 "+\\"（用户实测）
    updateTailColumnWidth();
    // 行数/行高/列宽都定下来了，此时才拿到最终 visualRect，统一重排一次 cellWidget。
    // 必须在这里做——上面几步每改一步 Qt 都会重算行高，早跑等于按旧的行高摆控件。
    syncCellWidgetGeometry();
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
    layout->addWidget(content, 0, Qt::AlignCenter);
    return host;
}

void CommandTableWidget::syncCellWidgetGeometry()
{
    if (!m_table || m_syncingGeometry) return;

    // 重排期间 setGeometry 本身可能再触发 header 的 sectionResized
    // （改列宽的行高重算），加个自锁避免递归重入。
    const QScopedValueRollback<bool> guard(m_syncingGeometry, true);

    const int rows = m_table->rowCount();
    for (int row = 0; row < rows; ++row) {
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

void CommandTableWidget::updateTailColumnWidth()
{
    if (!m_table) return;

    // 结尾符下拉项的文本会随字体/DPI/翻译变化，写死列宽必然在某个环境下把
    // "+\\r\\n" 裁成 "+\\"（用户实测报告）。改成按 combo 自己的 sizeHint 量：
    // fitComboToItems 已让它把箭头（画在 24px 右内边距里）也算进去，这里只要
    // 取最大者即可。没有行时用一次性探针 combo 量出下限，空表也不会把列压没。
    int widest = 0;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (auto *box = tailComboBoxAt(row)) {
            widest = qMax(widest, box->sizeHint().width());
        }
    }
    if (widest == 0) {
        QComboBox probe;
        probe.addItems(tailLabels());
        fitComboToItems(&probe);
        widest = probe.sizeHint().width();
    }
    const int width = qMax(TailColumnMinWidth, widest + 6);
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
    auto *tailComboBox = new QComboBox(this);
    tailComboBox->addItems(tailLabels());
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
            moveRow(from, to); // 内部会 refreshTable，灰显随行重建一起消失
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
        emit sendCommand(cmd, 0); // 默认发送到 A
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

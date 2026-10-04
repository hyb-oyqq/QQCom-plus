#include "commandtablewidget.h"
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

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
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels({tr("☑"), tr("No."), tr("发送框"),
                                        tr("Hex"), tr("延时"), tr("组包"), tr("发送")});

    // 列宽设置
    m_table->setColumnWidth(0, 30);  // 勾选框
    m_table->setColumnWidth(1, 40);  // No.
    m_table->setColumnWidth(2, 200); // 发送框
    m_table->setColumnWidth(3, 50);  // Hex
    m_table->setColumnWidth(4, 60);  // 延时
    m_table->setColumnWidth(5, 80);  // 组包
    m_table->setColumnWidth(6, 120); // 发送按钮

    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QTableWidget::SelectRows);
    m_table->setEditTriggers(QTableWidget::DoubleClicked | QTableWidget::EditKeyPressed);

    connect(m_table, &QTableWidget::cellChanged, this, &CommandTableWidget::onCellChanged);

    layout->addWidget(m_table);

    // 底部按钮
    auto *buttonLayout = new QHBoxLayout();

    m_addButton = new QPushButton(tr("添加指令"), this);
    m_removeButton = new QPushButton(tr("删除选中"), this);
    m_batchSendButton = new QPushButton(tr("一键初始化"), this);
    m_batchSendButton->setObjectName("primaryButton");

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

QList<CommandItem> CommandTableWidget::selectedCommands() const
{
    QList<CommandItem> commands;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 0) && m_table->item(row, 0)->checkState() == Qt::Checked) {
            commands.append(commandFromRow(row));
        }
    }
    return commands;
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

void CommandTableWidget::refreshTable()
{
    m_updating = true;

    m_table->setRowCount(0);

    for (int i = 0; i < m_currentSet.commands.size(); ++i) {
        int row = m_table->rowCount();
        m_table->insertRow(row);
        updateRow(row, m_currentSet.commands[i]);
    }

    m_updating = false;
}

void CommandTableWidget::updateRow(int row, const CommandItem &cmd)
{
    m_updating = true;

    // 列0: 勾选框
    auto *checkItem = new QTableWidgetItem();
    checkItem->setCheckState(cmd.enabled ? Qt::Checked : Qt::Unchecked);
    checkItem->setFlags(checkItem->flags() & ~Qt::ItemIsEditable);
    m_table->setItem(row, 0, checkItem);

    // 列1: No.
    auto *noItem = new QTableWidgetItem(QString::number(row));
    noItem->setFlags(noItem->flags() & ~Qt::ItemIsEditable);
    noItem->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, 1, noItem);

    // 列2: 发送框（可编辑）
    auto *contentItem = new QTableWidgetItem(cmd.content);
    m_table->setItem(row, 2, contentItem);

    // 列3: Hex（复选框控件）
    auto *hexCheckBox = new QCheckBox(this);
    hexCheckBox->setChecked(cmd.isHex);
    auto *hexWidget = new QWidget(this);
    auto *hexLayout = new QHBoxLayout(hexWidget);
    hexLayout->addWidget(hexCheckBox);
    hexLayout->setAlignment(Qt::AlignCenter);
    hexLayout->setContentsMargins(0, 0, 0, 0);
    connect(hexCheckBox, &QCheckBox::toggled, this, [this] {
        if (!m_updating) emit commandsChanged();
    });
    m_table->setCellWidget(row, 3, hexWidget);

    // 列4: 延时（SpinBox）
    auto *delaySpinBox = new QSpinBox(this);
    delaySpinBox->setRange(0, 10000);
    delaySpinBox->setSuffix(" ms");
    delaySpinBox->setValue(cmd.delayMs);
    connect(delaySpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
        if (!m_updating) emit commandsChanged();
    });
    m_table->setCellWidget(row, 4, delaySpinBox);

    // 列5: 组包（ComboBox）
    auto *packComboBox = new QComboBox(this);
    packComboBox->addItems({tr("无"), tr("组包")});
    packComboBox->setCurrentText(cmd.packMode);
    connect(packComboBox, &QComboBox::currentTextChanged, this, [this] {
        if (!m_updating) emit commandsChanged();
    });
    m_table->setCellWidget(row, 5, packComboBox);

    // 列6: 发送按钮
    auto *sendButton = new QPushButton(cmd.description.isEmpty() ? tr("发送") : cmd.description, this);
    sendButton->setProperty("row", row);
    connect(sendButton, &QPushButton::clicked, this, &CommandTableWidget::onSendClicked);
    m_table->setCellWidget(row, 6, sendButton);

    m_updating = false;
}

CommandItem CommandTableWidget::commandFromRow(int row) const
{
    if (row < 0 || row >= m_table->rowCount()) return CommandItem();

    CommandItem cmd;

    // 列0: 勾选框
    if (m_table->item(row, 0)) {
        cmd.enabled = m_table->item(row, 0)->checkState() == Qt::Checked;
    }

    // 列2: 内容
    if (m_table->item(row, 2)) {
        cmd.content = m_table->item(row, 2)->text();
    }

    // 列3: Hex
    if (m_table->cellWidget(row, 3)) {
        auto *hexWidget = m_table->cellWidget(row, 3);
        auto *hexCheckBox = hexWidget->findChild<QCheckBox*>();
        if (hexCheckBox) {
            cmd.isHex = hexCheckBox->isChecked();
        }
    }

    // 列4: 延时
    if (m_table->cellWidget(row, 4)) {
        auto *delaySpinBox = qobject_cast<QSpinBox*>(m_table->cellWidget(row, 4));
        if (delaySpinBox) {
            cmd.delayMs = delaySpinBox->value();
        }
    }

    // 列5: 组包
    if (m_table->cellWidget(row, 5)) {
        auto *packComboBox = qobject_cast<QComboBox*>(m_table->cellWidget(row, 5));
        if (packComboBox) {
            cmd.packMode = packComboBox->currentText();
        }
    }

    // 列6: 描述（从按钮文本获取）
    if (m_table->cellWidget(row, 6)) {
        auto *sendButton = qobject_cast<QPushButton*>(m_table->cellWidget(row, 6));
        if (sendButton) {
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

    int row = m_table->rowCount();
    m_table->insertRow(row);
    updateRow(row, newCmd);

    emit commandsChanged();
}

void CommandTableWidget::removeCommand()
{
    QList<int> selectedRows;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 0) && m_table->item(row, 0)->checkState() == Qt::Checked) {
            selectedRows.append(row);
        }
    }

    if (selectedRows.isEmpty()) return;

    // 从后往前删除，避免索引变化
    for (int i = selectedRows.size() - 1; i >= 0; --i) {
        int row = selectedRows[i];
        m_table->removeRow(row);
        if (row < m_currentSet.commands.size()) {
            m_currentSet.commands.removeAt(row);
        }
    }

    // 更新所有行号
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, 1)) {
            m_table->item(row, 1)->setText(QString::number(row));
        }
    }

    emit commandsChanged();
}

void CommandTableWidget::onCellChanged(int row, int column)
{
    if (m_updating) return;

    Q_UNUSED(row);

    // 列0: 勾选框变化
    if (column == 0) {
        emit commandsChanged();
    }
    // 列2: 内容变化
    else if (column == 2) {
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

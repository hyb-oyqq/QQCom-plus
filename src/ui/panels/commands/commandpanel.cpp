#include "commandpanel.h"
#include "commandtablewidget.h"
#include "app/commandmanager.h"
#include "persistence/preferencesstore.h"
#include "domain/commanditem.h"
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

CommandPanel::CommandPanel(QWidget *parent) : QWidget(parent)
{
    setupUi();

    // 连接 CommandManager 信号
    connect(CommandManager::instance(), &CommandManager::setsChanged,
            this, &CommandPanel::refreshSetComboBox);
    connect(CommandManager::instance(), &CommandManager::undoRedoChanged,
            this, &CommandPanel::refreshUndoRedoButtons);

    // 周期发送定时器
    m_periodicTimer = new QTimer(this);
    connect(m_periodicTimer, &QTimer::timeout, this, &CommandPanel::onPeriodicSendTimeout);

    refreshSetComboBox();
    refreshUndoRedoButtons();
}

void CommandPanel::setupUi()
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4, 4, 4, 4);
    mainLayout->setSpacing(4);

    // 顶部工具栏
    auto *topBar = new QHBoxLayout();

    m_setComboBox = new QComboBox(this);
    m_setComboBox->setMinimumWidth(150);
    connect(m_setComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CommandPanel::onCurrentSetChanged);

    m_addSetButton = new QPushButton(tr("添加"), this);
    m_removeSetButton = new QPushButton(tr("删除"), this);
    m_notesButton = new QPushButton(tr("备注"), this);
    m_undoButton = new QPushButton(tr("↶"), this);
    m_redoButton = new QPushButton(tr("↷"), this);
    m_importButton = new QPushButton(tr("导入"), this);
    m_exportButton = new QPushButton(tr("导出"), this);
    m_expandButton = new QPushButton(tr("«"), this);
    m_expandButton->setMaximumWidth(30);

    m_undoButton->setToolTip(tr("撤销"));
    m_redoButton->setToolTip(tr("重做"));
    m_expandButton->setToolTip(tr("收起面板"));

    connect(m_addSetButton, &QPushButton::clicked, this, &CommandPanel::onAddSetClicked);
    connect(m_removeSetButton, &QPushButton::clicked, this, &CommandPanel::onRemoveSetClicked);
    connect(m_notesButton, &QPushButton::clicked, this, &CommandPanel::onNotesClicked);
    connect(m_undoButton, &QPushButton::clicked, this, &CommandPanel::onUndoClicked);
    connect(m_redoButton, &QPushButton::clicked, this, &CommandPanel::onRedoClicked);
    connect(m_importButton, &QPushButton::clicked, this, &CommandPanel::onImportClicked);
    connect(m_exportButton, &QPushButton::clicked, this, &CommandPanel::onExportClicked);
    connect(m_expandButton, &QPushButton::clicked, this, [this] {
        setExpanded(!m_expanded);
    });

    topBar->addWidget(new QLabel(tr("分组:"), this));
    topBar->addWidget(m_setComboBox);
    topBar->addWidget(m_addSetButton);
    topBar->addWidget(m_removeSetButton);
    topBar->addWidget(m_notesButton);
    topBar->addWidget(m_undoButton);
    topBar->addWidget(m_redoButton);
    topBar->addWidget(m_importButton);
    topBar->addWidget(m_exportButton);
    topBar->addStretch();
    topBar->addWidget(m_expandButton);

    mainLayout->addLayout(topBar);

    // 标签页
    m_tabs = new QTabWidget(this);

    // 标签页1: 初始化
    m_initTable = new CommandTableWidget(this);
    connect(m_initTable, &CommandTableWidget::commandsChanged,
            this, &CommandPanel::onCommandsChanged);
    connect(m_initTable, &CommandTableWidget::sendCommand,
            this, &CommandPanel::onSendCommand);
    connect(m_initTable, &CommandTableWidget::batchSendRequested,
            this, &CommandPanel::onBatchSendRequested);
    m_tabs->addTab(m_initTable, tr("初始化"));

    // 标签页2: 周期发送
    auto *periodicWidget = new QWidget(this);
    auto *periodicLayout = new QVBoxLayout(periodicWidget);

    auto *periodicControlLayout = new QHBoxLayout();
    periodicControlLayout->addWidget(new QLabel(tr("间隔:"), this));
    m_periodicInterval = new QSpinBox(this);
    m_periodicInterval->setRange(100, 60000);
    m_periodicInterval->setValue(1000);
    m_periodicInterval->setSuffix(" ms");
    periodicControlLayout->addWidget(m_periodicInterval);

    m_startPeriodicButton = new QPushButton(tr("启动周期发送"), this);
    m_startPeriodicButton->setCheckable(true);
    connect(m_startPeriodicButton, &QPushButton::toggled, this, [this](bool checked) {
        if (checked) {
            m_periodicTimer->start(m_periodicInterval->value());
            m_startPeriodicButton->setText(tr("停止周期发送"));
            m_periodicInterval->setEnabled(false);
        } else {
            m_periodicTimer->stop();
            m_startPeriodicButton->setText(tr("启动周期发送"));
            m_periodicInterval->setEnabled(true);
        }
    });
    periodicControlLayout->addWidget(m_startPeriodicButton);
    periodicControlLayout->addStretch();

    periodicLayout->addLayout(periodicControlLayout);

    m_periodicTable = new CommandTableWidget(this);
    connect(m_periodicTable, &CommandTableWidget::commandsChanged,
            this, &CommandPanel::onCommandsChanged);
    connect(m_periodicTable, &CommandTableWidget::sendCommand,
            this, &CommandPanel::onSendCommand);
    periodicLayout->addWidget(m_periodicTable);

    m_tabs->addTab(periodicWidget, tr("周期发送"));

    // 标签页3: 解析(HEX) - 占位
    auto *hexParseWidget = new QWidget(this);
    auto *hexParseLayout = new QVBoxLayout(hexParseWidget);
    hexParseLayout->addWidget(new QLabel(tr("HEX 解析功能待实现..."), this));
    hexParseLayout->addStretch();
    m_tabs->addTab(hexParseWidget, tr("解析(HEX)"));

    // 标签页4: 解析(TXT) - 占位
    auto *txtParseWidget = new QWidget(this);
    auto *txtParseLayout = new QVBoxLayout(txtParseWidget);
    txtParseLayout->addWidget(new QLabel(tr("TXT 解析功能待实现..."), this));
    txtParseLayout->addStretch();
    m_tabs->addTab(txtParseWidget, tr("解析(TXT)"));

    mainLayout->addWidget(m_tabs);
}

void CommandPanel::setPreferencesStore(PreferencesStore *store)
{
    m_preferences = store;
    loadPreferences();
}

void CommandPanel::setExpanded(bool expanded)
{
    m_expanded = expanded;

    if (expanded) {
        m_tabs->show();
        m_expandButton->setText(tr("«"));
        m_expandButton->setToolTip(tr("收起面板"));
        setMinimumWidth(400);
    } else {
        m_tabs->hide();
        m_expandButton->setText(tr("»"));
        m_expandButton->setToolTip(tr("展开面板"));
        setMaximumWidth(100);
    }

    adjustSize();
}

void CommandPanel::onCurrentSetChanged(int index)
{
    if (index < 0) return;

    QUuid setId = m_setComboBox->itemData(index).toUuid();
    if (setId.isNull()) return;

    // 保存旧的指令集
    saveCurrentSet();

    // 加载新的指令集
    m_currentSetId = setId;
    CommandSet set = CommandManager::instance()->getSet(setId);
    m_initTable->setCommandSet(set);
    m_periodicTable->setCommandSet(set);
}

void CommandPanel::onAddSetClicked()
{
    bool ok;
    QString name = QInputDialog::getText(this, tr("添加指令集"),
                                         tr("指令集名称:"), QLineEdit::Normal,
                                         tr("新指令集"), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    CommandSet newSet(name);
    CommandManager::instance()->addSet(newSet);
    savePreferences();

    // 选中新增的指令集
    for (int i = 0; i < m_setComboBox->count(); ++i) {
        if (m_setComboBox->itemData(i).toUuid() == newSet.id) {
            m_setComboBox->setCurrentIndex(i);
            break;
        }
    }
}

void CommandPanel::onRemoveSetClicked()
{
    if (m_setComboBox->count() <= 1) {
        QMessageBox::warning(this, tr("无法删除"), tr("至少保留一个指令集"));
        return;
    }

    QUuid currentId = m_setComboBox->currentData().toUuid();
    if (currentId.isNull()) return;

    QString message = tr("确定要删除指令集 \"%1\" 吗？").arg(m_setComboBox->currentText());
    int ret = QMessageBox::question(this, tr("删除确认"), message);
    if (ret != QMessageBox::Yes) return;

    CommandManager::instance()->removeSet(currentId);
    savePreferences();
}

void CommandPanel::onNotesClicked()
{
    QUuid currentId = m_setComboBox->currentData().toUuid();
    if (currentId.isNull()) return;

    CommandSet set = CommandManager::instance()->getSet(currentId);

    bool ok;
    QString notes = QInputDialog::getMultiLineText(this, tr("备注"),
                                                    tr("指令集备注:"),
                                                    set.notes, &ok);
    if (!ok) return;

    set.notes = notes;
    CommandManager::instance()->updateSet(currentId, set);
    savePreferences();
}

void CommandPanel::onUndoClicked()
{
    CommandManager::instance()->undo();
    refreshSetComboBox();
    savePreferences();
}

void CommandPanel::onRedoClicked()
{
    CommandManager::instance()->redo();
    refreshSetComboBox();
    savePreferences();
}

void CommandPanel::onImportClicked()
{
    QString filePath = QFileDialog::getOpenFileName(this, tr("导入指令集"),
                                                     QString(),
                                                     tr("JSON 文件 (*.json)"));
    if (filePath.isEmpty()) return;

    QString errorMessage;
    bool success = CommandManager::instance()->importFromFile(filePath, errorMessage);

    if (success) {
        QMessageBox::information(this, tr("导入成功"), tr("指令集已成功导入"));
        savePreferences();
    } else {
        QMessageBox::critical(this, tr("导入失败"), errorMessage);
    }
}

void CommandPanel::onExportClicked()
{
    QString filePath = QFileDialog::getSaveFileName(this, tr("导出指令集"),
                                                     "commands.json",
                                                     tr("JSON 文件 (*.json)"));
    if (filePath.isEmpty()) return;

    bool success = CommandManager::instance()->exportToFile(filePath);

    if (success) {
        QMessageBox::information(this, tr("导出成功"),
                                 tr("指令集已导出到:\n%1").arg(filePath));
    } else {
        QMessageBox::critical(this, tr("导出失败"), tr("无法写入文件"));
    }
}

void CommandPanel::onCommandsChanged()
{
    saveCurrentSet();
    savePreferences();
}

void CommandPanel::onSendCommand(const CommandItem &cmd, int targetEndpoint)
{
    // 延时后发送
    QTimer::singleShot(cmd.delayMs, this, [this, cmd, targetEndpoint] {
        emit sendCommand(cmd.content, cmd.isHex, targetEndpoint);
    });
}

void CommandPanel::onBatchSendRequested(const QList<CommandItem> &commands, int targetEndpoint)
{
    // 批量发送，累加延时
    int totalDelay = 0;
    for (const auto &cmd : commands) {
        totalDelay += cmd.delayMs;
        QTimer::singleShot(totalDelay, this, [this, cmd, targetEndpoint] {
            emit sendCommand(cmd.content, cmd.isHex, targetEndpoint);
        });
    }
}

void CommandPanel::onPeriodicSendTimeout()
{
    // 周期发送所有启用的指令
    QList<CommandItem> commands = m_periodicTable->enabledCommands();
    if (!commands.isEmpty()) {
        onBatchSendRequested(commands, 0); // 默认发送到 A
    }
}

void CommandPanel::refreshSetComboBox()
{
    QUuid currentId = m_setComboBox->currentData().toUuid();

    m_setComboBox->clear();

    QList<CommandSet> sets = CommandManager::instance()->allSets();
    for (const auto &set : sets) {
        m_setComboBox->addItem(set.name, set.id.toString());
    }

    // 恢复选中
    int indexToSelect = 0;
    for (int i = 0; i < m_setComboBox->count(); ++i) {
        if (m_setComboBox->itemData(i).toUuid() == currentId) {
            indexToSelect = i;
            break;
        }
    }
    m_setComboBox->setCurrentIndex(indexToSelect);

    m_removeSetButton->setEnabled(m_setComboBox->count() > 1);
}

void CommandPanel::refreshUndoRedoButtons()
{
    m_undoButton->setEnabled(CommandManager::instance()->canUndo());
    m_redoButton->setEnabled(CommandManager::instance()->canRedo());
}

void CommandPanel::saveCurrentSet()
{
    if (m_currentSetId.isNull()) return;

    CommandSet set = m_initTable->commandSet();
    set.id = m_currentSetId;

    // 保留原来的名称和备注
    CommandSet oldSet = CommandManager::instance()->getSet(m_currentSetId);
    set.name = oldSet.name;
    set.notes = oldSet.notes;

    CommandManager::instance()->updateSet(m_currentSetId, set);
}

void CommandPanel::loadPreferences()
{
    if (!m_preferences) return;

    CommandManager::instance()->load(m_preferences);

    // 恢复展开状态
    bool expanded = m_preferences->value("commandPanel/expanded", true).toBool();
    setExpanded(expanded);

    // 恢复当前选中的指令集
    QString lastSetId = m_preferences->value("commandPanel/currentSet").toString();
    if (!lastSetId.isEmpty()) {
        for (int i = 0; i < m_setComboBox->count(); ++i) {
            if (m_setComboBox->itemData(i).toString() == lastSetId) {
                m_setComboBox->setCurrentIndex(i);
                break;
            }
        }
    }
}

void CommandPanel::savePreferences()
{
    if (!m_preferences) return;

    CommandManager::instance()->save(m_preferences);

    // 保存展开状态
    m_preferences->setValue("commandPanel/expanded", m_expanded);

    // 保存当前选中的指令集
    m_preferences->setValue("commandPanel/currentSet",
                            m_setComboBox->currentData().toString());
}

#include "commandpanel.h"
#include "commandtablewidget.h"
#include "exportgroupsdialog.h"
#include "app/commandformats.h"
#include "app/commandmanager.h"
#include "persistence/preferencesstore.h"
#include "domain/commanditem.h"
#include "ui/common/styledialogs.h"
#include <QComboBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

CommandPanel::CommandPanel(QWidget *parent) : QWidget(parent)
{
    setupUi();

    // 连接 CommandManager 信号
    connect(CommandManager::instance(), &CommandManager::setsChanged,
            this, &CommandPanel::refreshSetComboBox);

    refreshSetComboBox();

    // 构造完就把"默认展开"落到控件上（最小宽度、内边距）。不能只靠 m_expanded 的
    // 初值——那是"状态"不是"几何"，外部再调 setExpanded(true) 会因为值没变而直接返回。
    applyExpandedState();
}

void CommandPanel::setupUi()
{
    m_mainLayout = new QVBoxLayout(this);
    // 左右边距按面板当前宽度等比给（QSS 给不出"随宽度变化"的值），上下保持 4px。
    // 见 updateMargins：拖动右侧分栏边框时这里会跟着重算。
    m_mainLayout->setContentsMargins(4, 4, 4, 4);
    m_mainLayout->setSpacing(4);

    // 顶部工具栏（单行铺开）- 放入容器以便整体控制显示/隐藏
    m_topBarWidget = new QWidget(this);
    auto *topBar = new QHBoxLayout(m_topBarWidget);
    topBar->setContentsMargins(0, 0, 0, 0);

    m_setComboBox = new QComboBox(m_topBarWidget);
    m_setComboBox->setMinimumWidth(120);
    connect(m_setComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CommandPanel::onCurrentSetChanged);

    // 撤销/重做已移除：指令面板的编辑都是"所见即所得"的即时落库，撤销栈带来的
    // 隐蔽状态反而让用户困惑（用户钦定）。
    m_addSetButton = new QPushButton(tr("添加"), m_topBarWidget);
    m_removeSetButton = new QPushButton(tr("删除"), m_topBarWidget);
    m_renameSetButton = new QPushButton(tr("重命名"), m_topBarWidget);
    m_renameSetButton->setObjectName(QStringLiteral("renameCommandSet"));
    m_importButton = new QPushButton(tr("导入"), m_topBarWidget);
    m_exportButton = new QPushButton(tr("导出"), m_topBarWidget);

    connect(m_addSetButton, &QPushButton::clicked, this, &CommandPanel::onAddSetClicked);
    connect(m_removeSetButton, &QPushButton::clicked, this, &CommandPanel::onRemoveSetClicked);
    connect(m_renameSetButton, &QPushButton::clicked, this, &CommandPanel::onRenameSetClicked);
    connect(m_importButton, &QPushButton::clicked, this, &CommandPanel::onImportClicked);
    connect(m_exportButton, &QPushButton::clicked, this, &CommandPanel::onExportClicked);

    topBar->addWidget(new QLabel(tr("分组:"), m_topBarWidget));
    topBar->addWidget(m_setComboBox);
    topBar->addWidget(m_addSetButton);
    topBar->addWidget(m_removeSetButton);
    topBar->addWidget(m_renameSetButton);
    topBar->addStretch();
    topBar->addWidget(m_importButton);
    topBar->addWidget(m_exportButton);

    // 顶栏铺开要多宽，只记下来给「展开面板」当起步宽度。
    // 不再写成控件下限：下限会经分栏回到主窗口，手动把窗口拖窄时被顶住。
    {
        int wanted = topBar->spacing() * (topBar->count() - 1);
        for (int i = 0; i < topBar->count(); ++i) {
            if (auto *item = topBar->itemAt(i)) {
                if (item->spacerItem()) continue; // 弹性空档不占最小宽度
                if (auto *widget = item->widget()) wanted += widget->sizeHint().width();
            }
        }
        m_topBarMinimumWidth = wanted;
    }
    m_mainLayout->addWidget(m_topBarWidget);

    // 指令表格：只有一份列表，周期发送与协议解析页已移除
    m_commandTable = new CommandTableWidget(this);
    connect(m_commandTable, &CommandTableWidget::commandsChanged,
            this, &CommandPanel::onCommandsChanged);
    connect(m_commandTable, &CommandTableWidget::sendCommand,
            this, &CommandPanel::onSendCommand);
    connect(m_commandTable, &CommandTableWidget::batchSendRequested,
            this, &CommandPanel::onBatchSendRequested);

    m_mainLayout->addWidget(m_commandTable, 1);
}

void CommandPanel::setPreferencesStore(PreferencesStore *store)
{
    m_preferences = store;
    loadPreferences();
}

void CommandPanel::setExpanded(bool expanded)
{
    // 面板整体的收起/展开由监视区「扩展」开关驱动（见 TrafficPanel::setCommandPanelExpanded）。
    // 这里只负责把内容区收起来——外侧的分栏会跟着缩到只剩工具栏那条宽。
    if (m_expanded == expanded) return;
    m_expanded = expanded;
    applyExpandedState();
    savePreferences();
    emit expandedChanged(m_expanded);  // 通知主窗口调整分割器
}

void CommandPanel::applyExpandedState()
{
    // 折叠时隐藏顶部工具栏和指令表格，为数据显示让出空间
    if (m_topBarWidget) m_topBarWidget->setVisible(m_expanded);
    if (m_commandTable) m_commandTable->setVisible(m_expanded);

    if (m_expanded) {
        // 展开时的最小宽度 = max(表格内容所需宽度, 顶栏铺开所需宽度) + 左右内边距。
        // 之前写死 520：窗口窄的时候分栏仍会把面板压到 520 以下，顶栏的"分组"下拉与
        // 表格末尾的「≡」手柄列就被压没了（用户反馈）。改成从内容算出来的宽度，
        // 并把内边距加回去，才顶得住挤压。
        if (m_commandTable) {
            m_commandTable->refreshColumnWidths();
            m_commandTableMinimumWidth = m_commandTable->minimumContentWidth();
        }
        const int side = m_mainLayout->contentsMargins().left()
            + m_mainLayout->contentsMargins().right();
        const int wanted = qMax(m_commandTableMinimumWidth, m_topBarMinimumWidth) + side;
        m_expandedMinimumWidth = qMax(wanted, 520); // 展开时窗口加宽用的首选宽度，不是拖动下限
        setMinimumWidth(0);
        setMaximumWidth(QWIDGETSIZE_MAX);
    } else {
        setMinimumWidth(0);
        // 折叠后面板宽度为 0，完全隐藏
        setMaximumWidth(0);
    }

    updateMargins();
}

void CommandPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // 拖动右侧分栏边框只改变本控件宽度，布局不会替我们做这件事：宽度变了就重算
    // 内边距。表格那一列是 stretch（QHeaderView::Stretch），Qt 自己会跟着宽度重排，
    // 不必在这里插手，否则会和"发送列按最长名称自适应"打架。
    updateMargins();
}

void CommandPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateMargins();
    // 首次显示时表格宽度到这里才定型：让发送列按可用宽度再量一次
    if (m_commandTable) m_commandTable->refreshColumnWidths();
}

void CommandPanel::updateMargins()
{
    if (!m_mainLayout) return;

    // 面板越宽，左右留白越大——但只在很窄的区间内变化（4~10px），避免宽面板反而
    // 把内容挤得离边框太近。上下保持 4px：纵向空间永远让给数据区。
    const int panelWidth = QWidget::width();
    const int side = qBound(4, (panelWidth - 400) / 80 + 4, 10);
    if (m_mainLayout->contentsMargins().left() == side
        && m_mainLayout->contentsMargins().top() == 4) {
        return;
    }
    m_mainLayout->setContentsMargins(side, 4, side, 4);
}

void CommandPanel::onCurrentSetChanged(int index)
{
    if (index < 0) return;
    // 下拉框重建期间的伪变更：不回写、不重载，交给 refreshSetComboBox 自己收尾
    if (m_updatingSets) return;

    QUuid setId = m_setComboBox->itemData(index).toUuid();
    if (setId.isNull() || setId == m_currentSetId) return;

    // 保存旧的指令集（静默回写，不触发 setsChanged）
    saveCurrentSet();

    // 加载新的指令集
    m_currentSetId = setId;
    CommandSet set = CommandManager::instance()->getSet(setId);
    m_commandTable->setCommandSet(set);
}

void CommandPanel::onAddSetClicked()
{
    bool ok;
    QString name = StyleDialogs::getText(this, tr("添加指令集"),
                                         tr("指令集名称:"),
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
        StyleDialogs::warning(this, tr("无法删除"), tr("至少保留一个指令集"));
        return;
    }

    QUuid currentId = m_setComboBox->currentData().toUuid();
    if (currentId.isNull()) return;

    QString message = tr("确定要删除指令集 \"%1\" 吗？").arg(m_setComboBox->currentText());
    if (!StyleDialogs::question(this, tr("删除确认"), message)) return;

    CommandManager::instance()->removeSet(currentId);
    savePreferences();
}

void CommandPanel::onRenameSetClicked()
{
    QUuid currentId = m_setComboBox->currentData().toUuid();
    if (currentId.isNull()) return;

    CommandSet set = CommandManager::instance()->getSet(currentId);

    bool ok;
    const QString name = StyleDialogs::getText(this, tr("重命名指令集"),
                                               tr("指令集名称:"),
                                               set.name, &ok).trimmed();
    if (!ok || name.isEmpty() || name == set.name) return;

    set.name = name;
    // 名称显示在下拉框中，更新后需要广播，让当前选中项立即显示新名称。
    CommandManager::instance()->updateSet(currentId, set, true);
    savePreferences();
}

void CommandPanel::onImportClicked()
{
    QString filePath = StyleDialogs::getOpenFileName(this, tr("导入指令集"),
                                                     QString(),
                                                     tr("指令集文件，含 VOFA+ (*.json *.port *.blemsg);;所有文件 (*.*)"));
    if (filePath.isEmpty()) return;

    QString errorMessage;
    QString detectedLabel;
    bool success = CommandManager::instance()->importFromFile(filePath, errorMessage,
                                                              &detectedLabel);

    if (success) {
        StyleDialogs::information(this, tr("导入成功"),
                                 tr("已按 %1 格式导入指令集").arg(detectedLabel));
        savePreferences();
    } else {
        StyleDialogs::critical(this, tr("导入失败"), errorMessage);
    }
}

void CommandPanel::onExportClicked()
{
    QList<CommandSet> sets = CommandManager::instance()->allSets();
    if (sets.isEmpty()) {
        StyleDialogs::information(this, tr("没有可导出的分组"), tr("当前没有任何指令集"));
        return;
    }

    // 先把界面上还没落模型的最后一次编辑同步进来，否则导出的会少这一下改动
    saveCurrentSet();
    sets = CommandManager::instance()->allSets();

    ExportGroupsDialog dialog(sets, this);
    if (dialog.exec() != QDialog::Accepted) return;

    QList<CommandSet> selected = dialog.selectedSets();
    if (selected.isEmpty()) {
        StyleDialogs::warning(this, tr("未选择分组"), tr("请至少勾选一个分组"));
        return;
    }

    const CommandFormats::Format format = dialog.format();
    const QString defaultName = QStringLiteral("commands.%1").arg(CommandFormats::suffix(format));

    QString filePath = StyleDialogs::getSaveFileName(this, tr("导出指令集"),
                                                    defaultName,
                                                    CommandFormats::saveFilter(format));
    if (filePath.isEmpty()) return;

    QString errorMessage;
    if (CommandManager::instance()->exportToFile(filePath, format, selected, errorMessage)) {
        StyleDialogs::information(this, tr("导出成功"),
                                 tr("指令集已导出到:\n%1").arg(filePath));
    } else {
        StyleDialogs::critical(this, tr("导出失败"), errorMessage);
    }
}

void CommandPanel::onCommandsChanged()
{
    saveCurrentSet();
    savePreferences();
}

void CommandPanel::onSendCommand(const CommandItem &cmd)
{
    // 只负责"按用户填的延时等一拍再发"，怎么发、发去哪由组合根决定：
    // 编码与结尾符在 MainWindow::handleCommandSend 里按统一规则处理，目标取发送区
    // 当前选择。命令面板不碰字节、不碰目标——它是"内容与时机"的来源，仅此而已。
    QTimer::singleShot(cmd.delayMs, this, [this, cmd] { emit sendCommand(cmd); });
}

void CommandPanel::onBatchSendRequested(const QList<int> &rows, const QList<CommandItem> &commands)
{
    // 批量发送，累加延时（第 n 条在第 n 条之前所有延时之和后发出）。
    //
    // **进度与发送在同一拍里推进**：定时器回调里先点亮该行的游标，再发出去。
    // 这是唯一的时序来源——若让表格另起一套定时器去"猜"进度，两套时钟迟早错开，
    // 而高亮跟实际发送不同步比没有高亮更让人困惑。
    constexpr int RunTailHoldMs = 700; // 最后一拍发完后再留一会儿，让收尾可见

    m_commandTable->beginSendRun();

    int totalDelay = 0;
    for (int i = 0; i < commands.size(); ++i) {
        totalDelay += commands.at(i).delayMs;
        const int row = rows.value(i, -1);
        QTimer::singleShot(totalDelay, this, [this, command = commands.at(i), row] {
            m_commandTable->setSendCursor(row);
            emit sendCommand(command);
        });
    }

    // 一轮结束：收掉游标，但**保留已发送的绿色勾**——用户要能一眼看出这一轮覆盖了
    // 哪几条（这正是"表示目前发送到哪条指令了"的另一半）。
    QTimer::singleShot(totalDelay + RunTailHoldMs, this,
        [this] { m_commandTable->endSendRun(); });
}

void CommandPanel::refreshSetComboBox()
{
    // 重入保护：refresh → currentIndexChanged → 回写 → setsChanged → refresh 会无限递归
    if (m_updatingSets) return;
    m_updatingSets = true;

    // clear() 把 currentIndex 打回 -1、随后 addItem() 又变回 0，整段重建产生的都是
    // 伪变更。屏蔽信号保证"模型 → UI"单向流动：重建期间绝不回写模型。
    const QSignalBlocker blocker(m_setComboBox);

    const QUuid selectedId = m_currentSetId.isNull()
                                 ? m_setComboBox->currentData().toUuid()
                                 : m_currentSetId;

    m_setComboBox->clear();

    QList<CommandSet> sets = CommandManager::instance()->allSets();
    for (const auto &set : sets) {
        m_setComboBox->addItem(set.name, set.id.toString());
    }

    // 恢复选中
    int indexToSelect = -1;
    for (int i = 0; i < m_setComboBox->count(); ++i) {
        if (m_setComboBox->itemData(i).toUuid() == selectedId) {
            indexToSelect = i;
            break;
        }
    }
    // 原选中项可能已被删除（removeSet / undo），退回第一项
    if (indexToSelect < 0 && m_setComboBox->count() > 0) indexToSelect = 0;
    m_setComboBox->setCurrentIndex(indexToSelect);

    m_removeSetButton->setEnabled(m_setComboBox->count() > 1);

    // 下拉框的选中项可能已被外部改变（撤销/删除/导入/加载），把模型里的指令集灌回表格。
    // 表格只是模型的镜像，这一步只灌数据、绝不反向回写。
    const QUuid idToLoad = indexToSelect >= 0
                               ? m_setComboBox->itemData(indexToSelect).toUuid()
                               : QUuid();
    m_currentSetId = idToLoad;
    const CommandSet currentSet = idToLoad.isNull()
                                      ? CommandSet()
                                      : CommandManager::instance()->getSet(idToLoad);
    m_commandTable->setCommandSet(currentSet);

    m_updatingSets = false;
}

void CommandPanel::saveCurrentSet()
{
    if (m_currentSetId.isNull()) return;

    CommandSet set = m_commandTable->commandSet();
    set.id = m_currentSetId;

    // 保留原来的名称和备注
    CommandSet oldSet = CommandManager::instance()->getSet(m_currentSetId);
    set.name = oldSet.name;
    set.notes = oldSet.notes;

    // 静默回写：只改内存、不压撤销快照、不广播 setsChanged。
    //
    // 这条路径每一次行内编辑都会走到（勾选 / 改延时 / 选结尾符 / 改内容），所以
    // "静默"是硬要求而不是优化：一旦广播，命令面板会重建下拉框 → 表格 setCommandSet
    // → refreshTable 销毁行内控件，而其中就有**正在派发这次信号的那个控件**——
    // 用户报告的"拖拽崩""选结尾符崩"正是这条链的 use-after-free。
    // 表格是模型的镜像，值由这里的 set 主动推过去，模型不反向通知。
    CommandManager::instance()->syncSet(m_currentSetId, set);
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

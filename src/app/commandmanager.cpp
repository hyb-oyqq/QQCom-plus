#include "commandmanager.h"
#include "persistence/preferencesstore.h"
#include <QJsonArray>
#include <utility>

CommandManager::CommandManager(QObject *parent) : QObject(parent)
{
}

CommandManager* CommandManager::instance()
{
    static CommandManager manager;
    return &manager;
}

CommandSet CommandManager::getSet(const QUuid &id) const
{
    for (const auto &set : m_sets) {
        if (set.id == id) return set;
    }
    return CommandSet();
}

void CommandManager::addSet(const CommandSet &set)
{
    pushSnapshot();
    m_sets.append(set);
    emit setsChanged();
}

void CommandManager::updateSet(const QUuid &id, const CommandSet &set, bool pushUndo)
{
    for (int i = 0; i < m_sets.size(); ++i) {
        if (m_sets[i].id == id) {
            if (pushUndo) pushSnapshot();
            m_sets[i] = set;
            emit setsChanged();
            return;
        }
    }
}

void CommandManager::removeSet(const QUuid &id)
{
    for (int i = 0; i < m_sets.size(); ++i) {
        if (m_sets[i].id == id) {
            pushSnapshot();
            m_sets.removeAt(i);
            emit setsChanged();
            return;
        }
    }
}

void CommandManager::load(PreferencesStore *store)
{
    if (!store) return;

    QJsonArray array = store->value("commandSets").toJsonArray();
    m_undoStack.clear();
    m_redoStack.clear();
    m_sets.clear();
    for (const auto &value : array) {
        m_sets.append(CommandSet::fromJson(value.toObject()));
    }

    // 如果没有任何指令集，创建一个默认的
    if (m_sets.isEmpty()) {
        CommandSet defaultSet(tr("默认指令集"));
        m_sets.append(defaultSet);
    }

    emit setsChanged();
    emit undoRedoChanged();
}

void CommandManager::save(PreferencesStore *store)
{
    if (!store) return;

    QJsonArray array;
    for (const auto &set : m_sets) {
        array.append(set.toJson());
    }
    store->setValue("commandSets", array);
}

bool CommandManager::exportToFile(const QString &filePath, const CommandFormats::Format format,
                                 const QList<CommandSet> &sets, QString &errorMessage)
{
    return CommandFormats::writeFile(filePath, format, sets, errorMessage);
}

bool CommandManager::importFromFile(const QString &filePath, QString &errorMessage, QString *detectedLabel)
{
    QList<CommandSet> sets;
    CommandFormats::Format format;
    if (!CommandFormats::parseFile(filePath, sets, errorMessage, &format)) return false;
    if (detectedLabel) *detectedLabel = CommandFormats::label(format);
    if (sets.isEmpty()) return true;

    pushSnapshot();
    for (auto &set : sets) {
        // 分组和命令都创建新身份，重复导入不与已有条目冲突。
        set.id = QUuid::createUuid();
        for (auto &command : set.commands) command.id = QUuid::createUuid();
    }
    m_sets.append(std::move(sets));

    emit setsChanged();
    return true;
}

void CommandManager::pushSnapshot()
{
    // 保存当前状态到撤销栈
    m_undoStack.push(m_sets);
    if (m_undoStack.size() > MaxUndoSteps) {
        m_undoStack.removeFirst();
    }
    // 一旦有新操作，清空重做栈
    m_redoStack.clear();
    emit undoRedoChanged();
}

void CommandManager::undo()
{
    if (m_undoStack.isEmpty()) return;

    // 当前状态进入重做栈
    m_redoStack.push(m_sets);
    // 恢复上一个状态
    m_sets = m_undoStack.pop();
    emit setsChanged();
    emit undoRedoChanged();
}

void CommandManager::redo()
{
    if (m_redoStack.isEmpty()) return;

    // 当前状态进入撤销栈
    m_undoStack.push(m_sets);
    // 恢复重做状态
    m_sets = m_redoStack.pop();
    emit setsChanged();
    emit undoRedoChanged();
}

void CommandManager::syncSet(const QUuid &id, const CommandSet &set)
{
    // 静默更新：只改内存里的那一份，不压快照、**不广播 setsChanged**。
    //
    // 这里必须彻底静默，不能转手调用 updateSet()——后者无条件 emit setsChanged()，
    // 而调用方 CommandPanel::saveCurrentSet() 位于"表格编辑 → 落库"这条**最高频**的
    // 路径上（勾选、改延时、选结尾符、改内容都会走到）。广播的实际后果是：
    //   setsChanged → CommandPanel::refreshSetComboBox() → CommandTableWidget::setCommandSet()
    //   → refreshTable() → 销毁整表行内控件，
    // 而此刻正在派发信号的，恰恰就是其中一个控件（例如用户刚点选的结尾符下拉框）。
    // 两个用户可复现的崩溃——"拖动崩"与"选择换行符崩"——都出在这条链的末尾
    // （use-after-free），"快捷指令列表越用越卡"也出在这里（每次编辑都全表重建）。
    // 表格是模型的镜像：编辑后的值由 saveCurrentSet 主动推给模型，模型不需要
    // 再反过来通知一次"你改了"。
    for (auto &existing : m_sets) {
        if (existing.id == id) {
            existing = set;
            return;
        }
    }
}

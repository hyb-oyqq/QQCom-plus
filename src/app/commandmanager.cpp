#include "commandmanager.h"
#include "persistence/preferencesstore.h"
#include <QJsonArray>
#include <QSet>
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
    CommandSet missing;
    missing.id = QUuid();
    return missing;
}

void CommandManager::addSet(const CommandSet &set)
{
    pushSnapshot();
    m_sets.append(set);
    m_serializedSets.remove(set.id);
    emit setsChanged();
}

void CommandManager::updateSet(const QUuid &id, const CommandSet &set, bool pushUndo)
{
    for (int i = 0; i < m_sets.size(); ++i) {
        if (m_sets[i].id == id) {
            if (pushUndo) pushSnapshot();
            m_sets[i] = set;
            m_serializedSets.remove(id);
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
            m_serializedSets.remove(id);
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
    m_serializedSets.clear();
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
    QSet<QUuid> serializedIds;
    for (const auto &set : m_sets) {
        // 旧配置可能带重复 UUID；缓存不能把后面的分组覆盖成前一组。
        if (serializedIds.contains(set.id)) {
            array.append(set.toJson());
            continue;
        }
        serializedIds.insert(set.id);
        auto cached = m_serializedSets.constFind(set.id);
        if (cached == m_serializedSets.cend())
            cached = m_serializedSets.insert(set.id, set.toJson());
        array.append(cached.value());
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
    m_serializedSets.clear();

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
    m_serializedSets.clear();
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
    m_serializedSets.clear();
    emit setsChanged();
    emit undoRedoChanged();
}

void CommandManager::syncSet(const QUuid &id, const CommandSet &set)
{
    // 行内编辑只更新数据；广播会在信号派发期间反向重建表格。
    for (auto &existing : m_sets) {
        if (existing.id == id) {
            if (existing.name == set.name && existing.notes == set.notes
                && existing.commands == set.commands) return;
            existing = set;
            m_serializedSets.remove(id);
            return;
        }
    }
}

bool CommandManager::moveCommands(const QUuid &sourceId, const QUuid &targetId, const QList<int> &rows)
{
    if (rows.isEmpty() || sourceId == targetId) return false;
    int sourceIndex = -1;
    int targetIndex = -1;
    for (int i = 0; i < m_sets.size(); ++i) {
        if (m_sets.at(i).id == sourceId) {
            if (sourceIndex >= 0) return false;
            sourceIndex = i;
        }
        if (m_sets.at(i).id == targetId) {
            if (targetIndex >= 0) return false;
            targetIndex = i;
        }
    }
    if (sourceIndex < 0 || targetIndex < 0) return false;
    const auto &commands = m_sets.at(sourceIndex).commands;
    QSet<int> moving;
    for (int row : rows) {
        if (row < 0 || row >= commands.size()) return false;
        moving.insert(row);
    }
    auto source = m_sets.at(sourceIndex);
    auto target = m_sets.at(targetIndex);
    source.commands.clear();
    source.commands.reserve(commands.size() - moving.size());
    target.commands.reserve(target.commands.size() + moving.size());
    for (int row = 0; row < commands.size(); ++row) {
        if (moving.contains(row)) target.commands.append(commands.at(row));
        else source.commands.append(commands.at(row));
    }
    pushSnapshot();
    m_sets[sourceIndex] = std::move(source);
    m_sets[targetIndex] = std::move(target);
    m_serializedSets.remove(sourceId);
    m_serializedSets.remove(targetId);
    emit setsChanged();
    return true;
}

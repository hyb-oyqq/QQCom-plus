#include "commandmanager.h"
#include "persistence/preferencesstore.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

CommandManager *CommandManager::s_instance = nullptr;

CommandManager::CommandManager(QObject *parent) : QObject(parent)
{
}

CommandManager* CommandManager::instance()
{
    if (!s_instance) {
        s_instance = new CommandManager();
    }
    return s_instance;
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
    if (pushUndo) {
        pushSnapshot();
    }
    for (int i = 0; i < m_sets.size(); ++i) {
        if (m_sets[i].id == id) {
            m_sets[i] = set;
            emit setsChanged();
            return;
        }
    }
}

void CommandManager::removeSet(const QUuid &id)
{
    pushSnapshot();
    for (int i = 0; i < m_sets.size(); ++i) {
        if (m_sets[i].id == id) {
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
                                 const QList<CommandSet> &sets, const QString &errorMessage)
{
    Q_UNUSED(format);  // 当前只支持 JSON 格式
    Q_UNUSED(errorMessage);  // 预留用于错误消息

    QJsonArray array;
    for (const auto &set : sets) {
        array.append(set.toJson());
    }

    QJsonObject root;
    root["version"] = "1.0";
    root["commandSets"] = array;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

bool CommandManager::importFromFile(const QString &filePath, QString &errorMessage, QString *importedSetName)
{
    Q_UNUSED(importedSetName);  // 预留用于返回导入的集合名称

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errorMessage = tr("无法打开文件");
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (doc.isNull()) {
        errorMessage = tr("JSON 解析失败: %1").arg(parseError.errorString());
        return false;
    }

    QJsonObject root = doc.object();
    if (!root.contains("commandSets")) {
        errorMessage = tr("无效的指令集文件格式");
        return false;
    }

    pushSnapshot();

    QJsonArray array = root["commandSets"].toArray();
    for (const auto &value : array) {
        CommandSet set = CommandSet::fromJson(value.toObject());
        // 导入时生成新ID，避免冲突
        set.id = QUuid::createUuid();
        m_sets.append(set);
    }

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
    // 静默更新，不触发撤销栈
    updateSet(id, set, false);
}

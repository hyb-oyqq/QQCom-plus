#ifndef COMMANDMANAGER_H
#define COMMANDMANAGER_H

#include "domain/commandset.h"
#include "commandformats.h"
#include <QObject>
#include <QList>
#include <QStack>

class PreferencesStore;

// 指令集管理器：负责多个指令集的增删改查、持久化、导入导出、撤销重做。
// 单例模式，全局共享，UI层通过信号监听变化。
class CommandManager : public QObject
{
    Q_OBJECT

public:
    static CommandManager* instance();

    // 指令集管理
    QList<CommandSet> allSets() const { return m_sets; }
    CommandSet getSet(const QUuid &id) const;
    void addSet(const CommandSet &set);
    void updateSet(const QUuid &id, const CommandSet &set, bool pushUndo = false);
    void syncSet(const QUuid &id, const CommandSet &set);
    void removeSet(const QUuid &id);
    int setCount() const { return m_sets.size(); }

    // 持久化
    void load(PreferencesStore *store);
    void save(PreferencesStore *store);

    // 导入导出
    bool exportToFile(const QString &filePath, const CommandFormats::Format format,
                     const QList<CommandSet> &sets, QString &errorMessage);
    bool importFromFile(const QString &filePath, QString &errorMessage, QString *detectedLabel = nullptr);

    // 撤销重做（操作快照）
    void pushSnapshot();
    void undo();
    void redo();
    bool canUndo() const { return !m_undoStack.isEmpty(); }
    bool canRedo() const { return !m_redoStack.isEmpty(); }

signals:
    void setsChanged();
    void undoRedoChanged();

private:
    explicit CommandManager(QObject *parent = nullptr);

    QList<CommandSet> m_sets;
    QStack<QList<CommandSet>> m_undoStack;  // 最多10步
    QStack<QList<CommandSet>> m_redoStack;
    static constexpr int MaxUndoSteps = 10;
};

#endif

#ifndef COMMANDTABLEWIDGET_H
#define COMMANDTABLEWIDGET_H

#include "domain/commandset.h"
#include <QWidget>

class QTableWidget;
class QPushButton;
class QTimer;

// 指令表格：显示和编辑单个指令集的所有指令。
// 支持添加、删除、编辑、拖拽排序、单条发送、批量发送。
class CommandTableWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CommandTableWidget(QWidget *parent = nullptr);

    // 加载指令集
    void setCommandSet(const CommandSet &set);
    CommandSet commandSet() const;

    // 获取选中的指令
    QList<CommandItem> selectedCommands() const;
    QList<CommandItem> enabledCommands() const;

signals:
    void commandsChanged();
    void sendCommand(const CommandItem &cmd, int targetEndpoint); // 0=A, 1=B, 2=AB
    void batchSendRequested(const QList<CommandItem> &commands, int targetEndpoint);

private slots:
    void addCommand();
    void removeCommand();
    void onCellChanged(int row, int column);
    void onSendClicked();

private:
    void setupUi();
    void refreshTable();
    void updateRow(int row, const CommandItem &cmd);
    CommandItem commandFromRow(int row) const;

    QTableWidget *m_table = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_batchSendButton = nullptr;

    CommandSet m_currentSet;
    bool m_updating = false; // 防止递归更新
};

#endif

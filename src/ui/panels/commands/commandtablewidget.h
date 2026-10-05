#ifndef COMMANDTABLEWIDGET_H
#define COMMANDTABLEWIDGET_H

#include "domain/commandset.h"
#include <QWidget>

class QTableWidget;
class QPushButton;
class QTimer;
class QSpinBox;
class QComboBox;

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
    void onCustomContextMenuRequested(const QPoint &pos);

public:
    void refreshColumnWidths();

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupUi();
    void refreshTable();
    void updateRow(int row, const CommandItem &cmd);
    CommandItem commandFromRow(int row) const;

    // 行操作
    int rowCount() const;
    bool isRowChecked(int row) const;
    QList<int> checkedRows() const;
    void copyRows(const QList<int> &rows);
    void removeRows(const QList<int> &rows);
    void moveRow(int from, int to);
    bool confirmRemoval(int count);
    void renameRow(int row);

    // 列宽管理
    void updateSendColumnWidth();
    void updateTailColumnWidth();

    // 几何同步
    void syncCellWidgetGeometry();
    QWidget *wrapCentered(QWidget *widget);

    // 拖拽指示器
    void showDropIndicator(int insertAt);
    void hideDropIndicator();
    void setDragRowDimmed(int row, bool dimmed);

    // 辅助获取控件
    QSpinBox *delaySpinBoxAt(int row) const;
    QComboBox *tailComboBoxAt(int row) const;
    QPushButton *sendButtonAt(int row) const;

    QTableWidget *m_table = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_batchSendButton = nullptr;
    QWidget *m_dropIndicator = nullptr;

    CommandSet m_currentSet;
    bool m_updating = false; // 防止递归更新
    bool m_syncingGeometry = false; // 防止递归同步几何

    // 拖拽状态
    int m_dragRow = -1;
    int m_dropAt = -1;
};

#endif

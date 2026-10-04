#ifndef COMMANDPANEL_H
#define COMMANDPANEL_H

#include "domain/commanditem.h"
#include <QWidget>
#include <QUuid>

class QComboBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTabWidget;
class QTimer;
class CommandTableWidget;
class PreferencesStore;

// 指令面板：右侧可展开/收起的面板，包含指令集管理、周期发送、导入导出。
// 集成多个标签页：初始化、周期发送、解析(HEX)、解析(TXT)。
class CommandPanel : public QWidget
{
    Q_OBJECT

public:
    explicit CommandPanel(QWidget *parent = nullptr);

    // 设置持久化存储
    void setPreferencesStore(PreferencesStore *store);

    // 展开/收起
    bool isExpanded() const { return m_expanded; }
    void setExpanded(bool expanded);

signals:
    void sendCommand(const QString &content, bool isHex, int targetEndpoint);
    void batchSendRequested(const QStringList &commands, const QList<int> &delays, int targetEndpoint);

private slots:
    void onCurrentSetChanged(int index);
    void onAddSetClicked();
    void onRemoveSetClicked();
    void onNotesClicked();
    void onUndoClicked();
    void onRedoClicked();
    void onImportClicked();
    void onExportClicked();
    void onCommandsChanged();
    void onSendCommand(const CommandItem &cmd, int targetEndpoint);
    void onBatchSendRequested(const QList<CommandItem> &commands, int targetEndpoint);
    void onPeriodicSendTimeout();
    void refreshSetComboBox();
    void refreshUndoRedoButtons();

private:
    void setupUi();
    void saveCurrentSet();
    void loadPreferences();
    void savePreferences();

    QComboBox *m_setComboBox = nullptr;
    QPushButton *m_addSetButton = nullptr;
    QPushButton *m_removeSetButton = nullptr;
    QPushButton *m_notesButton = nullptr;
    QPushButton *m_undoButton = nullptr;
    QPushButton *m_redoButton = nullptr;
    QPushButton *m_importButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_expandButton = nullptr;

    QTabWidget *m_tabs = nullptr;
    CommandTableWidget *m_initTable = nullptr;
    CommandTableWidget *m_periodicTable = nullptr;

    QTimer *m_periodicTimer = nullptr;
    QSpinBox *m_periodicInterval = nullptr;
    QPushButton *m_startPeriodicButton = nullptr;

    PreferencesStore *m_preferences = nullptr;
    bool m_expanded = true;
    QUuid m_currentSetId;
};

#endif

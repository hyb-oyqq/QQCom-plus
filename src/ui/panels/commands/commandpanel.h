#ifndef COMMANDPANEL_H
#define COMMANDPANEL_H

#include "domain/commanditem.h"
#include <QList>
#include <QWidget>
#include <QUuid>

class QComboBox;
class QPushButton;
class QVBoxLayout;
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
    // 展开时"内容完整可见"所需的最小宽度。主窗口用它算窗口最小宽度：
    // 窗口放不下两侧面板的最小宽度时，应该变大窗口而不是压缩内容（用户钦定）。
    int expandedMinimumWidth() const { return m_expandedMinimumWidth; }

signals:
    // 把一条指令交给组合根去发。**不带目标参数**：目标是"发送区当前选中"这一件事，
    // 由发送区唯一持有（见 SendPanel::targetMask）。这里再多一个目标参数就等于
    // 给同一件事留了第二个来源，迟早出现"快捷按钮发错了口"。
    void sendCommand(const CommandItem &command);
    void expandedChanged(bool expanded);  // 展开/折叠状态改变时发出

private slots:
    void onCurrentSetChanged(int index);
    void onAddSetClicked();
    void onRemoveSetClicked();
    void onRenameSetClicked();
    void onImportClicked();
    void onExportClicked();
    void onContentWrapToggled(bool wrap);
    void onCommandsChanged();
    void onSendCommand(const CommandItem &cmd);
    // rows 与 commands 一一对应且同序：时序与进度都在这一个函数里排，两边不会错开。
    // loop 为真时这一轮的最后一条发完后，再排同样的一轮。
    void onBatchSendRequested(const QList<int> &rows, const QList<CommandItem> &commands, bool loop);
    void onBatchSendStopRequested();
    void refreshSetComboBox();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void setupUi();
    void saveCurrentSet();
    void loadPreferences();
    void savePreferences();
    void applyExpandedState();
    void updateMargins();
    // 按累加延时排一轮。loop 且这一轮还没被取消时，最后一条发出后再排下一轮。
    void scheduleBatch(const QList<int> &rows, const QList<CommandItem> &commands, bool loop);
    void setBatchButtonLooping(bool looping);

    QVBoxLayout *m_mainLayout = nullptr;
    QWidget *m_topBarWidget = nullptr;  // 顶部工具栏容器，用于整体显示/隐藏
    QComboBox *m_setComboBox = nullptr;
    QPushButton *m_addSetButton = nullptr;
    QPushButton *m_removeSetButton = nullptr;
    QPushButton *m_renameSetButton = nullptr;
    QPushButton *m_importButton = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_wrapButton = nullptr;

    CommandTableWidget *m_commandTable = nullptr;


    PreferencesStore *m_preferences = nullptr;
    // 没有保存过展开状态时收起。构造时先落到几何上，偏好读到后再改。
    bool m_expanded = false;
    bool m_updatingSets = false;
    // 恢复偏好期间挡住 savePreferences。setExpanded / 换行 / 切分组都会顺手保存，
    // 中间状态若写出去，会把已经存过的分组盖成第一组。
    bool m_loading = false;
    QUuid m_currentSetId;
    // 展开时的最小宽度与两项宽度来源（顶栏 / 表格内容）：
    // 窗口不够宽时，这些值会被主窗口并入"窗口最小宽度"的计算，宁可把窗口撑开，
    // 也不让"分组"下拉与末尾的手柄列被裁掉（用户反馈）。
    int m_topBarMinimumWidth = 0;
    int m_commandTableMinimumWidth = 0;
    int m_expandedMinimumWidth = 0;
    // 循环发送的代数。每次新开一轮或用户停止都加一，旧定时器看到代数不对就不再发、不再续轮。
    int m_batchGeneration = 0;
};

#endif

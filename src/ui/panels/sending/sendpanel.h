#ifndef SENDPANEL_H
#define SENDPANEL_H

#include <QWidget>
#include "domain/portdraft.h"
#include "domain/trafficmessage.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class PreferencesStore;
class EndpointRuntime;
class FileTransferWidget;
class QPushButton;
class QResizeEvent;
class QSpinBox;
class QTabWidget;
class QTimer;

// 单个发送区：目标用 A/B/AB 三选一的可勾选按钮表达（与数据显示区端点开关同款样式），
// 正文固定约三行，格式/末尾贴在正文下方。格式列表与数据显示区共用 PayloadCodec。
// 发送内容经 PayloadCodec 严格解析后整体发出；目标端点的连接状态由组合根注入。
class SendPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SendPanel(QWidget *parent = nullptr);
    void setPorts(const QList<PortDraft> &ports);
    // 端点连接状态：发送按钮只在"选中目标里有已连接的"时可用。
    void setEndpointOpen(int index, bool open);
    void setPayload(const QByteArray &bytes);
    // 组合根注入的偏好存储：编码格式与行尾在会话间保持用户上次的选择。
    void setPreferences(PreferencesStore *store);
    void setRuntime(EndpointRuntime *runtime);
    void handleRawDataReceived(int index, const Traffic::Source &source, const QByteArray &payload);
    // 当前标签页内容撑开所需的高度。文件页比手动发送高，分栏按当前页重新分配。
    int preferredHeight() const;

    // 发送目标的**唯一来源**：位 1 = A，位 2 = B（都选中 = 3）。
    // 旁路发送（指令面板的单点 / 批量发送）必须取它，不得自己另定一套目标规则——
    // 否则会出现"同一条指令，手点「发送」能到 B，快捷按钮却发去了 A"。
    int targetMask() const;
    // 旁路发送入口：把一段**已经准备好的原始字节**按发送区当前选中的目标发出去。
    // 存在意义是让指令面板与手点「发送」共用同一条路（同一个 sendRequested 信号、
    // 同一套目标规则、同一个发送来源标记），而不是自己也去调一次 runtime——
    // 早先指令面板直接 invokeMethod(runtime, "write")，而 EndpointRuntime **没有
    // write 这个槽**，于是"快捷面板点了没反应"，且没有任何报错。
    // 不做"按钮可用"这道 UI 闸：目标端口没开时应当产生一条可见的失败记录，
    // 而不是静默什么都不做（反馈比沉默重要）。
    void deliverPayload(const QByteArray &payload);

signals:
    // payload 已按当前格式/末尾解析为原始字节；targetMask 位 1/2 表示 A/B。
    // 定时发送的每一拍也走同一个信号，路径与手动发送完全一致。
    // scheduled 参数表示是否为定时发送（false 为手动发送）
    void sendRequested(const QByteArray &payload, int targetMask, bool scheduled = false);
    void preferredHeightChanged(int height);

private:
    void updatePreview();
    void updateTargets();
    void updateSendAvailability();
    // 目标 A/B/AB 与「发送」四个按钮取同一个宽度（跟随其中 sizeHint 最宽者）。
    // 不写死像素：字体/DPI/翻译变化时重算即可自动跟随（见 buttonmetrics.h）。
    void unifyActionWidths();
    void issueSend(bool scheduled = false);
    void loadPreferences();
    void savePreferences();

protected:
    void resizeEvent(QResizeEvent *event) override;

    QList<PortDraft> m_ports;
    QPushButton *m_targetA;
    QPushButton *m_targetB;
    QPushButton *m_targetBoth;
    QComboBox *m_format;
    QComboBox *m_suffix;
    QPlainTextEdit *m_editor;
    QLabel *m_preview;
    bool m_previewError = false;
    QPushButton *m_sendButton;
    QLabel *m_connectionHint;
    QCheckBox *m_repeat;
    QSpinBox *m_interval;
    QSpinBox *m_times;
    QTimer *m_repeatTimer;
    QTabWidget *m_tabs = nullptr;
    FileTransferWidget *m_fileTransfer = nullptr;
    int m_remainingTicks = 0;      // 定时发送剩余次数；0 = 持续
    bool m_openFlags[2] = {false, false};
    bool m_payloadValid = false;
    QByteArray m_payloadBytes;
    PreferencesStore *m_preferences = nullptr;
    bool m_loading = false; // 加载偏好期间联动触发的保存要抑制，与 TrafficPanel 同一模式
};

#endif

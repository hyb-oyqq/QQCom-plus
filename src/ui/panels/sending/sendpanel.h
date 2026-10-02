#ifndef SENDPANEL_H
#define SENDPANEL_H

#include <QWidget>
#include "domain/portdraft.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPlainTextEdit;
class PreferencesStore;
class QPushButton;
class QSpinBox;
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

signals:
    // payload 已按当前格式/末尾解析为原始字节；targetMask 位 1/2 表示 A/B。
    // 定时发送的每一拍也走同一个信号，路径与手动发送完全一致。
    void sendRequested(const QByteArray &payload, int targetMask);

private:
    void updatePreview();
    void updateTargets();
    void updateSendAvailability();
    void issueSend();
    void loadPreferences();
    void savePreferences();

    QList<PortDraft> m_ports;
    QPushButton *m_targetA;
    QPushButton *m_targetB;
    QPushButton *m_targetBoth;
    QComboBox *m_format;
    QComboBox *m_suffix;
    QPlainTextEdit *m_editor;
    QLabel *m_preview;
    bool m_previewError = false;
    QLabel *m_length;
    QPushButton *m_sendButton;
    QLabel *m_connectionHint;
    QCheckBox *m_repeat;
    QSpinBox *m_interval;
    QSpinBox *m_times;
    QTimer *m_repeatTimer;
    int m_remainingTicks = 0;      // 定时发送剩余次数；0 = 持续
    bool m_openFlags[2] = {false, false};
    bool m_payloadValid = false;
    QByteArray m_payloadBytes;
    PreferencesStore *m_preferences = nullptr;
    bool m_loading = false; // 加载偏好期间联动触发的保存要抑制，与 TrafficPanel 同一模式
};

#endif

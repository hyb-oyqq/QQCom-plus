#ifndef SENDPANEL_H
#define SENDPANEL_H

#include "../portselector/portselectorwidget.h"

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

// 单个发送区：目标用 A/B/AB 三选一的可勾选按钮表达（与数据显示区端点开关同款样式），
// 正文固定约三行，格式/末尾贴在正文下方。格式列表与数据显示区共用 PayloadCodec。
// 快捷命令页已移除，后续作为右侧可展开面板回归，因此这里不再持有命令列表。
class SendPanel : public QWidget
{
    Q_OBJECT
public:
    explicit SendPanel(QWidget *parent = nullptr);
    void setPorts(const QList<PortDraft> &ports);
    void setPayload(const QByteArray &bytes);
private:
    void updatePreview();
    void updateTargets();
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
};

#endif

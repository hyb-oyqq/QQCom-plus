#ifndef PORTSELECTORWIDGET_H
#define PORTSELECTORWIDGET_H

#include <QIcon>
#include <QWidget>
#include <QSet>
#include "domain/portdraft.h"
#include "domain/serialportdescriptor.h"

class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QToolButton;

// 端点标识与草稿摘要的格式化在 ui/common/endpointformat.h；
// 下拉框内容自适应在 ui/common/combometrics.h，面板间共用，不必 include 本控件。

// One endpoint per horizontal row: badge, type, inline parameters, state and action.
class PortSelectorWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PortSelectorWidget(QWidget *parent = nullptr);
    void setDraft(const PortDraft &draft);
    PortDraft draft() const;
    bool hasValidConfiguration() const { return m_validationError.isEmpty(); }
    QString validationError() const { return m_validationError; }
    void setEndpointLabel(const QString &label);
    // Set once a transport reports its state; the action button reflects it.
    // 打开期间锁定类型与参数，避免"边连边改"造成配置与运行态不一致。
    void setConnectionOpen(bool open);
    void setSerialPorts(const QList<SerialPortDescriptor> &ports);
    void setAddressHistory(const QStringList &remote, const QStringList &local);
    void commitAddressHistory();
    // TCP 服务器客户端列表（显示用）；其他类型传空列表即可。
    void setPeerList(const QStringList &peers);

signals:
    void configurationChanged();
    void addressCommitted(const QString &address, bool localAddress);
    // 动作按钮被点击：打开/关闭/取消连接都发这一个信号，方向由上层根据
    // 运行状态决定——控件自己不持有通信状态机。
    void actionRequested();

private:
    void notifyChange();
    void updateValidation();
    void updateActionLabels();
    void updateAdvancedToggle();
    void selectSerialPort(const QString &portName);
    void rememberAddress(const QString &key);
    QComboBox *addAddress(const QString &key, bool allowHostName = false);
    QComboBox *addChoice(const QString &key, const QStringList &items, bool editable = false);
    QSpinBox *addNumber(const QString &key, int minimum, int maximum);
    PortDraft m_draft;
    bool m_loading = false;
    bool m_initialised = false;
    QLabel *m_badge;
    QComboBox *m_type;
    QStackedWidget *m_pages;
    QStackedWidget *m_advancedPages;
    QWidget *m_advancedPanel;
    QToolButton *m_advanced;
    QIcon m_advancedIconIdle;
    QIcon m_advancedIconActive;
    QPushButton *m_action;
    bool m_open = false;
    QString m_validationError;
    QMap<QString, QWidget *> m_fields;
    QComboBox *m_serialPorts;
    QList<SerialPortDescriptor> m_availablePorts;
    bool m_serialScanComplete = false;
    QSet<QString> m_editedAddresses;
    QLabel *m_peerHint = nullptr; // TCP 服务器高级参数页的客户端提示行
};

#endif

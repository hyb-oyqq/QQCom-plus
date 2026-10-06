#ifndef PORTSELECTORWIDGET_H
#define PORTSELECTORWIDGET_H

#include <QIcon>
#include <QWidget>
#include <QSet>
#include "domain/portdraft.h"
#include "domain/serialportdescriptor.h"

class QCheckBox;
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
    QWidget *buildFramingRow(); // 接收组帧：与端点类型无关，挂在高级参数区底部
    void syncFramingControls(); // 模式联动：只有结束符模式需要结束符输入
    void applyFramingToDraft(PortDraft &draft) const;
    QComboBox *m_framingMode = nullptr;
    QLineEdit *m_framingDelimiter = nullptr;
    QLabel *m_framingIdleLabel = nullptr;
    QLabel *m_framingIdleUnit = nullptr; // "ms" 单位标签：单位在框外，输入框只放数字
    QSpinBox *m_framingIdle = nullptr;
    QLabel *m_framingThresholdLabel = nullptr;
    QLabel *m_framingThresholdUnit = nullptr; // "字节" 单位标签：同上，跟随兜底设置一起置灰
    QSpinBox *m_framingThreshold = nullptr;
    QCheckBox *m_framingKeepBoundary = nullptr;
    QCheckBox *m_framingLogHints = nullptr;
    QLabel *m_framingHint = nullptr; // 组帧模式的白话说明（跟随模式切换）
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
    // 上次校验的错误字段：自动展开只响应"新出现"的错误，程序化载入的遗留错误不展开。
    QSet<QWidget *> m_lastInvalid;
    QMap<QString, QWidget *> m_fields;
    QComboBox *m_serialPorts;
    QList<SerialPortDescriptor> m_availablePorts;
    bool m_serialScanComplete = false;
    QSet<QString> m_editedAddresses;
    QLabel *m_peerHint = nullptr; // TCP 服务器高级参数页的客户端提示行
};

#endif

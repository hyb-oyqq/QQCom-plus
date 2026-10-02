#ifndef PORTSELECTORWIDGET_H
#define PORTSELECTORWIDGET_H

#include <QIcon>
#include <QWidget>
#include <QSet>
#include "../../../domain/portdraft.h"
#include "../../../domain/serialportdescriptor.h"

class QComboBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QToolButton;

QString portKindName(int kind);
// Endpoint identity is derived from the row position and type ("A · 串口"). It is
// displayed, never edited, so no configuration field holds a user-supplied name.
QString endpointLabel(int index, int kind);
QString portDraftSummary(const PortDraft &draft);
QString portDraftDetails(const PortDraft &draft);
QString portActionName(int kind, bool open);
QString portActionIconName(int kind, bool open);
void fitComboToItems(QComboBox *box);

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
    void setConnectionOpen(bool open);
    bool isConnectionOpen() const { return m_open; }
    void setSerialPorts(const QList<SerialPortDescriptor> &ports);
    void setAddressHistory(const QStringList &remote, const QStringList &local);
    void commitAddressHistory();

signals:
    void configurationChanged();
    void addressCommitted(const QString &address, bool localAddress);

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
};

#endif

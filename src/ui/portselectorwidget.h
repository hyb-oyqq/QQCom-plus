#ifndef PORTSELECTORWIDGET_H
#define PORTSELECTORWIDGET_H

#include <QVariantMap>
#include <QUuid>
#include <QWidget>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QStackedWidget;

// UI draft only; runtime transports and persisted configurations are separate.
struct PortDraft
{
    QUuid id = QUuid::createUuid();
    QString name;
    int kind = 0; // Serial, TCP client, TCP server, UDP
    QVariantMap values;
};

QString portKindName(int kind);
QString portDraftSummary(const PortDraft &draft);

class PortSelectorWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PortSelectorWidget(QWidget *parent = nullptr);
    void setDraft(const PortDraft &draft);
    PortDraft draft() const;

signals:
    void configurationChanged();

private:
    void notifyChange();
    QComboBox *addChoice(const QString &key, const QStringList &items, bool editable = false);
    QLineEdit *addText(const QString &key, const QString &placeholder);
    QSpinBox *addNumber(const QString &key, int minimum, int maximum);
    PortDraft m_draft;
    bool m_loading = false;
    QLineEdit *m_name;
    QComboBox *m_type;
    QStackedWidget *m_pages;
    QMap<QString, QWidget *> m_fields;
};

#endif

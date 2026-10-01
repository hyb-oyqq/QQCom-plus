#ifndef ROUTEEDITORWIDGET_H
#define ROUTEEDITORWIDGET_H

#include "portselectorwidget.h"

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

class RouteEditorWidget : public QWidget
{
    Q_OBJECT
public:
    explicit RouteEditorWidget(QWidget *parent = nullptr);
    void setPorts(const QList<PortDraft> &ports);
signals:
    void draftCountChanged(int count);
private:
    struct RuleDraft { QUuid source; QUuid target; bool duplex; QString targetMode; };
    void updateEditor();
    void rebuildTable();
    QList<PortDraft> m_ports;
    QList<RuleDraft> m_rules;
    QComboBox *m_source;
    QComboBox *m_target;
    QComboBox *m_direction;
    QComboBox *m_targetMode;
    QLabel *m_hint;
    QPushButton *m_add;
    QTableWidget *m_table;
};

#endif

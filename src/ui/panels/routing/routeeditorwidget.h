#ifndef ROUTEEDITORWIDGET_H
#define ROUTEEDITORWIDGET_H

#include <QWidget>
#include "domain/bridgelink.h"
#include "domain/portdraft.h"

class QComboBox;
class QPushButton;

// The endpoint pair is fixed to A/B, so direction and the start action are all that is left.
// The direction options name the pair themselves ("A → B"), which is why the panel keeps no
// title and no separate summary label.
class RouteEditorWidget : public QWidget
{
    Q_OBJECT
public:
    explicit RouteEditorWidget(QWidget *parent = nullptr);
    void setEndpoints(const QList<PortDraft> &ports, bool ready);
    BridgeLink link() const;
signals:
    void bridgeChanged(const BridgeLink &link);
private:
    void updateBridgeState();
    QString bridgeSummary() const;
    QList<PortDraft> m_ports;
    bool m_ready = false;
    QComboBox *m_direction;
    QPushButton *m_bridge;
    bool m_active = false;
};

#endif

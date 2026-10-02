#ifndef PORTDISCOVERYSERVICE_H
#define PORTDISCOVERYSERVICE_H

#include "../domain/serialportdescriptor.h"
#include <QFutureWatcher>
#include <QObject>

class PortDiscoveryService final : public QObject
{
    Q_OBJECT
public:
    explicit PortDiscoveryService(QObject *parent = nullptr);
    void refresh();

signals:
    void portsChanged(const QList<SerialPortDescriptor> &ports);
    void refreshingChanged(bool refreshing);

private:
    QFutureWatcher<QList<SerialPortDescriptor>> m_watcher;
    QList<SerialPortDescriptor> m_published;
    bool m_refreshing = false;
};

#endif

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
    // 每次扫描结束都发，列表没变也发。generation 从 1 递增，用来判断快照是否新于断线时刻。
    void scanFinished(quint64 generation, const QList<SerialPortDescriptor> &ports);
    void refreshingChanged(bool refreshing);

private:
    QFutureWatcher<QList<SerialPortDescriptor>> m_watcher;
    QList<SerialPortDescriptor> m_published;
    quint64 m_generation = 0;
    bool m_refreshing = false;
};

#endif

#include "portdiscoveryservice.h"

#include <QCollator>
#include <QSerialPortInfo>
#include <QtConcurrentRun>
#include <algorithm>

PortDiscoveryService::PortDiscoveryService(QObject *parent) : QObject(parent)
{
    connect(&m_watcher, &QFutureWatcher<QList<SerialPortDescriptor>>::finished, this, [this] {
        const auto ports = m_watcher.result();
        m_refreshing = false;
        ++m_generation;
        // 自动刷新时列表通常不变；只在快照真正变化时发布，避免重建下拉框打断浏览。
        if (ports != m_published) {
            m_published = ports;
            emit portsChanged(ports);
        }
        emit scanFinished(m_generation, ports);
        emit refreshingChanged(false);
    });
}

void PortDiscoveryService::refresh()
{
    if (m_refreshing) return;
    m_refreshing = true;
    emit refreshingChanged(true);
    // Capture no UI/service objects: an in-flight scan remains safe during shutdown.
    m_watcher.setFuture(QtConcurrent::run([] {
        QList<SerialPortDescriptor> ports;
        for (const auto &port : QSerialPortInfo::availablePorts())
            ports.append({port.portName(), port.description(), port.manufacturer(), port.systemLocation()});
        QCollator collator;
        collator.setNumericMode(true);
        std::sort(ports.begin(), ports.end(), [&collator](const auto &a, const auto &b) {
            return collator.compare(a.portName, b.portName) < 0;
        });
        return ports;
    }));
}

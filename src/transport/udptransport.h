#ifndef UDPTRANSPORT_H
#define UDPTRANSPORT_H

#include <QHostAddress>
#include "transport/abstracttransport.h"

class QUdpSocket;

// UDP 端点（合并参考工程的 Client/Server 两种形态）：一个 socket 同时
// 完成本地绑定与目标发送。接收保留数据报边界与来源地址；发送永远发往
// 配置里的目标地址，不会因为别的来源来包而悄悄改变方向。
class UdpTransport : public AbstractTransport
{
    Q_OBJECT
public:
    explicit UdpTransport(int endpointIndex, QObject *parent = nullptr);

    void start(const PortDraft &draft, quint64 generation) override;
    void stop() override;
    qint64 write(const QByteArray &payload, const QUuid &peerId) override;

private:
    void drainPendingDatagrams();
    QUdpSocket *m_socket = nullptr;
    QHostAddress m_targetAddress;
    quint16 m_targetPort = 0;
};

#endif

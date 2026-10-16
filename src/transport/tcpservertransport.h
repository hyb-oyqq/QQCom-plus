#ifndef TCPSERVERTRANSPORT_H
#define TCPSERVERTRANSPORT_H

#include <QHash>
#include "transport/abstracttransport.h"

class QTcpServer;
class QTcpSocket;

// TCP 服务器端点。监听同步完成；每个 accept 出来的客户端获得独立
// UUID 身份，按 peer 收发（peerId 为空 = 全部客户端），客户端上限
// 超出的连接立即拒绝，不做静默排队。
class TcpServerTransport : public AbstractTransport
{
    Q_OBJECT
public:
    explicit TcpServerTransport(int endpointIndex, QObject *parent = nullptr);

    void start(const PortDraft &draft, quint64 generation) override;
    void stop() override;
    qint64 write(const QByteArray &payload, const QUuid &peerId) override;
    qint64 discardPendingInput() override;
    QString writeAddress(const QUuid &peerId) const override;

private:
    void acceptPending();
    void drainPeer(QTcpSocket *socket);
    void detachPeer(QTcpSocket *socket);
    void publishPeers();

    struct Peer
    {
        QUuid id;
        QString address;
    };
    QTcpServer *m_server = nullptr;
    QHash<QTcpSocket *, Peer> m_peers;
    int m_maxClients = 64;
};

#endif

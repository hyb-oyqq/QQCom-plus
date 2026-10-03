#include "transport/tcpservertransport.h"

#include <QTcpServer>
#include <QTcpSocket>

namespace {
// 每客户端读缓冲上限，与串口/TCP 客户端一致：客户端只发不收时，
// 有限读缓冲让 TCP 背压停在内核，而不是在应用内存里悄悄堆积。
constexpr qint64 PeerReadBufferBytes = 256 * 1024;
} // namespace

TcpServerTransport::TcpServerTransport(int endpointIndex, QObject *parent)
    : AbstractTransport(parent)
{
    m_endpointIndex = endpointIndex;
}

void TcpServerTransport::start(const PortDraft &draft, quint64 generation)
{
    m_draft = draft;
    m_generation = generation;

    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    m_peers.clear();

    const auto addressText = draft.values.value(QStringLiteral("server.address"), "0.0.0.0").toString();
    const int port = draft.values.value(QStringLiteral("server.port"), 9000).toInt();
    m_maxClients = qBound(1, draft.values.value(QStringLiteral("server.clients"), 64).toInt(), 64);

    const QHostAddress address = addressText == QLatin1String("0.0.0.0")
        ? QHostAddress(QHostAddress::AnyIPv4) : QHostAddress(addressText);
    if (address.isNull()) {
        emit errorOccurred(m_generation, tr("无效的监听地址：%1").arg(addressText));
        emit stateChanged(m_generation, int(Transport::State::Closed), tr("打开失败"));
        return;
    }

    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &TcpServerTransport::acceptPending);
    if (!m_server->listen(address, quint16(port))) {
        emit errorOccurred(m_generation, tr("监听 %1:%2 失败：%3")
            .arg(addressText).arg(port).arg(m_server->errorString()));
        emit stateChanged(m_generation, int(Transport::State::Closed), tr("打开失败"));
        return;
    }
    emit stateChanged(m_generation, int(Transport::State::Open), tr("监听 %1:%2")
        .arg(m_server->serverAddress().toString())
        .arg(m_server->serverPort()));
}

void TcpServerTransport::stop()
{
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
        it.key()->disconnectFromHost();
        it.key()->deleteLater();
    }
    m_peers.clear();
    if (m_server)
        m_server->close();
}

qint64 TcpServerTransport::write(const QByteArray &payload, const QUuid &peerId)
{
    if (!m_server || !m_server->isListening())
        return -1;
    qint64 accepted = 0;
    if (!peerId.isNull()) {
        // 直接遍历匹配 peer：客户端数量有上限（≤64），代价可忽略。
        QTcpSocket *target = nullptr;
        for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it)
            if (it.value().id == peerId) { target = it.key(); break; }
        if (!target)
            return -1;
        accepted = target->write(payload);
    } else {
        for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it)
            accepted += qMax<qint64>(0, it.key()->write(payload));
    }
    return accepted;
}

void TcpServerTransport::acceptPending()
{
    while (m_server && m_server->hasPendingConnections()) {
        auto *socket = m_server->nextPendingConnection();
        // 改挂到传输对象上：nextPendingConnection 默认把 socket 认 QTcpServer 作父，
        // 而断开/stop 时我们又对它 deleteLater——两条销毁路径会在关闭后打架
        // （父析构删一遍、队列里的 deleteLater 再删一遍）。归属收敛到本对象后，
        // deleteLater 与父子析构互斥，不会有悬空事件。
        socket->setParent(this);
        if (m_peers.size() >= m_maxClients) {
            // 超出上限立即拒绝；连接里的待处理字节随断开丢弃，不假装收下了。
            socket->abort();
            socket->deleteLater();
            emit errorOccurred(m_generation, tr("客户端数达到上限 %1，已拒绝新连接").arg(m_maxClients));
            continue;
        }
        Peer peer;
        peer.id = QUuid::createUuid();
        peer.address = QStringLiteral("%1:%2").arg(
            socket->peerAddress().toString()).arg(socket->peerPort());
        socket->setReadBufferSize(PeerReadBufferBytes);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            const auto it = m_peers.constFind(socket);
            if (it == m_peers.cend()) return;
            const auto payload = socket->readAll();
            if (!payload.isEmpty())
                emit dataReceived(m_generation, makeSource(m_generation, it.value().id, it.value().address),
                    payload, int(Traffic::Framing::Stream));
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { detachPeer(socket); });
        // socket 断开后的 errorOccurred 只属于这个已detach 的对象，无需单独处理；
        // 销毁交给 disconnected 里的 deleteLater。
        m_peers.insert(socket, peer);
        QList<Transport::PeerInfo> peers;
        peers.reserve(m_peers.size());
        for (auto pit = m_peers.cbegin(); pit != m_peers.cend(); ++pit)
            peers.append(Transport::PeerInfo{pit.value().id, pit.value().address});
        emit peerListChanged(m_generation, peers);
    }
}

void TcpServerTransport::detachPeer(QTcpSocket *socket)
{
    if (!m_peers.contains(socket))
        return;
    m_peers.remove(socket);
    socket->deleteLater();
    QList<Transport::PeerInfo> peers;
    peers.reserve(m_peers.size());
    for (auto pit = m_peers.cbegin(); pit != m_peers.cend(); ++pit)
        peers.append(Transport::PeerInfo{pit.value().id, pit.value().address});
    emit peerListChanged(m_generation, peers);
}

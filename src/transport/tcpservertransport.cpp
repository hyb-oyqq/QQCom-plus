#include "transport/tcpservertransport.h"

#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <utility>

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
    stop();
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
    // disconnectFromHost 可同步发 disconnected；先移走容器并断开回调，
    // 避免 detachPeer 在遍历过程中删除当前迭代器。
    const auto peers = std::exchange(m_peers, {});
    for (auto it = peers.cbegin(); it != peers.cend(); ++it) {
        it.key()->disconnect(this);
        it.key()->disconnectFromHost();
        it.key()->deleteLater();
    }
    if (m_server)
        m_server->close();
}

qint64 TcpServerTransport::write(const QByteArray &payload, const QUuid &peerId)
{
    if (!m_server || !m_server->isListening() || m_peers.isEmpty())
        return -1;
    if (!peerId.isNull()) {
        // 直接遍历匹配 peer：客户端数量有上限（≤64），代价可忽略。
        QTcpSocket *target = nullptr;
        for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it)
            if (it.value().id == peerId) { target = it.key(); break; }
        if (!target)
            return -1;
        return target->write(payload);
    }
    // 广播按最差的一个客户端记账。把各端接受数加起来会把「一个写满、一个没写上」
    // 伪装成整包完成，少掉的那段再也补不回来。
    qint64 least = payload.size();
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
        const qint64 accepted = it.key()->write(payload);
        least = qMin(least, accepted < 0 ? qint64(0) : accepted);
    }
    return least;
}

qint64 TcpServerTransport::discardPendingInput()
{
    qint64 dropped = 0;
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
        if (it.key()->bytesAvailable() <= 0)
            continue;
        dropped += it.key()->readAll().size();
    }
    return dropped;
}

QString TcpServerTransport::writeAddress(const QUuid &peerId) const
{
    if (!peerId.isNull()) {
        for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it)
            if (it.value().id == peerId) return it.value().address;
        return {};
    }
    // 空 peerId 是写全部客户端。只有一个时地址是确定的；多个时日志不能假装只有一个。
    return m_peers.size() == 1 ? m_peers.constBegin().value().address : QString();
}

void TcpServerTransport::acceptPending()
{
    while (m_server && m_server->hasPendingConnections()) {
        auto *socket = m_server->nextPendingConnection();
        // 连接归传输对象管理，独立于监听器的重新创建。
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
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { drainPeer(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] { detachPeer(socket); });
        // socket 断开后的 errorOccurred 只属于这个已detach 的对象，无需单独处理；
        // 销毁交给 disconnected 里的 deleteLater。
        m_peers.insert(socket, peer);
        // 接入必须进数据区：断开已经有错误行，连上只改状态栏人数会让人看不到谁来了。
        emit peerConnected(m_generation, peer.id, peer.address);
        publishPeers();
        // 客户端可能在 accept 之前就把第一包写进了套接字。丢掉它的话，请求-应答看起来像收不到。
        drainPeer(socket);
    }
}

void TcpServerTransport::drainPeer(QTcpSocket *socket)
{
    const auto it = m_peers.constFind(socket);
    if (it == m_peers.cend() || !socket || socket->bytesAvailable() <= 0)
        return;
    const auto id = it.value().id;
    const auto address = it.value().address;
    const auto payload = socket->readAll();
    if (payload.isEmpty())
        return;
    emit dataReceived(m_generation, makeSource(m_generation, id, address),
        payload, int(Traffic::Framing::Stream));
    if (!m_peers.contains(socket) || socket->bytesAvailable() <= 0)
        return;
    QPointer<QTcpSocket> alive(socket);
    QMetaObject::invokeMethod(this, [this, alive] {
        if (alive) drainPeer(alive);
    }, Qt::QueuedConnection);
}

void TcpServerTransport::detachPeer(QTcpSocket *socket)
{
    if (!m_peers.contains(socket))
        return;
    // 客户端离开必须看得见：以前只在 peer 列表里悄悄少一个（状态栏数字变化），
    // "对端什么时候断的"无从查起。作为运行期异常事件发布，服务器本身仍在监听。
    // 用户主动 stop() 时由上层代次过滤这些收尾事件，不会误报。
    const QString address = m_peers.value(socket).address;
    m_peers.remove(socket);
    socket->deleteLater();
    emit errorOccurred(m_generation, tr("客户端 %1 已断开").arg(address));
    publishPeers();
}

void TcpServerTransport::publishPeers()
{
    QList<Transport::PeerInfo> peers;
    peers.reserve(m_peers.size());
    for (auto pit = m_peers.cbegin(); pit != m_peers.cend(); ++pit)
        peers.append(Transport::PeerInfo{pit.value().id, pit.value().address});
    emit peerListChanged(m_generation, peers);
}

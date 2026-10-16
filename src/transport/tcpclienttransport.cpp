#include "transport/tcpclienttransport.h"

#include <QTcpSocket>
#include <QTimer>

TcpClientTransport::TcpClientTransport(int endpointIndex, QObject *parent)
    : AbstractTransport(parent)
{
    m_endpointIndex = endpointIndex;
}

void TcpClientTransport::start(const PortDraft &draft, quint64 generation)
{
    m_draft = draft;
    m_generation = generation;
    m_open = false;
    m_userStop = false;
    m_failureReported = false;

    const auto host = draft.values.value(QStringLiteral("client.host")).toString().trimmed();
    const int port = draft.values.value(QStringLiteral("client.port"), 9000).toInt();
    const int timeoutMs = draft.values.value(QStringLiteral("client.timeout"), 5000).toInt();

    if (m_socket) {
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    if (!m_timeout) {
        m_timeout = new QTimer(this);
        m_timeout->setSingleShot(true);
        connect(m_timeout, &QTimer::timeout, this, [this] {
            // 定时器触发时连接仍未建立：主动放弃，不再等系统超时。
            // abort 在连接阶段不会发出 disconnected；失败只由这里报一次。
            if (m_socket)
                m_socket->abort();
            if (m_userStop || m_failureReported)
                return;
            m_failureReported = true;
            emit errorOccurred(m_generation, tr("连接超时"));
            emit stateChanged(m_generation, int(Transport::State::Closed), tr("连接超时"));
        });
    }

    m_socket = new QTcpSocket(this);
    m_socket->setReadBufferSize(256 * 1024);
    // 必须在 connectToHost 之前订阅。Qt 在发出 connected() 之前就会打开读通知，
    // 对端若在握手后立刻写入，readyRead 可能先于 connected() 到达；那时再订阅就丢了第一包。
    connect(m_socket, &QTcpSocket::readyRead, this, &TcpClientTransport::drainSocket);
    connect(m_socket, &QTcpSocket::connected, this, [this] {
        m_timeout->stop();
        m_open = true;
        emit stateChanged(m_generation, int(Transport::State::Open), tr("已连接 %1")
            .arg(peerText()));
        // 通知已经读进缓冲、但 readyRead 被跳过时，这里补一次。不要丢弃：这是本次会话的数据。
        drainSocket();
    });
    connect(m_socket, &QTcpSocket::disconnected, this, [this] {
        m_timeout->stop();
        const bool wasOpen = m_open;
        m_open = false;
        if (m_userStop) {
            emit stateChanged(m_generation, int(Transport::State::Closed), tr("已断开"));
        } else if (wasOpen && !m_failureReported) {
            // 对端关闭：这是调试时最需要看见的事件。打开后的其它致命错误已报过，这里不再叠一条。
            m_failureReported = true;
            emit errorOccurred(m_generation, tr("连接已断开"));
            emit stateChanged(m_generation, int(Transport::State::Closed), tr("连接已断开"));
        }
        // wasOpen == false：连接从未建立，失败原因已由 errorOccurred 处理器统一给出
        //（"连接失败：<errorString>" / "连接超时"）。这里若再播一条"连接失败"，
        // 一次连不上的端口会同时产生两条 Error + 两条 State，去重与计数都跟着乱。
    });
    connect(m_socket, &QTcpSocket::errorOccurred, this,
        [this](QAbstractSocket::SocketError error) {
            if (m_userStop || m_failureReported)
                return;
            // 尚未连上：超时、拒绝、找不到主机都走这一条。
            if (!m_open) {
                m_failureReported = true;
                m_timeout->stop();
                emit errorOccurred(m_generation, tr("连接失败：%1").arg(m_socket->errorString()));
                emit stateChanged(m_generation, int(Transport::State::Closed), tr("连接失败"));
                return;
            }
            // 对端正常关闭交给 disconnected，避免同一事件两条记录。
            // TemporaryError 是非阻塞套接字暂时不可写，连接还在，不能拆掉。
            if (error == QAbstractSocket::RemoteHostClosedError
                || error == QAbstractSocket::TemporaryError)
                return;
            const QString detail = m_socket->errorString();
            m_failureReported = true;
            m_open = false;
            m_timeout->stop();
            emit errorOccurred(m_generation, tr("连接中断：%1").arg(detail));
            emit stateChanged(m_generation, int(Transport::State::Closed), tr("连接中断"));
            // 先清 m_open 再 abort：disconnected 不会再报「连接已断开」。
            // 套接字可能仍显示已连接但不再可读，不 abort 界面会停在已打开。
            if (m_socket->state() != QAbstractSocket::UnconnectedState)
                m_socket->abort();
        });
    emit stateChanged(m_generation, int(Transport::State::Opening),
        tr("连接中 %1:%2").arg(host).arg(port));
    m_socket->connectToHost(host, quint16(port));
    if (timeoutMs > 0)
        m_timeout->start(timeoutMs);
}

void TcpClientTransport::stop()
{
    m_userStop = true;
    if (m_timeout)
        m_timeout->stop();
    if (m_socket)
        m_socket->disconnectFromHost(); // 尽量把已缓冲数据送出去
}

qint64 TcpClientTransport::write(const QByteArray &payload, const QUuid &peerId)
{
    Q_UNUSED(peerId);
    if (!m_socket || !m_open)
        return -1;
    return m_socket->write(payload);
}

void TcpClientTransport::drainSocket()
{
    // 连接完成前对端地址还没定。字节留在缓冲里，connected() 里再读，避免来源是空的。
    if (!m_socket || !m_open || m_socket->bytesAvailable() <= 0)
        return;
    const auto payload = m_socket->readAll();
    if (payload.isEmpty())
        return;
    emit dataReceived(m_generation, makeSource(m_generation, QUuid(), peerText()), payload,
        int(Traffic::Framing::Stream));
    // 槽执行期间到达的字节不会再触发 readyRead。排队再读，避免和桥接回写叠在同一层调用栈上。
    if (m_socket && m_socket->bytesAvailable() > 0)
        QMetaObject::invokeMethod(this, [this] { drainSocket(); }, Qt::QueuedConnection);
}

qint64 TcpClientTransport::discardPendingInput()
{
    if (!m_socket || !m_open || m_socket->bytesAvailable() <= 0)
        return 0;
    return m_socket->readAll().size();
}

QString TcpClientTransport::writeAddress(const QUuid &peerId) const
{
    Q_UNUSED(peerId);
    return peerText();
}

QString TcpClientTransport::peerText() const
{
    if (!m_socket || !m_open) return {};
    return QStringLiteral("%1:%2").arg(m_socket->peerAddress().toString()).arg(m_socket->peerPort());
}

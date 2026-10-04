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
            m_socket->abort();
            emit errorOccurred(m_generation, tr("连接超时"));
            emit stateChanged(m_generation, int(Transport::State::Closed), tr("连接超时"));
        });
    }

    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected, this, [this] {
        m_timeout->stop();
        m_open = true;
        emit stateChanged(m_generation, int(Transport::State::Open), tr("已连接 %1:%2")
            .arg(m_socket->peerName(), QString::number(m_socket->peerPort())));
    });
    connect(m_socket, &QTcpSocket::disconnected, this, [this] {
        m_timeout->stop();
        const bool wasOpen = m_open;
        m_open = false;
        if (m_userStop) {
            emit stateChanged(m_generation, int(Transport::State::Closed), tr("已断开"));
        } else if (wasOpen) {
            // 对端关闭或网络中断：这是调试时最需要看见的事件。
            emit errorOccurred(m_generation, tr("连接已断开"));
            emit stateChanged(m_generation, int(Transport::State::Closed), tr("连接已断开"));
        }
        // wasOpen == false：连接从未建立，失败原因已由 errorOccurred 处理器统一给出
        //（"连接失败：<errorString>" / "连接超时"）。这里若再播一条"连接失败"，
        // 一次连不上的端口会同时产生两条 Error + 两条 State，去重与计数都跟着乱。
    });
    connect(m_socket, &QTcpSocket::errorOccurred, this,
        [this](QAbstractSocket::SocketError error) {
            Q_UNUSED(error);
            // Open 阶段的 RemoteHostClosed 由 disconnected 统一播报，避免一行事件两条记录。
            if (m_open && error == QAbstractSocket::RemoteHostClosedError) return;
            if (!m_open) {
                m_timeout->stop();
                emit errorOccurred(m_generation, tr("连接失败：%1").arg(m_socket->errorString()));
                emit stateChanged(m_generation, int(Transport::State::Closed), tr("连接失败"));
            }
        });
    connect(m_socket, &QTcpSocket::readyRead, this, [this] {
        const auto payload = m_socket->readAll();
        if (!payload.isEmpty())
            emit dataReceived(m_generation, makeSource(m_generation), payload,
                int(Traffic::Framing::Stream));
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

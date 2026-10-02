#include "transport/udptransport.h"

#include <QUdpSocket>

UdpTransport::UdpTransport(int endpointIndex, QObject *parent)
    : AbstractTransport(parent)
{
    m_endpointIndex = endpointIndex;
}

void UdpTransport::start(const PortDraft &draft, quint64 generation)
{
    m_draft = draft;
    m_generation = generation;

    if (m_socket) {
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }

    const auto bindText = draft.values.value(QStringLiteral("udp.address"), "0.0.0.0").toString();
    const int bindPort = draft.values.value(QStringLiteral("udp.port"), 0).toInt();
    const QHostAddress bindAddress = bindText == QLatin1String("0.0.0.0")
        ? QHostAddress(QHostAddress::AnyIPv4) : QHostAddress(bindText);
    if (bindAddress.isNull()) {
        emit errorOccurred(m_generation, tr("无效的本地地址：%1").arg(bindText));
        emit stateChanged(m_generation, int(Transport::State::Closed), tr("打开失败"));
        return;
    }

    // 目标地址可留空：仅接收（界面与校验规则同此约定）。
    const auto targetText = draft.values.value(QStringLiteral("udp.target")).toString().trimmed();
    const int targetPort = draft.values.value(QStringLiteral("udp.targetPort"), 0).toInt();
    if (!targetText.isEmpty()) {
        m_targetAddress = QHostAddress(targetText);
        m_targetPort = quint16(qBound(1, targetPort, 65535));
        if (m_targetAddress.isNull()) {
            emit errorOccurred(m_generation, tr("无效的目标地址：%1").arg(targetText));
            emit stateChanged(m_generation, int(Transport::State::Closed), tr("打开失败"));
            return;
        }
    } else {
        m_targetAddress.clear();
        m_targetPort = 0;
    }

    m_socket = new QUdpSocket(this);
    connect(m_socket, &QUdpSocket::readyRead, this, &UdpTransport::drainPendingDatagrams);
    if (!m_socket->bind(bindAddress, quint16(bindPort))) {
        emit errorOccurred(m_generation, tr("绑定 %1:%2 失败：%3")
            .arg(bindText).arg(bindPort).arg(m_socket->errorString()));
        emit stateChanged(m_generation, int(Transport::State::Closed), tr("打开失败"));
        return;
    }
    const auto targetSuffix = m_targetPort != 0
        ? tr(" → 发往 %1:%2").arg(m_targetAddress.toString()).arg(m_targetPort)
        : tr("（仅接收）");
    emit stateChanged(m_generation, int(Transport::State::Open), tr("绑定 %1:%2%3")
        .arg(m_socket->localAddress().toString())
        .arg(m_socket->localPort())
        .arg(targetSuffix));
}

void UdpTransport::stop()
{
    if (m_socket)
        m_socket->close();
}

qint64 UdpTransport::write(const QByteArray &payload, const QUuid &peerId)
{
    Q_UNUSED(peerId);
    if (!m_socket || m_socket->state() != QAbstractSocket::BoundState)
        return -1;
    if (m_targetPort == 0)
        return -1; // 仅接收的端点没有发送目标
    // 整报提交：writeDatagram 不支持部分写，返回值要么是整报长度要么是 -1。
    return m_socket->writeDatagram(payload, m_targetAddress, m_targetPort);
}

void UdpTransport::drainPendingDatagrams()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        QByteArray payload(qsizetype(m_socket->pendingDatagramSize()), Qt::Uninitialized);
        QHostAddress sender;
        quint16 senderPort = 0;
        const qint64 read = m_socket->readDatagram(payload.data(), payload.size(), &sender, &senderPort);
        if (read < 0)
            return; // 读失败保留剩余，下轮 readyRead 继续
        payload.resize(read); // 零长数据报仍是合法数据报，保留空 payload
        auto source = makeSource(m_generation, QUuid(),
            QStringLiteral("%1:%2").arg(sender.toString()).arg(senderPort));
        emit dataReceived(m_generation, source, payload, int(Traffic::Framing::Datagram));
    }
}

#ifdef _WIN32
// winsock2 必须先于 Qt 头。Qt 在 Windows 上会包含 windows.h，后包含 winsock2 会和 winsock.h 冲突。
// 这里不能用 Q_OS_WIN：它要等 Qt 头进来才有定义。
#include <winsock2.h>
#include <mswsock.h>
#ifndef SIO_UDP_CONNRESET
// _WSAIOW(IOC_VENDOR, 12)。当前 SDK 的 mstcpip.h 不公开这个控制码。
#define SIO_UDP_CONNRESET 0x9800000CUL
#endif
#endif

#include "transport/udptransport.h"

#include <QUdpSocket>

namespace {

// Windows 默认在收到 ICMP 端口不可达后让后续 recv 返回 WSAECONNRESET。
// 一次发往未监听端口（手动发送或桥接转发）就会让这个套接字再也收不到数据。
void silenceUdpPortUnreachable(QUdpSocket *socket)
{
#ifdef _WIN32
    const auto descriptor = socket->socketDescriptor();
    if (descriptor < 0)
        return;
    BOOL report = FALSE;
    DWORD returned = 0;
    ::WSAIoctl(SOCKET(descriptor), SIO_UDP_CONNRESET, &report, sizeof(report),
        nullptr, 0, &returned, nullptr, nullptr);
#else
    Q_UNUSED(socket);
#endif
}

} // namespace

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

    // 目标地址可留空：仅接收。收到的数据报不隐式改变目的地址，避免任意来源
    // 在用户不知情时接管后续发送。
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
    if (!m_socket->bind(bindAddress, quint16(bindPort))) {
        emit errorOccurred(m_generation, tr("绑定 %1:%2 失败：%3")
            .arg(bindText).arg(bindPort).arg(m_socket->errorString()));
        emit stateChanged(m_generation, int(Transport::State::Closed), tr("打开失败"));
        return;
    }
    silenceUdpPortUnreachable(m_socket);
    discardPendingInput();
    connect(m_socket, &QUdpSocket::readyRead, this, &UdpTransport::drainPendingDatagrams);
    const auto targetSuffix = m_targetPort != 0
        ? tr(" → 发送目标 %1:%2").arg(m_targetAddress.toString()).arg(m_targetPort)
        : tr("；未设置发送目标：仅接收，收到数据报不会建立连接或自动回发");
    emit stateChanged(m_generation, int(Transport::State::Open), tr("监听 %1:%2%3")
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

QString UdpTransport::writeAddress(const QUuid &peerId) const
{
    Q_UNUSED(peerId);
    if (m_targetPort == 0) return {};
    return QStringLiteral("%1:%2").arg(m_targetAddress.toString()).arg(m_targetPort);
}

qint64 UdpTransport::discardPendingInput()
{
    if (!m_socket) return 0;
    qint64 dropped = 0;
    while (m_socket->hasPendingDatagrams()) {
        const qint64 size = m_socket->pendingDatagramSize();
        QHostAddress sender;
        quint16 senderPort = 0;
        // maxSize 为 0 时 Qt 丢掉整报并返回 0，长度要用探测到的尺寸。
        if (m_socket->readDatagram(nullptr, 0, &sender, &senderPort) < 0)
            break;
        if (size > 0)
            dropped += size;
    }
    return dropped;
}

void UdpTransport::drainPendingDatagrams()
{
    if (!m_socket)
        return;
    // Qt 6.10 的 readDatagram 无论成败都会重新打开读通知。失败只丢掉这一拍，
    // 后面的包仍会触发 readyRead。读失败时这一报可能还在队首，不能立刻再排队，
    // 否则同一条错误会把 I/O 线程转满。
    bool consumed = false;
    for (int n = 0; n < 256 && m_socket->hasPendingDatagrams(); ++n) {
        const qint64 size = m_socket->pendingDatagramSize();
        if (size < 0) {
            // 尺寸都拿不到：读走这一报，避免它一直占着队首。字节数未知，只记错误。
            if (m_socket->readDatagram(nullptr, 0) < 0) {
                emit errorOccurred(m_generation, tr("读取数据报失败：%1").arg(m_socket->errorString()));
                break;
            }
            consumed = true;
            emit errorOccurred(m_generation, tr("丢弃 1 个无法读取的数据报"));
            continue;
        }
        QByteArray payload(qsizetype(size), Qt::Uninitialized);
        QHostAddress sender;
        quint16 senderPort = 0;
        const qint64 read = m_socket->readDatagram(payload.data(), payload.size(), &sender, &senderPort);
        if (read < 0) {
            // 读失败没有单独的丢弃行，字节数挂在这条错误上，状态栏只加一次。
            const qint64 dropped = qMax<qint64>(0, size);
            emit errorOccurred(m_generation, tr("丢弃 1 个数据报（%1 字节）：%2")
                .arg(dropped)
                .arg(m_socket->errorString()), dropped);
            break;
        }
        consumed = true;
        payload.resize(read); // 零长数据报仍是合法数据报，保留空 payload
        auto source = makeSource(m_generation, QUuid(),
            QStringLiteral("%1:%2").arg(sender.toString()).arg(senderPort));
        emit dataReceived(m_generation, source, payload, int(Traffic::Framing::Datagram));
    }
    // 槽里又到的数据报同样不会嵌套触发 readyRead。读走过才排队。
    if (consumed && m_socket->hasPendingDatagrams())
        QMetaObject::invokeMethod(this, [this] { drainPendingDatagrams(); }, Qt::QueuedConnection);
}

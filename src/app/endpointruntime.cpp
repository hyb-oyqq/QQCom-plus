#include "app/endpointruntime.h"
#include "transport/transportfactory.h"

#include <QDateTime>

EndpointRuntime::EndpointRuntime(QObject *parent) : QObject(parent)
{
    m_clock.start();
}

EndpointRuntime::~EndpointRuntime()
{
    // 在 I/O 线程内析构（组合根 quit + wait 之后），传输对象就地关闭销毁，
    // 不依赖延迟删除。
    for (auto *&transport : m_transports) {
        if (!transport) continue;
        transport->stop();
        delete transport;
        transport = nullptr;
    }
}

void EndpointRuntime::openEndpoint(int index, const PortDraft &draft)
{
    if (index < 0 || index >= EndpointCount) return;
    closeTransport(index);
    m_generations[index]++;
    m_drafts[index] = draft;

    auto *transport = TransportFactory::create(draft.kind, index, this);
    if (!transport) return;
    m_transports[index] = transport;
    const quint64 generation = m_generations[index];

    // 连接按代次闭包：实例被替换后，旧实例晚到的信号一律丢弃。
    connect(transport, &AbstractTransport::stateChanged, this,
        [this, index, generation](quint64 gen, int state, const QString &detail) {
            if (gen != m_generations[index]) return;
            auto message = makeMessage(index, Traffic::Direction::State, Traffic::Origin::Automatic,
                {}, Traffic::Framing::Message);
            message.statusText = detail;
            publish(std::move(message));
            emit endpointStateChanged(index, state, detail);
        });
    connect(transport, &AbstractTransport::dataReceived, this,
        [this, index, generation](quint64 gen, const Traffic::Source &source,
                                  const QByteArray &payload, int framing) {
            if (gen != m_generations[index]) return;
            auto message = makeMessage(index, Traffic::Direction::Receive, Traffic::Origin::Automatic,
                payload, Traffic::Framing(framing));
            message.source = source; // 传输层带 peer/代次的完整来源
            message.frameId = QUuid::createUuid();
            publish(std::move(message));
        });
    connect(transport, &AbstractTransport::peerListChanged, this,
        [this, index, generation](quint64 gen, const QList<Transport::PeerInfo> &peers) {
            if (gen != m_generations[index]) return;
            QStringList addresses;
            addresses.reserve(peers.size());
            for (const auto &peer : peers) addresses.append(peer.address);
            emit endpointPeersChanged(index, addresses);
        });
    connect(transport, &AbstractTransport::errorOccurred, this,
        [this, index, generation](quint64 gen, const QString &text) {
            if (gen != m_generations[index]) return;
            auto message = makeMessage(index, Traffic::Direction::Error, Traffic::Origin::Automatic,
                {}, Traffic::Framing::Message);
            message.statusText = text;
            publish(std::move(message));
        });

    transport->start(draft, generation);
}

void EndpointRuntime::closeEndpoint(int index)
{
    if (index < 0 || index >= EndpointCount) return;
    closeTransport(index);

    auto message = makeMessage(index, Traffic::Direction::State, Traffic::Origin::Automatic,
        {}, Traffic::Framing::Message);
    message.statusText = tr("已关闭");
    publish(std::move(message));
    emit endpointStateChanged(index, int(Transport::State::Closed), tr("已关闭"));
    emit endpointPeersChanged(index, {});
}

void EndpointRuntime::sendPayload(int index, const QByteArray &payload, int origin)
{
    if (index < 0 || index >= EndpointCount) return;
    auto message = makeMessage(index, Traffic::Direction::Transmit,
        Traffic::Origin(origin), payload, Traffic::Framing::Message);
    message.requestId = QUuid::createUuid();

    auto *transport = m_transports[index];
    // 写入同步返回"本地接受"的字节数：全部接受记 LocalCompleted（仅代表
    // 本地写出，不等同对端收到），部分接受记 Partial，拒绝/失败记 Failed。
    const qint64 accepted = transport ? transport->write(payload, QUuid()) : -1;
    if (accepted < 0) {
        message.state = Traffic::State::Failed;
        message.statusText = transport ? tr("写入被拒绝：端口未就绪或无可用目标") : tr("端口未打开");
    } else if (accepted >= payload.size()) {
        message.state = Traffic::State::LocalCompleted;
    } else {
        message.state = Traffic::State::Partial;
        message.statusText = tr("本地接受 %1/%2 字节").arg(accepted).arg(payload.size());
    }
    publish(std::move(message));
}

void EndpointRuntime::closeTransport(int index)
{
    auto *&transport = m_transports[index];
    if (!transport) return;
    m_generations[index]++; // 立即作废旧实例的一切在途事件
    transport->stop();
    transport->deleteLater(); // 同线程延迟删除；关闭引发的收尾信号已被代次过滤
    transport = nullptr;
}

Traffic::Source EndpointRuntime::sourceOf(int index) const
{
    auto source = Traffic::makeSource(m_drafts[index], index);
    source.generation = m_generations[index];
    return source;
}

Traffic::Message EndpointRuntime::makeMessage(int index, Traffic::Direction direction,
    Traffic::Origin origin, const QByteArray &raw, Traffic::Framing framing)
{
    Traffic::Message message;
    message.id = m_nextMessageId++;
    message.timestamp = QDateTime::currentDateTime();
    message.monotonicNs = m_clock.nsecsElapsed();
    message.direction = direction;
    message.origin = origin;
    message.framing = framing;
    message.raw = raw;
    message.rawLength = raw.size();
    message.source = sourceOf(index);
    return message;
}

void EndpointRuntime::publish(Traffic::Message message)
{
    emit messageReceived(std::move(message));
}

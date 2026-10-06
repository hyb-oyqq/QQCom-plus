#include "app/endpointruntime.h"
#include "transport/transportfactory.h"

#include <QDateTime>
#include <QTimer>

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

namespace {

// 状态行的正文：**状态词 + 传输层细节**。
//
// 传输层给的 detail 只是"哪个口的什么参数"（例如 `COM2 @115200`），单看它读不出
// 这条是"刚打开"还是"刚失败"——用户反馈"只有端口信息而没有已打开已关闭的日志"，
// 指的就是这个：屏幕上出现一行 `状态: COM2 @115200`，看不出连接到底成没成。
// 所以 Open/Opening 加状态词。
//
// **Closed 则要反过来补身份**：它的 detail 是通用措辞（`已关闭 · 用户停止` /
// `设备错误`），本身不含端口，单看一行"状态: 已关闭 · 用户停止"根本不知道是哪个口关了
//（用户反馈"不知道谁关闭了"）。而 Open 的 detail（`COM3 @115200`）与 Error 的正文
//（`打开串口 COM3 失败`）都已自带端口，再补身份就是把同一个端口说两遍——所以身份
// 只补在**没说清是谁**的地方（`identity` 为空时自动省略）。
QString stateRowText(int state, const QString &detail, const QString &identity)
{
    switch (Transport::State(state)) {
    case Transport::State::Open:
        return detail.isEmpty() ? QObject::tr("已打开") : QObject::tr("已打开 · %1").arg(detail);
    case Transport::State::Opening:
        return detail.isEmpty() ? QObject::tr("正在打开") : QObject::tr("正在打开 · %1").arg(detail);
    case Transport::State::Closed:
        if (detail.isEmpty())
            return identity.isEmpty() ? QObject::tr("已关闭")
                                      : QObject::tr("%1 已关闭").arg(identity);
        return identity.isEmpty() ? detail : QStringLiteral("%1 %2").arg(identity, detail);
    }
    return detail;
}

// 事件行的端点身份：**字母 + 具体端口**，例如 `A · COM3`、`B · 127.0.0.1:9000`。
// 字母与端点在界面上的叫法一致（用户按"端口 A / 端口 B"思考），端口取自草稿的真实
// 配置值（未配置时退化成只有字母，不拿默认端口冒充）。
//
// 这里没有复用 UI 的 endpointLabel()：那个是"字母 · 类型"（`A · 串口`），出自
// src/ui/common，而运行时属于 app 层、不能依赖 UI（见 services.pri 的分层）。
// 两者用途不同——那个用于参数区标识，这个是日志行的归属标记，带的是端口而非类型。
QString endpointIdentity(int index, const PortDraft &draft)
{
    if (index < 0 || index > 25) return {};
    const QString letter = QString(QChar::fromLatin1('A' + char(index)));
    const QString port = portShortLabel(draft);
    return port.isEmpty() ? letter : QStringLiteral("%1 · %2").arg(letter, port);
}

} // namespace

void EndpointRuntime::openEndpoint(int index, const PortDraft &draft)
{
    if (index < 0 || index >= EndpointCount) return;
    closeTransport(index);
    m_generations[index]++;
    m_pendingFailureGeneration[index] = 0; // 换代即作废旧失败合并锚点，避免串代误并
    m_drafts[index] = draft;
    // 组帧策略随 open 下发；换策略即清空缓冲，不把上次连接的半帧带进来。
    m_framers[index].setConfig(draft.framing);

    auto *transport = TransportFactory::create(draft.kind, index, this);
    if (!transport) return;
    m_transports[index] = transport;
    const quint64 generation = m_generations[index];

    // 空闲输出定时器在本对象已归属 I/O 线程后创建，超时回调自然落在 I/O 线程。
    auto *idle = new QTimer(this);
    idle->setSingleShot(true);
    connect(idle, &QTimer::timeout, this, [this, index, generation] {
        if (generation != m_generations[index]) return;
        flushFramer(index, ReceiveFramer::Idle);
    });
    m_idleTimers[index] = idle;

    // 连接按代次闭包：实例被替换后，旧实例晚到的信号一律丢弃。
    connect(transport, &AbstractTransport::stateChanged, this,
        [this, index, generation](quint64 gen, int state, const QString &detail) {
            if (gen != m_generations[index]) return;
            // 一次失败被传输层拆成"errorOccurred + Closed"两条信号；错误行已经给出了完整
            // 原因，这条随之而来的 Closed 若再单独成行，用户看到的就是"错误 xx"＋"状态 打开失败"
            // 两条（他反馈的"错误跟状态感觉重复了"）。所以它只负责把端点确实回到 Closed
            // 这件事告诉界面（复位按钮、状态栏摘要），不再写第二条日志。
            const bool mergedIntoRecentError = state == int(Transport::State::Closed)
                && m_pendingFailureGeneration[index] == generation;
            m_pendingFailureGeneration[index] = 0; // 任何一次状态上报都消费掉这个锚点
            if (!mergedIntoRecentError) {
                auto message = makeMessage(index, Traffic::Direction::State, Traffic::Origin::Automatic,
                    {}, Traffic::Framing::Message);
            // 状态词与细节在这里拼定，监视区因此能看到"已打开 · COM2 @115200"这样
            // 一眼读出结果的迁移记录（见 stateRowText 的说明）。
            // 身份取自**当前草稿**：它要么是本代次 open 下发的配置，要么（重开后）
            // 已是新配置，两种情况都比"不写是谁"准确。
            message.statusText = stateRowText(state, detail, endpointIdentity(index, m_drafts[index]));
                publish(std::move(message));
            }
            emit endpointStateChanged(index, state, detail);
        });
    connect(transport, &AbstractTransport::dataReceived, this,
        [this, index, generation](quint64 gen, const Traffic::Source &source,
                                  const QByteArray &payload, int framing) {
            if (gen != m_generations[index]) return;
            // 数据报自带边界，直接就是一条；字节流片段才交给组帧器。
            if (Traffic::Framing(framing) != Traffic::Framing::Stream) {
                auto message = makeMessage(index, Traffic::Direction::Receive,
                    Traffic::Origin::Automatic, payload, Traffic::Framing(framing));
                message.source = source; // 传输层带 peer/代次的完整来源
                message.frameId = QUuid::createUuid();
                publish(std::move(message));
                return;
            }
            feedFramer(index, source, payload);
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
            // 失败的唯一一条日志：错误行正文就是完整原因。传输层随后若紧跟一条同代次
            // Closed（打开失败 / 设备错误 / 连接断开都会），状态处理器会把它并进这里，
            // 不再单独成行——但"端点已回到 Closed"仍会照常上报给界面。
            m_pendingFailureGeneration[index] = generation;
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

    // "谁关的"必须写在正文里：以前这条写死"已关闭"，用户看着一条没有来源的关闭记录，
    // 无从判断是自己点了停止，还是对端断了。用户主动关闭是这个来源的**专有措辞**，
    // 语义落在 detail 本身（而不是给所有关闭统一套前缀——那会把"打开失败"读成"关过"）。
    const QString stopped = tr("已关闭 · 用户停止");
    auto message = makeMessage(index, Traffic::Direction::State, Traffic::Origin::Automatic,
        {}, Traffic::Framing::Message);
    // 与传输层上报的状态行共用同一套拼装（stateRowText）——用户主动关闭也是一次
    // 状态迁移，正文口径必须一致。身份同样要带上，否则"谁关了"只答了一半：
    // 知道了原因，还是不知道是哪个端口。
    message.statusText = stateRowText(int(Transport::State::Closed), stopped,
        endpointIdentity(index, m_drafts[index]));
    publish(std::move(message));
    emit endpointStateChanged(index, int(Transport::State::Closed), stopped);
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
    // 关闭前把组帧缓冲里的残留输出掉：半帧不该凭空消失，用户要看到最后那几个字节。
    flushFramer(index, ReceiveFramer::Closed);
    if (auto *idle = m_idleTimers[index]) {
        idle->stop();
        delete idle;
        m_idleTimers[index] = nullptr;
    }
    auto *&transport = m_transports[index];
    if (!transport) return;
    m_generations[index]++; // 立即作废旧实例的一切在途事件
    transport->stop();
    transport->deleteLater(); // 同线程延迟删除；关闭引发的收尾信号已被代次过滤
    transport = nullptr;
}

void EndpointRuntime::feedFramer(int index, const Traffic::Source &source, const QByteArray &payload)
{
    const auto frames = m_framers[index].feed(payload);
    publishFrames(index, source, frames);
    restartIdleTimer(index); // 有新数据即推迟空闲输出
}

void EndpointRuntime::publishFrames(int index, const Traffic::Source &source,
    const QList<ReceiveFramer::Frame> &frames)
{
    for (const auto &frame : frames) {
        // 按判定收齐的是完整消息；兜底切出来的仍是流片段（后面可能还有）。
        auto message = makeMessage(index, Traffic::Direction::Receive, Traffic::Origin::Automatic,
            frame.bytes, frame.complete ? Traffic::Framing::Message : Traffic::Framing::Stream);
        message.source = source;
        message.frameId = QUuid::createUuid();
        if (!frame.complete) message.hasMoreFragments = true;
        publish(std::move(message));
        // 提示日志：只有开关打开且这次输出有话要说才发，正常定界不产生噪声。
        if (m_framers[index].config().logHints && !frame.hint.isEmpty()) {
            auto hint = makeMessage(index, Traffic::Direction::Warning, Traffic::Origin::Automatic,
                {}, Traffic::Framing::Message);
            hint.statusText = frame.hint;
            publish(std::move(hint));
        }
    }
}

void EndpointRuntime::flushFramer(int index, ReceiveFramer::Trigger trigger)
{
    const auto frames = m_framers[index].flush(trigger);
    publishFrames(index, sourceOf(index), frames);
}

void EndpointRuntime::restartIdleTimer(int index)
{
    auto *idle = m_idleTimers[index];
    if (!idle) return;
    const int timeout = m_framers[index].config().idleTimeoutMs;
    if (timeout <= 0) return;
    idle->start(timeout);
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

#ifndef ENDPOINTRUNTIME_H
#define ENDPOINTRUNTIME_H

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include "app/receiveframer.h"
#include "domain/portdraft.h"
#include "domain/trafficmessage.h"
#include "transport/abstracttransport.h"

// 通信运行时：工作区固定 A/B 两个端点。对象整体归属 I/O 线程，GUI 经
// 队列调用的槽下发命令，运行时把传输层事件装配成 Traffic::Message 回发
// GUI——GUI 因此不需要知道传输层类型，也不跨线程碰任何 socket。
//
// 命令路径（GUI → 运行时，全部排队）：
//   openEndpoint(index, draft)  关旧实例 → 新代次 → 新建传输并 start
//   closeEndpoint(index)        用户关闭；此后旧实例事件按代次丢弃
//   sendPayload(index, payload, origin)  写入并把结果装配成 TX 消息
//
// 线程内职责只有三件事：代次过滤、消息装配（id/时间戳/来源）、事件转发。
// 路由转发、周期发送、拥塞策略属于后续里程碑，不在这里预埋。
class EndpointRuntime : public QObject
{
    Q_OBJECT
public:
    static constexpr int EndpointCount = 2;

    explicit EndpointRuntime(QObject *parent = nullptr);
    ~EndpointRuntime() override;

public slots:
    void openEndpoint(int index, const PortDraft &draft);
    void closeEndpoint(int index);
    void sendPayload(int index, const QByteArray &payload, int origin);

signals:
    void endpointStateChanged(int index, int state, const QString &detail);
    void endpointPeersChanged(int index, const QStringList &peers);
    void messageReceived(const Traffic::Message &message);

private:
    void closeTransport(int index);
    Traffic::Source sourceOf(int index) const;
    // 装配一条消息：会话内 id、墙钟 + 单调时钟、来源身份。
    Traffic::Message makeMessage(int index, Traffic::Direction direction, Traffic::Origin origin,
        const QByteArray &raw, Traffic::Framing framing);
    void publish(Traffic::Message message);
    // 组帧：字节流端点（串口、TCP 客户端）的片段先进组帧器，输出帧再装配成消息；
    // 数据报（UDP）自带边界，不经过组帧器。
    void feedFramer(int index, const Traffic::Source &source, const QByteArray &payload);
    void publishFrames(int index, const Traffic::Source &source,
        const QList<ReceiveFramer::Frame> &frames);
    void flushFramer(int index, ReceiveFramer::Trigger trigger);
    void restartIdleTimer(int index);

    AbstractTransport *m_transports[EndpointCount] = {};
    PortDraft m_drafts[EndpointCount];
    quint64 m_generations[EndpointCount] = {};
    ReceiveFramer m_framers[EndpointCount];
    // 空闲输出定时器：在本对象归属 I/O 线程后（openEndpoint 里）创建，线程归属才正确。
    QTimer *m_idleTimers[EndpointCount] = {};
    quint64 m_nextMessageId = 1;
    QElapsedTimer m_clock; // 构造即启动；速率与超时都依赖单调时钟
};

#endif

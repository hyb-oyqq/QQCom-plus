#ifndef ENDPOINTRUNTIME_H
#define ENDPOINTRUNTIME_H

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <atomic>
#include "app/receiveframer.h"
#include "domain/bridgelink.h"
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
//   setBridge(link, a, b)                建立或停止 A/B 转发；a、b 只用于状态日志
//
// 负责连接生命周期、按来源隔离的接收组帧、消息装配及事件发布。
// 桥接只做一件事：一端收到的原始字节原样写到另一端。周期发送尚未实现。
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
    // 自动恢复的一拍。GUI 只在「正在等待恢复」时按间隔调用。
    // 串口在 portPresent 为真（扫描到同名设备）时才打开；网络端点忽略该标志。
    void recoverEndpoint(int index, const PortDraft &draft, bool portPresent);
    // 用户取消等待。可从 GUI 线程直接调用：只写原子标志，已经排队的恢复拍会直接跳过。
    // 用户再次打开时由 openEndpoint 清掉。
    void suppressRecover(int index);
    void sendPayload(int index, const QByteArray &payload, int origin);
    // 后两个草稿只用于状态日志里的端口名。不替换已打开会话的配置。
    void setBridge(const BridgeLink &link, const PortDraft &endpointA = {}, const PortDraft &endpointB = {});
    void shutdown();

signals:
    void endpointStateChanged(int index, int state, const QString &detail);
    void endpointPeersChanged(int index, const QStringList &peers);
    // 每次底层收到的原始片段。文件传输等协议消费者使用它，不经过组帧器改写边界。
    void rawDataReceived(int index, const Traffic::Source &source, const QByteArray &payload);
    void messageReceived(const Traffic::Message &message);

private:
    struct ReceiveSession
    {
        ReceiveFramer framer;
        Traffic::Source source;
        QTimer *idle = nullptr;
    };
    void closeTransport(int index);
    // 异常关闭后释放传输对象，自动恢复才能在下一拍重新打开。代次照常作废。
    void retireTransport(int index);
    Traffic::Source sourceOf(int index) const;
    // 装配一条消息：会话内 id、墙钟 + 单调时钟、来源身份。
    Traffic::Message makeMessage(int index, Traffic::Direction direction, Traffic::Origin origin,
        const QByteArray &raw, Traffic::Framing framing);
    void publish(Traffic::Message message);
    // 组帧：串口和 TCP 字节流按各自连接缓冲，输出帧再装配成消息；
    // 数据报（UDP）自带边界，不经过组帧器。
    void feedFramer(int index, const Traffic::Source &source, const QByteArray &payload);
    void publishFrames(int index, const Traffic::Source &source,
        const QList<ReceiveFramer::Frame> &frames);
    void flushSession(int index, ReceiveSession &session, ReceiveFramer::Trigger trigger);
    void flushFramers(int index, ReceiveFramer::Trigger trigger);
    void removeDisconnectedPeers(int index, const QList<Transport::PeerInfo> &peers);
    // 打开端点或建立桥接时丢掉还没读走的输入，避免旧字节进入本次会话或被转发。
    void discardPendingInput();
    void forwardReceived(int index, const QByteArray &payload);

    // 打开失败的自动恢复只记一条警告。成功或用户停止后清掉，下次失败再记。
    bool m_recoverNoticeSent[EndpointCount] = {};
    // 用户已经取消等待。GUI 线程写、I/O 线程读，所以用原子量，不能等队列槽。
    std::atomic<bool> m_recoverSuppressed[EndpointCount] = {};
    AbstractTransport *m_transports[EndpointCount] = {};
    BridgeLink m_bridge;
    PortDraft m_drafts[EndpointCount];
    quint64 m_generations[EndpointCount] = {};
    // 失败合并的锚点：传输层把一次失败拆成 errorOccurred + stateChanged(Closed) 两条信号
    // （两条都是真实事件，底层契约不许删），但用户需要读到的只有一条。这里记下"刚报过
    // 错误的代次"，紧随其后的同代次 Closed 便并入那条错误行、不再另发零信息的状态行。
    // 0 是哨兵：代次从 1 起递增，0 永远不是一次真实事件的代次。
    quint64 m_pendingFailureGeneration[EndpointCount] = {};
    // UDP 没有握手。这里记录本次打开期间实际发过数据报的来源，首次见到时写一条
    // 「对端已连接」状态日志，并刷新参数区来源列表。绝不把它当成发送目标。
    QSet<QString> m_observedUdpPeers[EndpointCount];
    // TCP 服务端按连接隔离缓冲和空闲计时；串口/客户端使用空 peerId。
    // 仅启用组帧并收到非空数据时创建，直通与 UDP 不分配定时器。
    QHash<QUuid, ReceiveSession> m_receiveSessions[EndpointCount];
    quint64 m_nextMessageId = 1;
    QElapsedTimer m_clock; // 构造即启动；速率与超时都依赖单调时钟
};

#endif

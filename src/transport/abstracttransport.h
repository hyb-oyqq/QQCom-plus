#ifndef ABSTRACTTRANSPORT_H
#define ABSTRACTTRANSPORT_H

#include <QByteArray>
#include <QObject>
#include <QUuid>
#include "domain/portdraft.h"
#include "domain/trafficmessage.h"
#include "transport/transporttypes.h"

// 传输层契约：一个端点对应一个实例，只负责打开/关闭/收发；不认识界面，
// 也不做路由与显示决策——事件交给上层（EndpointRuntime）装配成消息。
//
// 线程约定：实例必须在所属线程内创建、调用与销毁（组合根把它交给 I/O
// 线程）；信号跨线程发出时由 Qt 自动排队。write() 同步返回"本地已接受"
// 的字节数——对 TCP/串口这只代表进入发送缓冲，不代表对端已收到。
//
// 会话代次（generation）：端点每重新打开/替换实例自增一次。所有事件都
// 携带发出时的代次，上层据此丢弃旧实例的残留回调（ARCHITECTURE §5.2）。
class AbstractTransport : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    // 按草稿打开端点。串口/UDP/TCP 服务器同步完成；TCP 客户端异步，
    // 结果经 stateChanged 发布。实现须在打开失败时发布 errorOccurred
    // 并回到 Closed，而不是把失败留在中间状态。
    virtual void start(const PortDraft &draft, quint64 generation) = 0;

    // 用户主动关闭。幂等；调用后实例尽快回到 Closed（异步断开的
    // 收尾事件允许继续到达，上层用代次过滤）。
    virtual void stop() = 0;

    // 写数据。peerId 为空：单目标端点写默认目标，TCP 服务器写全部客户端；
    // 非空：只写该 peer（仅 TCP 服务器支持）。返回所有目标累计接受的
    // 字节数，-1 表示当前不可写（未打开 / 无可用目标）。真实写出进度
    // 不在此函数内。
    virtual qint64 write(const QByteArray &payload, const QUuid &peerId) = 0;

signals:
    // 状态迁移。detail 是给人看的一句话（如 "COM3 @115200"、"连接中 127.0.0.1:9000"）。
    void stateChanged(quint64 generation, int state, const QString &detail);
    // 接收到的字节。framing 取 Traffic::Framing：串口/TCP 是流片段，
    // UDP 一次接收是一个完整数据报（保留边界与来源地址）。
    void dataReceived(quint64 generation, const Traffic::Source &source,
        const QByteArray &payload, int framing);
    // TCP 服务器的客户端集合变化；其余类型不发。
    void peerListChanged(quint64 generation, const QList<Transport::PeerInfo> &peers);
    // 离散错误：打开失败、意外断开、设备拔出等。状态变化仍走 stateChanged。
    void errorOccurred(quint64 generation, const QString &message);

protected:
    // 组装消息来源：草稿身份 + 本会话代次 + peer 信息（如有）。
    Traffic::Source makeSource(quint64 generation, const QUuid &peerId = QUuid(),
        const QString &peerAddress = QString()) const;

    PortDraft m_draft;
    int m_endpointIndex = 0;   // 0 = A，1 = B
    quint64 m_generation = 0;  // start() 记录的当前代次
};

#endif

#ifndef TRANSPORTTYPES_H
#define TRANSPORTTYPES_H

#include <QList>
#include <QMetaType>
#include <QUuid>

namespace Transport {

// 端点运行状态。错误不单独设态：动作按钮只有开/关两种形态，失败一律回到
// Closed，原因经 errorOccurred 发布；Opening 只属于异步连接（TCP 客户端），
// 其余类型的 start() 同步完成，直接到 Open 或 Closed。
enum class State
{
    Closed = 0,
    Opening,
    Open
};

// TCP 服务器的一个已连接客户端。身份是稳定 UUID，不用列表下标或裸
// socket descriptor：客户端断开重连后，旧 id 不会指到新连接上。
struct PeerInfo
{
    QUuid id;
    QString address; // "127.0.0.1:52344"
};

} // namespace Transport

Q_DECLARE_METATYPE(Transport::State)
Q_DECLARE_METATYPE(QList<Transport::PeerInfo>)

#endif

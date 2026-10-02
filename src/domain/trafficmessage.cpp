#include "domain/trafficmessage.h"

#include <QObject>

namespace Traffic {

QString directionName(Direction direction)
{
    switch (direction) {
    case Direction::Receive: return QObject::tr("RX");
    case Direction::Transmit: return QObject::tr("TX");
    case Direction::State: return QObject::tr("状态");
    case Direction::Warning: return QObject::tr("警告");
    case Direction::Error: return QObject::tr("错误");
    }
    return {};
}

QString originName(Origin origin)
{
    switch (origin) {
    case Origin::Automatic: return QObject::tr("接收");
    case Origin::Manual: return QObject::tr("手动");
    case Origin::Scheduled: return QObject::tr("定时");
    case Origin::Forward: return QObject::tr("转发");
    case Origin::Sample: return QObject::tr("示例");
    }
    return {};
}

QString stateName(State state)
{
    switch (state) {
    case State::None: return {};
    case State::Requested: return QObject::tr("已提交");
    case State::Admitted: return QObject::tr("已准入");
    case State::Writing: return QObject::tr("写入中");
    case State::LocalCompleted: return QObject::tr("本地完成");
    case State::Partial: return QObject::tr("部分写出");
    case State::Rejected: return QObject::tr("已拒绝");
    case State::Cancelled: return QObject::tr("已取消");
    case State::Failed: return QObject::tr("失败");
    }
    return {};
}

QString framingName(Framing framing)
{
    switch (framing) {
    case Framing::Stream: return QObject::tr("流片段");
    case Framing::Datagram: return QObject::tr("数据报");
    case Framing::Message: return QObject::tr("完整消息");
    }
    return {};
}

Source makeSource(const PortDraft &draft, int index)
{
    Source source;
    source.endpointId = draft.id;
    source.endpointIndex = index;
    source.endpointKind = draft.kind;
    return source;
}

Message makeSampleMessage(quint64 id, const QDateTime &timestamp, const Source &source, Direction direction,
    const QByteArray &raw, Framing framing)
{
    Message message;
    message.id = id;
    message.timestamp = timestamp;
    // 单调时钟留 0：示例数据没有真实时序可言，填一个假值只会让排序看起来有依据。
    message.direction = direction;
    message.origin = Origin::Sample;
    message.framing = framing;
    message.source = source;
    message.raw = raw;
    message.rawLength = raw.size();
    return message;
}

} // namespace Traffic

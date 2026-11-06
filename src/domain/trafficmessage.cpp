#include "domain/trafficmessage.h"

#include <QObject>
#include <QString>

namespace Traffic {

QString directionName(Direction direction)
{
    // 前缀和文件日志共用这些标记，保持 ASCII，不跟界面语言走。
    switch (direction) {
    case Direction::Receive: return QStringLiteral("RX");
    case Direction::Transmit: return QStringLiteral("TX");
    case Direction::Info: return QStringLiteral("I");
    case Direction::Warning: return QStringLiteral("W");
    case Direction::Error: return QStringLiteral("E");
    case Direction::State: return QStringLiteral("S");
    }
    return {};
}

QString originName(Origin origin)
{
    switch (origin) {
    case Origin::Automatic: return QStringLiteral("AUTO");
    case Origin::Manual: return QStringLiteral("MAN");
    case Origin::Scheduled: return QStringLiteral("TIMER");
    case Origin::Forward: return QStringLiteral("FWD");
    case Origin::Sample: return QStringLiteral("SAMPLE");
    case Origin::Automation: return QStringLiteral("AI");
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

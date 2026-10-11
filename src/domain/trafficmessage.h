#ifndef TRAFFICTMESSAGE_H
#define TRAFFICTMESSAGE_H

#include "domain/portdraft.h"

#include <QByteArray>
#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QUuid>

// 监视、过滤、显示与文件记录共用的消息值类型。
//
// 分层约定：这里只有事实数据，没有控件、没有渲染、没有缓存。任何"怎么显示"的
// 决定（列开关、格式化、搜索命中）都属于 UI 层——原始字节是唯一事实依据，
// 派生文本一律现算，不写回消息。这样切换显示格式、增删显示列都不会改动数据本身，
// 也不会让两条同样的消息因为显示设置不同而无法比较。
//
// 来源、时间、原始字节与展示事件共用此类型；当前消费者需要这些信息一起传递。

namespace Traffic {

// 消息方向。State/Warning/Error 不是字节流，记录的是端口状态迁移与离散事件，
// 同样占用显示行，但 raw 为空、统计不计入收发字节。
// 与参考实现（BLE 调试工具 TransmissionLogType）的分级对应：State ≈ info
// （连接建立、自动重连完成），Warning ≈ 可观察但不中止的降级（显示预览裁剪、
// 队列溢出丢弃、重连中），Error ≈ error（写入失败、编码失败）。
// TX/RX 之外借用 Direction 承载离散事件，避免为它们单开一套平行类型。
enum class Direction
{
    Receive,
    Transmit,
    State,
    Warning,
    Error
};

// 消息从哪来。与 SendRequest::origin 对齐：转发产生的 TX 不该和用户手动
// 发送的 TX 在界面上无法区分，Sample 用来标识界面预览数据（不计入统计）。
enum class Origin
{
    Automatic, // 真实接收 / 状态迁移
    Manual,    // 用户手动发送
    Scheduled, // 周期或顺序任务
    Forward,   // 路由转发
    Sample     // 界面示例数据
};

// 字节边界语义。这一项决定一条消息能不能被当成"完整的一条"来阅读：
// 串口与 TCP 是字节流，readyRead 片段不等于协议帧；UDP 一次接收就是一个报文。
// 不显式区分，界面会把流片段显示成完整消息，关键词过滤也会在错误的边界上匹配。
enum class Framing
{
    Stream,    // 字节流片段，可能与同源前后片段同属一条消息
    Datagram,  // 完整数据报（UDP）
    Message    // 由协议解析或分包器确定的完整消息
};

// 发送状态。非 Transmit 方向恒为 None。
enum class State
{
    None,
    Requested,     // 已提交，尚未确认准入
    Admitted,      // 已通过容量准入，等待写入
    Writing,       // 正在写入
    LocalCompleted,// 本地写出完成，不代表远端应用已收到
    Partial,       // 部分字节写出后失败
    Rejected,      // 被准入策略拒绝
    Cancelled,     // 用户取消或端口关闭
    Failed         // I/O 错误
};

// 消息来源。"是谁产生的这些字节"这一维度独立于方向：
// 同一个 A 端点既可能收到数据，也可能回显它刚转发的数据。
struct Source
{
    QUuid endpointId;         // 端点身份；为空表示尚未归属任何端点
    int endpointIndex = -1;   // 0 = A，1 = B；监视区前缀用它画端口字母，文件日志用它拼 "A · 串口"
    int endpointKind = PortDraft::Serial; // PortDraft::Kind
    quint64 generation = 0;   // 端点代次：重开或换类型后自增，用来丢弃旧连接的残留事件
    QString endpointName;     // 配置名，如 "串口 A"；可为空
    QUuid peerId;             // TCP Server 的单个连接身份；无连接概念时为空
    QString peerAddress;      // "127.0.0.1:52344"；UDP 来源地址同样记在这里

    bool isValid() const { return !endpointId.isNull(); }
    bool hasPeer() const { return !peerId.isNull() || !peerAddress.isEmpty(); }
};

// 一条消息。字段分组即关注点分组：身份、时间、语义、边界、关联、字节、展示。
struct Message
{
    // ---- 身份与时间 ----
    quint64 id = 0;          // 会话内单调递增，UI 稳定 key，筛选与排序都靠它而不是行号
    QDateTime timestamp;     // 墙钟，展示用
    qint64 monotonicNs = 0;  // 单调时钟，排序、速率与超时用；不受系统时间调整影响

    // ---- 语义 ----
    Direction direction = Direction::Receive;
    Origin origin = Origin::Automatic;
    State state = State::None;

    // ---- 来源 ----
    Source source;

    // ---- 边界 ----
    Framing framing = Framing::Stream;
    bool hasMoreFragments = false;  // 后面还有同源片段；组帧器在收齐后回填

    // ---- 字节 ----
    QByteArray raw;        // 保留的原始字节，可能是完整消息也可能被截断
    qint64 rawLength = 0;  // 事件的真实字节数；与 raw.size() 不等即发生了截断
    bool truncated = false;

    // ---- 展示辅助 ----
    // 自由文本，只放机器放不进去的说明：分包器名称、错误码含义、停止原因。
    // 能用枚举表达的一律用枚举，别往这里塞可枚举的状态。
    QString statusText;

    bool isValid() const { return id != 0 || !raw.isEmpty() || !source.endpointId.isNull(); }
    bool isData() const { return direction == Direction::Receive || direction == Direction::Transmit; }
    // 显示的字节数：真实长度，不是被截断后剩下的长度。
    qint64 displayBytes() const { return rawLength; }
    // 跨片段时才成立：这条不是一条独立完整的消息。
    bool isFragment() const { return framing == Framing::Stream; }
};

// 由端口草稿派生消息来源。行号决定 "A/B" 身份，类型来自草稿，peer 与地址留空——
// 连接建立后由传输层补齐。放在 domain 是因为它是纯值构造，没有控件依赖。
Source makeSource(const PortDraft &draft, int index);

// 中文名称，供列显示、导出与筛选器使用；经 tr() 保留翻译能力。
// 日志前缀和文件列共用的 ASCII 标记：RX/TX/STATE/WARN/ERR，AUTO/MAN/TIMER/FWD/SAMPLE。
// 不走翻译，避免前缀里出现中文。
QString directionName(Direction direction);
QString originName(Origin origin);
QString stateName(State state);
QString framingName(Framing framing);

// 界面示例数据专用：标记为 Sample 方向与来源，UI 据此提示"不计入收发统计"。
// 放在 domain 是因为它同样是一条合法的消息，只是 origin 不同——
// 这样示例数据也能跑通与真实数据完全相同的筛选与导出路径。
Message makeSampleMessage(quint64 id, const QDateTime &timestamp, const Source &source, Direction direction,
    const QByteArray &raw, Framing framing = Framing::Message);

} // namespace Traffic

Q_DECLARE_METATYPE(Traffic::Message)
Q_DECLARE_METATYPE(Traffic::Source)

#endif

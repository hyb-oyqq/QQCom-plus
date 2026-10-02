#ifndef RECEIVEFRAMING_H
#define RECEIVEFRAMING_H

#include <QByteArray>
#include <QMetaType>

// 接收组帧配置：把字节流片段重组成"完整的一条"（Traffic::Framing::Message）的策略。
// 它是端点的接收配置，不是消息的属性——消息只携带组帧结果（Framing 枚举），
// 本结构决定结果怎么产生。串口与 TCP 是字节流，readyRead 片段不等于协议帧；
// UDP 报文自带边界，不经过本配置直接就是 Framing::Datagram。
//
// 与参考实现（BLE 调试工具"接收组帧"设置）对齐的三种完整帧判定：
// 仅兜底输出、结束符拆分、长度前缀（u16 小端）。
//
// 兜底约定：无论选哪种判定，缓冲中的不完整数据都会在空闲超时达到或缓冲字节数
// 达到阈值时输出，保证慢速/无定界协议的数据不会无限滞留、内存不会无限增长。
struct ReceiveFraming
{
    // 完整帧判定。Passthrough 即"仅兜底输出"：收到什么出什么，不做任何拆分。
    enum Mode
    {
        Passthrough = 0,
        Delimiter,        // 结束符拆分：遇到结束符即输出一帧；其后字节作为下一帧继续缓冲
        LengthPrefixU16LE // 长度前缀：帧头 2 字节小端表示后续 payload 字节数，达到该长度即输出
    };

    Mode mode = Passthrough;
    QByteArray delimiter;           // mode == Delimiter 时的结束符字节序列
    int idleTimeoutMs = 100;        // 空闲输出超时：超过该时长无新数据即输出缓冲
    int bufferThresholdBytes = 300; // 缓冲输出阈值：缓冲达到该字节数即强制输出
    // 以下两项对齐参考实现：帧内容是否携带定界结构本身。
    // Delimiter 拆分默认剥离结束符（协议帧通常不含定界符）；
    // 长度前缀默认保留帧头（下游解析器常需要它定位 payload）。
    bool includeDelimiter = false;  // 输出帧是否包含结束符本身（仅 Delimiter）
    bool includeLengthPrefix = true;// 输出帧是否包含 2 字节长度前缀（仅 LengthPrefixU16LE）

    bool isValid() const
    {
        if (idleTimeoutMs <= 0 || bufferThresholdBytes <= 0) return false;
        if (mode == Delimiter) return !delimiter.isEmpty();
        return true;
    }

    bool operator==(const ReceiveFraming &other) const
    {
        return mode == other.mode && delimiter == other.delimiter
            && idleTimeoutMs == other.idleTimeoutMs
            && bufferThresholdBytes == other.bufferThresholdBytes
            && includeDelimiter == other.includeDelimiter
            && includeLengthPrefix == other.includeLengthPrefix;
    }

    bool operator!=(const ReceiveFraming &other) const { return !(*this == other); }
};

Q_DECLARE_METATYPE(ReceiveFraming)

#endif

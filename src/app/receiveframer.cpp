#include "app/receiveframer.h"

#include <QCoreApplication>

namespace {

QString tr(const char *text) { return QCoreApplication::translate("ReceiveFramer", text); }

// 小端 16 位长度前缀：帧头两字节表示后续 payload 字节数。
int prefixLength(const QByteArray &buffer, qsizetype offset)
{
    return int(quint8(buffer.at(offset))) | (int(quint8(buffer.at(offset + 1))) << 8);
}

} // namespace

ReceiveFramer::ReceiveFramer(const ReceiveFraming &config) : m_config(config) {}

void ReceiveFramer::setConfig(const ReceiveFraming &config)
{
    // 换判定方式即丢弃缓冲：半帧在另一种判定下没有意义。
    m_config = config;
    reset();
}

void ReceiveFramer::reset()
{
    m_buffer.clear();
    m_scanFrom = 0;
}

QString ReceiveFramer::triggerText(Trigger trigger, int bytes)
{
    switch (trigger) {
    case Boundary:
        return tr("按定界收齐 %1 字节").arg(bytes);
    case Threshold:
        return tr("缓冲达到阈值，强制输出 %1 字节").arg(bytes);
    case Idle:
        return tr("空闲超时，强制输出 %1 字节").arg(bytes);
    case Closed:
        return tr("端口关闭，输出残留 %1 字节").arg(bytes);
    }
    return {};
}

QList<ReceiveFramer::Frame> ReceiveFramer::takeByThreshold()
{
    QList<Frame> frames;
    if (m_buffer.size() < m_config.bufferThresholdBytes) return frames;
    return flush(Threshold);
}

QList<ReceiveFramer::Frame> ReceiveFramer::feed(const QByteArray &chunk)
{
    QList<Frame> frames;
    // 配置不合法（含"结束符模式却没有结束符"）时退回直通：绝不把数据吞在缓冲里。
    const bool usable = m_config.isValid() && m_config.mode != ReceiveFraming::Passthrough;
    if (!usable) {
        if (chunk.isEmpty()) return frames;
        Frame frame;
        frame.bytes = chunk;
        frame.trigger = Boundary; // 直通：收到即完整
        frames.append(frame);
        return frames;
    }

    if (chunk.isEmpty()) return frames;
    m_buffer += chunk;
    if (m_config.mode == ReceiveFraming::IdleTimeout) {
        // 空闲模式的正常输出由 EndpointRuntime 的单次定时器触发；
        // 这里仅处理长度兜底，避免单次输入过大时缓冲无限增长。
        return takeByThreshold();
    }
    // 一批只消费一次缓冲头部；未匹配的旧前缀下次无需重新扫描。
    qsizetype consumed = 0;
    for (;;) {
        if (m_config.mode == ReceiveFraming::Delimiter) {
            const qsizetype at = m_buffer.indexOf(m_config.delimiter, m_scanFrom);
            if (at < 0) {
                m_scanFrom = qMax(consumed, m_buffer.size() - m_config.delimiter.size() + 1);
                break;
            }
            const qsizetype end = at + m_config.delimiter.size();
            const qsizetype taken = (m_config.includeDelimiter ? end : at) - consumed;
            Frame frame;
            frame.bytes = m_buffer.mid(consumed, taken);
            frame.trigger = Boundary;
            consumed = end;
            m_scanFrom = consumed;
            frames.append(frame);
            continue;
        }
        // 长度前缀：帧头不足 2 字节就继续等；长度 0 的空帧也要收（占位帧），
        // 否则会陷入"永远等第 3 字节"的死循环。
        if (m_buffer.size() - consumed < 2) break;
        const int payload = prefixLength(m_buffer, consumed);
        if (m_buffer.size() - consumed < 2 + payload) break;
        Frame frame;
        frame.bytes = m_config.includeLengthPrefix ? m_buffer.mid(consumed, 2 + payload)
                                                   : m_buffer.mid(consumed + 2, payload);
        frame.trigger = Boundary;
        consumed += 2 + payload;
        frames.append(frame);
    }
    if (consumed > 0) m_buffer.remove(0, consumed);
    m_scanFrom = qMax<qsizetype>(0, m_scanFrom - consumed);
    frames += takeByThreshold();
    return frames;
}

QList<ReceiveFramer::Frame> ReceiveFramer::flush(Trigger trigger)
{
    QList<Frame> frames;
    if (m_buffer.isEmpty()) return frames;
    Frame frame;
    frame.bytes = m_buffer;
    frame.complete = false;
    frame.trigger = trigger;
    if (m_config.logHints) frame.hint = triggerText(trigger, m_buffer.size());
    reset();
    frames.append(frame);
    return frames;
}

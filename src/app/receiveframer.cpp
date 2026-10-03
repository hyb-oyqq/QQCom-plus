#include "app/receiveframer.h"

#include <QCoreApplication>

namespace {

QString tr(const char *text) { return QCoreApplication::translate("ReceiveFramer", text); }

// 小端 16 位长度前缀：帧头两字节表示后续 payload 字节数。
int prefixLength(const QByteArray &buffer)
{
    return int(quint8(buffer.at(0))) | (int(quint8(buffer.at(1))) << 8);
}

} // namespace

ReceiveFramer::ReceiveFramer(const ReceiveFraming &config) : m_config(config) {}

void ReceiveFramer::setConfig(const ReceiveFraming &config)
{
    // 换判定方式即丢弃缓冲：半帧在另一种判定下没有意义。
    m_config = config;
    m_buffer.clear();
}

void ReceiveFramer::reset()
{
    m_buffer.clear();
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
    Frame frame;
    frame.bytes = m_buffer;
    frame.complete = false;
    frame.trigger = Threshold;
    frame.hint = tr("缓冲达到 %1 字节阈值，强制输出（未等到定界）").arg(m_config.bufferThresholdBytes);
    m_buffer.clear();
    frames.append(frame);
    return frames;
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

    m_buffer += chunk;
    // 一次可能收进多帧，循环吐完；阈值输出在循环内就地生效。
    for (;;) {
        if (m_config.mode == ReceiveFraming::Delimiter) {
            const int at = m_buffer.indexOf(m_config.delimiter);
            if (at < 0) {
                frames += takeByThreshold();
                break;
            }
            const int taken = m_config.includeDelimiter ? at + m_config.delimiter.size() : at;
            Frame frame;
            frame.bytes = m_buffer.left(taken);
            frame.trigger = Boundary;
            m_buffer.remove(0, at + m_config.delimiter.size());
            frames.append(frame);
            continue;
        }
        // 长度前缀：帧头不足 2 字节就继续等；长度 0 的空帧也要收（占位帧），
        // 否则会陷入"永远等第 3 字节"的死循环。
        if (m_buffer.size() < 2) break;
        const int payload = prefixLength(m_buffer);
        if (m_buffer.size() < 2 + payload) {
            frames += takeByThreshold();
            break;
        }
        Frame frame;
        frame.bytes = m_config.includeLengthPrefix ? m_buffer.left(2 + payload)
                                                   : m_buffer.mid(2, payload);
        frame.trigger = Boundary;
        m_buffer.remove(0, 2 + payload);
        frames.append(frame);
    }
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
    frame.hint = triggerText(trigger, m_buffer.size());
    m_buffer.clear();
    frames.append(frame);
    return frames;
}

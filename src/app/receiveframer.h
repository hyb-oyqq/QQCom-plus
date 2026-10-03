#ifndef RECEIVEFRAMER_H
#define RECEIVEFRAMER_H

#include <QByteArray>
#include <QString>
#include <QList>
#include "domain/receiveframing.h"

// 接收组帧器：把字节流片段重组成"完整的一条"。
//
// 只做一件事——吃进片段、吐出帧，不碰传输对象、不碰界面、不持有时钟：
// 空闲输出由调用方（EndpointRuntime 的定时器）触发 flush，缓冲阈值输出在 feed 里
// 就地完成。这样它可以在 core 测试里被完整驱动，不需要线程与事件循环。
//
// 三种判定（与 ReceiveFraming::Mode 一一对应）：
//   Passthrough        收到什么出什么，只做兜底（UDP/数据报也不经过它）；
//   Delimiter          遇到结束符即出一帧，其后字节作为下一帧继续缓冲；
//   LengthPrefixU16LE  帧头 2 字节小端 = 后续字节数，收够即出一帧。
//
// 共同兜底：缓冲达到阈值就地输出，空闲超时由调用方 flush 输出——
// 慢速协议不会无限滞留，缓冲也不会无界增长。
//
// complete 字段区分"按判定收齐的完整帧"与"兜底切出来的片段"：
// 前者是 Framing::Message，后者仍是 Framing::Stream（可能还有后续片段）。
class ReceiveFramer
{
public:
    // 输出触发原因，只用于提示日志与显示，不参与相等性判定。
    enum Trigger
    {
        Boundary,  // 按结束符/长度前缀判定收齐
        Threshold, // 缓冲达到阈值强制输出
        Idle,      // 空闲超时（调用方 flush）
        Closed     // 端点关闭时的收尾输出
    };

    struct Frame
    {
        QByteArray bytes;
        bool complete = true; // true=Framing::Message；false=兜底片段
        Trigger trigger = Boundary;
        QString hint;        // 提示日志文本；为空表示无需提示
    };

    explicit ReceiveFramer(const ReceiveFraming &config = {});

    void setConfig(const ReceiveFraming &config); // 换配置即丢弃缓冲：两种判定的缓冲内容不可混用
    const ReceiveFraming &config() const { return m_config; }
    int pendingBytes() const { return m_buffer.size(); }

    // 喂入一段字节流，返回本次判定出的帧（可能 0 条、也可能多条：一次收到多帧）。
    QList<Frame> feed(const QByteArray &chunk);
    // 空闲超时或端点关闭：把缓冲内容按兜底输出（缓冲为空则返回空列表）。
    QList<Frame> flush(Trigger trigger);
    void reset();

    static QString triggerText(Trigger trigger, int bytes);

private:
    // 缓冲达到阈值：整段按兜底输出，complete=false。
    QList<Frame> takeByThreshold();

    ReceiveFraming m_config;
    QByteArray m_buffer;
};

#endif

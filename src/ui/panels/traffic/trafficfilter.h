#ifndef TRAFFICFILTER_H
#define TRAFFICFILTER_H

#include "domain/trafficmessage.h"
#include "ui/common/payloadcodec.h"
#include <QSet>
#include <QStringList>

// 过滤判定的唯一实现：文本显示在消息到达时调用它，条件由调用方持有。
// 全部条件都作用在结构化字段上：端点/方向来自消息本身，
// 内容条件则对同一份原始字节做多种视图的匹配，所见即可搜。
class TrafficFilter
{
public:
    // 内容条件是字面匹配（大小写可选），构造时统一去除首尾空白。
    class TextCondition
    {
    public:
        TextCondition() = default;
        TextCondition(const QString &literal, Qt::CaseSensitivity sensitivity);
        bool matches(const QString &text) const;
        bool isEmpty() const;
    private:
        QString m_literal;
        Qt::CaseSensitivity m_sensitivity = Qt::CaseInsensitive;
    };

    struct Options
    {
        QSet<QUuid> endpoints; // 集合为空表示不显示任何端点的记录
        // 桥接状态不属于 A 或 B。只要 A、B 有一端在显示，这条日志就留下。
        bool rx = true;
        bool tx = true;
        bool forwards = true; // 桥接转发产生的发送行。关掉只影响显示，不停止转发。
        bool infos = true;    // 信息：对端出现、客户端接入、组帧说明
        bool warnings = true; // 警告日志
        bool errors = true;   // 错误日志
        bool states = true;   // 状态：端口打开/关闭，桥接建立/停止

        // 顶栏显示开关和显示设置里的事件开关：点了就作用到已有行。
        // 关键词、排除词、大小写仍等「应用过滤」。
        void copyLiveVisibilityFrom(const Options &current)
        {
            endpoints = current.endpoints;
            rx = current.rx;
            tx = current.tx;
            forwards = current.forwards;
            infos = current.infos;
            warnings = current.warnings;
            errors = current.errors;
            states = current.states;
        }
        bool liveVisibilityDiffers(const Options &other) const
        {
            return endpoints != other.endpoints || rx != other.rx || tx != other.tx
                || forwards != other.forwards || infos != other.infos
                || warnings != other.warnings || errors != other.errors
                || states != other.states;
        }
        // 内容条件：关键词任一命中即通过（组内 OR），排除词命中任一即隐藏（黑名单）。
        // 两者共用同一套候选文本视图，见 Matcher。
        QStringList keywords;
        QStringList excludes;
        bool caseSensitive = false;

        // 面板用它判断"条件是否变了、视图是否还和条件一致"。
        bool operator==(const Options &other) const
        {
            return endpoints == other.endpoints && rx == other.rx && tx == other.tx
                && forwards == other.forwards
                && infos == other.infos && warnings == other.warnings
                && errors == other.errors && states == other.states
                && keywords == other.keywords && excludes == other.excludes
                && caseSensitive == other.caseSensitive;
        }
        bool operator!=(const Options &other) const { return !(*this == other); }
    };

    // 条件快照：候选文本按成本排序并逐项短路，
    // 最贵的两条 HEX 串在多数消息上根本不会被构造（热路径性能红线，见设计稿 §6）。
    class Matcher
    {
    public:
        Matcher() = default;
        Matcher(const Options &options, PayloadCodec::Format format);
        bool accepts(const Traffic::Message &record) const;
        // 供摘要与持久化复用；Matcher 持有的是构造时的条件快照。
        const Options &options() const { return m_options; }
    private:
        Options m_options;
        PayloadCodec::Format m_format = PayloadCodec::Format::Hex;
        QList<TextCondition> m_keywords;
        QList<TextCondition> m_excludes;
    };

    // 一次性判定，等价于 Matcher(options, format).accepts(record)：供测试与低频场景使用。
    // 热路径必须自己构造一次 Matcher 反复调用，不要每条消息都走这里。
    static bool accepts(const Traffic::Message &record, const Options &options, PayloadCodec::Format format);
};

#endif

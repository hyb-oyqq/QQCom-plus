#include "trafficfilter.h"

TrafficFilter::TextCondition::TextCondition(const QString &literal, Qt::CaseSensitivity sensitivity)
    : m_literal(literal.trimmed()), m_sensitivity(sensitivity)
{
}

bool TrafficFilter::TextCondition::isEmpty() const
{
    return m_literal.isEmpty();
}

bool TrafficFilter::TextCondition::matches(const QString &text) const
{
    return !m_literal.isEmpty() && text.contains(m_literal, m_sensitivity);
}

TrafficFilter::Matcher::Matcher(const Options &options, PayloadCodec::Format format)
    : m_options(options), m_format(format)
{
    // 条件的解释只在这里发生一次：词表 → 预编译条件，热路径不再碰字符串表。
    const auto sensitivity = options.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    for (const auto &keyword : options.keywords) {
        TextCondition condition(keyword, sensitivity);
        if (!condition.isEmpty()) m_keywords.append(condition);
    }
    for (const auto &exclude : options.excludes) {
        TextCondition condition(exclude, sensitivity);
        if (!condition.isEmpty()) m_excludes.append(condition);
    }
}

bool TrafficFilter::Matcher::accepts(const Traffic::Message &r) const
{
    // 端点与方向是消息自带的事实字段，直接按结构判等，不解析显示文本。
    // 桥接状态没有端点身份。A、B 只要有一端还在显示，这条日志就该留下。
    if (m_options.endpoints.isEmpty()
        || (r.source.endpointIndex >= 0 && !m_options.endpoints.contains(r.source.endpointId)))
        return false;
    switch (r.direction) {
    case Traffic::Direction::Receive:
        if (!m_options.rx) return false;
        break;
    case Traffic::Direction::Transmit:
        if (!m_options.tx) return false;
        if (r.origin == Traffic::Origin::Forward && !m_options.forwards) return false;
        break;
    case Traffic::Direction::State:
        if (!m_options.states) return false;
        break;
    case Traffic::Direction::Warning:
        if (!m_options.warnings) return false;
        break;
    case Traffic::Direction::Error:
        if (!m_options.errors) return false;
        break;
    }
    if (m_keywords.isEmpty() && m_excludes.isEmpty()) return true;

    enum Verdict { KeepLooking, Decided, Rejected };
    bool keywordHit = m_keywords.isEmpty(); // 没写关键词 = 这一维不限
    bool rejected = false;
    const auto scan = [this, &keywordHit](const QString &text) {
        // 排除词优先于关键词：命中任一即否决，不再往下找。
        for (const auto &exclude : m_excludes)
            if (exclude.matches(text)) return Rejected;
        if (!keywordHit) {
            for (const auto &keyword : m_keywords) {
                if (keyword.matches(text)) {
                    keywordHit = true;
                    break;
                }
            }
        }
        // 关键词已满足、且没有排除词要继续查：后面更贵的视图（含两条 HEX 串）不必再构造。
        return keywordHit && m_excludes.isEmpty() ? Decided : KeepLooking;
    };
    // 返回 true 表示"已有结论"，调用方立即返回；scan 内部已经短路，
    // 这里不重算任何一遍匹配。
    const auto settled = [&scan, &rejected](const QString &text) {
        switch (scan(text)) {
        case KeepLooking:
            return false;
        case Decided:
            return true;
        default:
            rejected = true;
            return true;
        }
    };

    // 候选文本按"便宜 → 昂贵"排序，逐项短路。结构化字段与渲染文本先上，
    // 只有它们全都没给结论时才构造 HEX 串——常见关键词（"心跳"、对端地址）
    // 在到达时就命中，最贵的两条 HEX 根本不构造。
    if (settled(r.source.peerAddress)) return !rejected;
    if (settled(r.source.endpointName)) return !rejected;
    if (settled(r.statusText)) return !rejected;
    const QString rendered = PayloadCodec::render(r.raw, m_format);
    if (settled(rendered)) return !rejected;
    if (m_format != PayloadCodec::Format::Utf8
        && settled(PayloadCodec::render(r.raw, PayloadCodec::Format::Utf8)))
        return !rejected;
    // 两种 HEX 写法都算所见即可搜：紧凑写法与带空格写法。
    if (settled(QString::fromLatin1(r.raw.toHex()))) return !rejected;
    if (settled(QString::fromLatin1(r.raw.toHex(' ')))) return !rejected;
    // 全部视图扫完仍没命中关键词：淘汰。
    return !rejected && keywordHit;
}

bool TrafficFilter::accepts(const Traffic::Message &r, const Options &options, PayloadCodec::Format format)
{
    // 一次性判定：等价于构造一次 Matcher 再判一条。热路径（消息到达）必须自己
    // 持有 Matcher，避免每条消息重新整理词表。
    return Matcher(options, format).accepts(r);
}

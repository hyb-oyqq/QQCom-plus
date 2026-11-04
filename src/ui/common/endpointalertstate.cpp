#include "ui/common/endpointalertstate.h"

#include <algorithm>

namespace {
constexpr int BlinkDurationMs = 5000;
constexpr int BlinkStepMs = 500;
constexpr int StableRecoveryMs = 2000;
constexpr int MaxEntries = 16;
}

EndpointAlertState::EndpointAlertState(QObject *parent) : QObject(parent)
{
    m_clock.start();
    m_timer.setInterval(BlinkStepMs);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &EndpointAlertState::tick);
}

EndpointAlertState::Level EndpointAlertState::level() const
{
    if (m_fault && !m_faultAcknowledged) return Error;
    return std::any_of(m_entries.cbegin(), m_entries.cend(),
        [](const Entry &entry) { return entry.unread; }) ? Warning : Normal;
}

bool EndpointAlertState::blinking() const
{
    return m_blinkStart >= 0 && m_clock.elapsed() - m_blinkStart < BlinkDurationMs;
}

bool EndpointAlertState::bright() const
{
    return !blinking() || ((m_clock.elapsed() - m_blinkStart) / BlinkStepMs) % 2 == 0;
}

QString EndpointAlertState::summary() const
{
    for (auto it = m_entries.crbegin(); it != m_entries.crend(); ++it) {
        if (m_fault && (it->issue != Traffic::Issue::EndpointFailure || it->resolved || it->stopped))
            continue;
        if (!m_fault && !it->unread) continue;
        const QString state = it->resolved ? tr("已恢复")
            : m_fault ? tr("故障未恢复") : tr("待查看警告");
        return tr("%1：%2\n最近发生 %3 · 累计 %4 次\n激活窗口或点击界面可消除图标提醒")
            .arg(state, it->reason, it->lastSeen.toString(QStringLiteral("HH:mm:ss")))
            .arg(it->count);
    }
    return {};
}

void EndpointAlertState::report(Traffic::Issue issue, const QString &reason)
{
    if (issue == Traffic::Issue::None || reason.trimmed().isEmpty()) return;
    const bool fatal = issue == Traffic::Issue::EndpointFailure;
    const bool newIncident = fatal && (!m_fault || m_userAttempt);
    auto found = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry &entry) {
        return !newIncident && entry.issue == issue && entry.reason == reason && !entry.resolved && !entry.stopped
            && (entry.unread || (fatal && m_fault));
    });
    if (newIncident) m_faultAcknowledged = false;
    const bool newAttention = found == m_entries.end() && !(fatal && m_faultAcknowledged);
    if (found != m_entries.end()) {
        auto updated = *found;
        ++updated.count;
        updated.lastSeen = QDateTime::currentDateTime();
        m_entries.erase(found);
        m_entries.append(updated);
        // 已查看的持续故障重试只增加次数，不重新置未读或重启闪烁。
    } else {
        if (m_entries.size() == MaxEntries) {
            const auto expendable = std::find_if(m_entries.begin(), m_entries.end(), [&](const Entry &entry) {
                return !m_fault || entry.issue != Traffic::Issue::EndpointFailure || entry.resolved || entry.stopped;
            });
            m_entries.erase(expendable == m_entries.end() ? m_entries.begin() : expendable);
        }
        Entry entry;
        entry.issue = issue;
        entry.reason = reason;
        entry.lastSeen = QDateTime::currentDateTime();
        entry.unread = !(fatal && m_faultAcknowledged);
        m_entries.append(entry);
    }
    if (fatal) {
        m_fault = true;
        m_userAttempt = false;
        m_recoverAt = m_open ? m_clock.elapsed() + StableRecoveryMs : -1;
    }
    if (newAttention) m_blinkStart = m_clock.elapsed();
    syncTimer();
    emit changed();
    if (newAttention) emit attentionRequested();
}

void EndpointAlertState::setConnectionOpen(bool open)
{
    if (m_open == open) return;
    m_open = open;
    if (open) m_userAttempt = false;
    m_recoverAt = open && m_fault ? m_clock.elapsed() + StableRecoveryMs : -1;
    syncTimer();
}

void EndpointAlertState::acknowledge()
{
    for (auto &entry : m_entries) entry.unread = false;
    if (m_fault) m_faultAcknowledged = true;
    m_blinkStart = -1;
    syncTimer();
    emit changed();
}

void EndpointAlertState::beginUserAttempt()
{
    acknowledge();
    // 用户新点的一次连接失败要重新提醒；自动恢复仍属于已确认的同一故障。
    m_userAttempt = true;
}

void EndpointAlertState::stopByUser()
{
    for (auto &entry : m_entries) {
        entry.unread = false;
        if (entry.issue == Traffic::Issue::EndpointFailure && !entry.resolved)
            entry.stopped = true;
    }
    m_fault = false;
    m_faultAcknowledged = false;
    m_userAttempt = false;
    m_open = false;
    m_blinkStart = m_recoverAt = -1;
    syncTimer();
    emit changed();
}

void EndpointAlertState::tick()
{
    if (m_recoverAt >= 0 && m_clock.elapsed() >= m_recoverAt) {
        m_fault = false;
        m_faultAcknowledged = false;
        m_recoverAt = -1;
        m_blinkStart = -1;
        for (auto &entry : m_entries)
            if (entry.issue == Traffic::Issue::EndpointFailure && !entry.stopped)
                entry.resolved = true;
    }
    if (!blinking()) m_blinkStart = -1;
    syncTimer();
    emit changed();
}

void EndpointAlertState::syncTimer()
{
    if (blinking() || m_recoverAt >= 0) {
        if (!m_timer.isActive()) m_timer.start();
    } else {
        m_timer.stop();
    }
}

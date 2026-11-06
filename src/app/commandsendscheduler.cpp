#include "commandsendscheduler.h"
#include <limits>

namespace {
constexpr int RunTailHoldMs = 700;
constexpr int LoopGapMs = 50;
}

CommandSendScheduler::CommandSendScheduler(QObject *parent) : QObject(parent)
{
    m_timer.setParent(this);
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &CommandSendScheduler::advance);
}

void CommandSendScheduler::start(const QList<int> &rows, const QList<CommandItem> &commands, bool loop)
{
    stop();
    if (commands.isEmpty() || rows.size() != commands.size()) return;
    m_rows = rows;
    m_commands = commands;
    m_next = 0;
    m_loop = loop;
    m_phase = Phase::Command;
    m_dueMs = qMax(0, commands.first().delayMs);
    m_clock.start();
    m_active = true;
    const auto generation = m_generation;
    emit runStarted();
    if (m_active && generation == m_generation) arm();
}

void CommandSendScheduler::stop()
{
    ++m_generation;
    m_timer.stop();
    const bool wasActive = m_active;
    m_active = false;
    m_rows.clear();
    m_commands.clear();
    if (wasActive) emit finished();
}

void CommandSendScheduler::arm()
{
    // 使用单调时钟的绝对截止时间，避免逐条处理耗时累积为发送间隔漂移。
    const auto remaining = qMax(qint64(0), m_dueMs - m_clock.elapsed());
    m_timer.start(int(qMin(remaining, qint64(std::numeric_limits<int>::max()))));
}

void CommandSendScheduler::advance()
{
    if (!m_active) return;
    if (m_clock.elapsed() < m_dueMs) {
        arm();
        return;
    }
    if (m_phase == Phase::Tail) {
        stop();
        return;
    }
    if (m_phase == Phase::LoopGap) {
        m_next = 0;
        m_phase = Phase::Command;
        m_dueMs = m_clock.elapsed() + qMax(0, m_commands.first().delayMs);
        const auto generation = m_generation;
        emit runStarted();
        if (m_active && generation == m_generation) arm();
        return;
    }

    const auto command = m_commands.at(m_next);
    const int row = m_rows.at(m_next++);
    const auto generation = m_generation;
    emit commandReady(row, command);
    // 发送信号可能同步触发停止或启动新任务，旧任务不能继续排下一拍。
    if (!m_active || generation != m_generation) return;
    if (m_next < m_commands.size()) {
        m_dueMs += qMax(0, m_commands.at(m_next).delayMs);
    } else {
        m_phase = m_loop ? Phase::LoopGap : Phase::Tail;
        m_dueMs += m_loop ? qMax(LoopGapMs, command.delayMs) : RunTailHoldMs;
    }
    arm();
}

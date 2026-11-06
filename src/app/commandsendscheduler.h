#ifndef COMMANDSENDSCHEDULER_H
#define COMMANDSENDSCHEDULER_H

#include "domain/commanditem.h"
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QTimer>

// GUI 线程的发送时序器：整批只持有一个定时器，停止时立即撤销待发送工作。
class CommandSendScheduler : public QObject
{
    Q_OBJECT
public:
    explicit CommandSendScheduler(QObject *parent = nullptr);
    bool isActive() const { return m_active; }
    void start(const QList<int> &rows, const QList<CommandItem> &commands, bool loop);
    void stop();

signals:
    void runStarted();
    void commandReady(int row, const CommandItem &command);
    void finished();

private:
    void arm();
    void advance();
    enum class Phase { Command, LoopGap, Tail };
    QTimer m_timer;
    QElapsedTimer m_clock;
    QList<int> m_rows;
    QList<CommandItem> m_commands;
    qsizetype m_next = 0;
    qint64 m_dueMs = 0;
    quint64 m_generation = 0;
    Phase m_phase = Phase::Command;
    bool m_loop = false;
    bool m_active = false;
};

#endif

#ifndef ENDPOINTALERTSTATE_H
#define ENDPOINTALERTSTATE_H

#include "domain/trafficmessage.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QTimer>

// GUI 线程持有。连接、故障和未查看记录分别管理，不依赖数据区过滤或日志容量。
class EndpointAlertState : public QObject
{
    Q_OBJECT
public:
    enum Level { Normal, Warning, Error };
    struct Entry {
        Traffic::Issue issue = Traffic::Issue::None;
        QString reason;
        QDateTime lastSeen;
        quint64 count = 1;
        bool unread = true;
        bool resolved = false;
        bool stopped = false;
    };

    explicit EndpointAlertState(QObject *parent = nullptr);
    Level level() const;
    bool bright() const;
    bool blinking() const;
    bool hasFault() const { return m_fault; }
    const QList<Entry> &entries() const { return m_entries; }
    QString summary() const;

    void report(Traffic::Issue issue, const QString &reason);
    void setConnectionOpen(bool open);
    void acknowledge();
    void beginUserAttempt();
    void stopByUser();

signals:
    void changed();
    void attentionRequested();

private:
    void tick();
    void syncTimer();
    QList<Entry> m_entries;
    QElapsedTimer m_clock;
    QTimer m_timer;
    bool m_fault = false;
    bool m_faultAcknowledged = false;
    bool m_userAttempt = false;
    bool m_open = false;
    qint64 m_blinkStart = -1;
    qint64 m_recoverAt = -1;
};

#endif

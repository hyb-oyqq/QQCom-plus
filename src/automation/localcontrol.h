#ifndef LOCALCONTROL_H
#define LOCALCONTROL_H

#include <QJsonArray>
#include <QJsonObject>
#include <QLocalServer>
#include <functional>

namespace LocalControl {
QString directory();
// Only live named pipes are returned; stale instance files do not select a target.
QJsonArray instances(const QString &directory = {}, const QString &selector = {});
QJsonObject call(const QJsonObject &instance, const QString &tool,
                 const QJsonObject &arguments, int timeoutMs = 5000);
}

// Same-user local control connects the CLI to a running window.
// The GUI and CLI execute the same validated application tools.
class LocalControlServer final : public QObject
{
    Q_OBJECT
public:
    using Handler = std::function<QJsonObject(const QString &, const QJsonObject &)>;
    explicit LocalControlServer(const QString &version, QObject *parent = nullptr);
    ~LocalControlServer() override;
    void setHandler(Handler handler);
    bool start(const QString &directory = {});
    void stop();
    bool isListening() const { return m_server.isListening(); }
    QJsonObject descriptor() const;
    QString errorString() const { return m_error; }

private:
    void acceptConnections();
    QLocalServer m_server;
    Handler m_handler;
    QString m_version;
    QString m_id;
    QString m_manifest;
    QString m_startedAt;
    QString m_error;
};

#endif

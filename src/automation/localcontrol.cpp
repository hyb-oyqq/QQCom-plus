#include "localcontrol.h"
#include "capabilitycatalog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <memory>
#include <utility>

namespace {
constexpr qsizetype RequestLimit = 1024 * 1024;
constexpr qsizetype ResponseLimit = 8 * 1024 * 1024;

QByteArray json(const QJsonObject &value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }
}

namespace LocalControl {
QString directory()
{
    const auto override = qEnvironmentVariable("QQCOM_CONTROL_DIR");
    return override.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/ai"
                             : QDir(override).absolutePath();
}

QJsonArray instances(const QString &directory, const QString &selector)
{
    const QDir root(directory.isEmpty() ? LocalControl::directory() : directory);
    QJsonArray result;
    const auto files = root.entryList({"control-*.json"}, QDir::Files, QDir::Name);
    for (const auto &name : files) {
        QFile file(root.filePath(name));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 8192) continue;
        const auto instance = QJsonDocument::fromJson(file.readAll()).object();
        const auto id = instance.value("instanceId").toString();
        const auto pipe = instance.value("pipe").toString();
        if (instance.value("application") != "QQCom-plus" || instance.value("apiVersion").toInt() != 1
            || QUuid(id).isNull() || pipe != "qqcom-plus-control-" + id) continue;
        if (!selector.isEmpty() && selector != id
            && selector != QString::number(instance.value("pid").toInteger())) continue;
        QLocalSocket probe;
        probe.connectToServer(pipe);
        if (!probe.waitForConnected(100)) continue;
        probe.abort();
        result.append(instance);
    }
    return result;
}

QJsonObject call(const QJsonObject &instance, const QString &tool, const QJsonObject &arguments, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    const auto remaining = [&] { return qMax(0, timeoutMs - int(clock.elapsed())); };
    const auto fail = [](const QString &message) { return CapabilityCatalog::failure("control_unavailable", message); };
    QLocalSocket socket;
    socket.connectToServer(instance.value("pipe").toString());
    if (!socket.waitForConnected(remaining())) return fail(socket.errorString());
    const auto request = json({{"apiVersion", 1}, {"instanceId", instance.value("instanceId")},
        {"tool", tool}, {"arguments", arguments}}) + '\n';
    if (request.size() > RequestLimit) return CapabilityCatalog::failure("request_too_large", "Request exceeds 1 MiB");
    if (socket.write(request) != request.size()) return fail(socket.errorString());
    while (socket.bytesToWrite() > 0)
        if (remaining() == 0 || !socket.waitForBytesWritten(remaining()))
            return fail("Request delivery was interrupted; an operation may already have been submitted. Do not blindly retry writes.");
    QByteArray buffer;
    for (;;) {
        buffer += socket.readAll();
        if (buffer.size() > ResponseLimit) return fail("Response exceeds 8 MiB");
        const auto end = buffer.indexOf('\n');
        if (end >= 0) {
            QJsonParseError error;
            const auto response = QJsonDocument::fromJson(buffer.left(end), &error).object();
            if (error.error != QJsonParseError::NoError || response.value("instanceId") != instance.value("instanceId"))
                return fail("Invalid response or instance identity changed");
            if (!response.value("result").isObject()) return fail("Missing application result");
            return response.value("result").toObject();
        }
        if (remaining() == 0 || !socket.waitForReadyRead(remaining())) {
            if (socket.bytesAvailable() > 0) continue;
            return fail("No response from QQCom-plus before timeout; an operation may already have been submitted. Do not blindly retry writes.");
        }
    }
}
}

LocalControlServer::LocalControlServer(const QString &version, QObject *parent)
    : QObject(parent), m_server(this), m_version(version),
      m_id(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    connect(&m_server, &QLocalServer::newConnection, this, &LocalControlServer::acceptConnections);
}

LocalControlServer::~LocalControlServer() { stop(); }
void LocalControlServer::setHandler(Handler handler) { m_handler = std::move(handler); }

QJsonObject LocalControlServer::descriptor() const
{
    return {{"application", "QQCom-plus"}, {"apiVersion", 1}, {"version", m_version},
        {"pid", QCoreApplication::applicationPid()}, {"instanceId", m_id},
        {"pipe", "qqcom-plus-control-" + m_id}, {"startedAt", m_startedAt},
        {"executable", QCoreApplication::applicationFilePath()}};
}

bool LocalControlServer::start(const QString &directory)
{
    if (isListening()) return true;
    // Restrict the Windows named pipe to the current user account.
    m_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_server.listen("qqcom-plus-control-" + m_id)) { m_error = m_server.errorString(); return false; }
    const QDir root(directory.isEmpty() ? LocalControl::directory() : directory);
    if (!QDir().mkpath(root.absolutePath())) {
        m_error = "Cannot create local control discovery directory"; m_server.close(); return false;
    }
    m_startedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const auto path = root.filePath("control-" + m_id + ".json");
    QSaveFile manifest(path);
    const auto content = json(descriptor());
    if (!manifest.open(QIODevice::WriteOnly) || manifest.write(content) != content.size() || !manifest.commit()) {
        m_error = "Cannot write local control instance descriptor"; m_server.close(); return false;
    }
    m_manifest = path;
    return true;
}

void LocalControlServer::stop()
{
    m_server.close();
    const auto sockets = findChildren<QLocalSocket *>(QString(), Qt::FindDirectChildrenOnly);
    for (auto *socket : sockets) socket->abort();
    if (!m_manifest.isEmpty()) QFile::remove(m_manifest);
    m_manifest.clear();
}

void LocalControlServer::acceptConnections()
{
    while (m_server.hasPendingConnections()) {
        auto *socket = m_server.nextPendingConnection();
        socket->setParent(this);
        connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        if (socket->state() != QLocalSocket::ConnectedState) { socket->deleteLater(); continue; }
        if (findChildren<QLocalSocket *>(QString(), Qt::FindDirectChildrenOnly).size() > 16) {
            socket->abort(); continue;
        }
        socket->setReadBufferSize(RequestLimit + 1);
        auto *timeout = new QTimer(socket);
        timeout->setSingleShot(true);
        timeout->start(5000);
        connect(timeout, &QTimer::timeout, socket, &QLocalSocket::abort);
        auto buffer = std::make_shared<QByteArray>();
        const auto read = [this, socket, timeout, buffer] {
            if (socket->property("handled").toBool()) return;
            buffer->append(socket->readAll());
            if (buffer->size() > RequestLimit) { socket->abort(); return; }
            const auto end = buffer->indexOf('\n');
            if (end < 0) return;
            socket->setProperty("handled", true);
            timeout->stop();
            QJsonParseError parseError;
            const auto request = QJsonDocument::fromJson(buffer->left(end), &parseError).object();
            QJsonObject result;
            if (parseError.error != QJsonParseError::NoError || request.value("apiVersion").toInt() != 1
                || request.value("instanceId").toString() != m_id
                || !request.value("tool").isString() || !request.value("arguments").isObject()) {
                result = CapabilityCatalog::failure("invalid_request", "Expected API version, instance identity, tool and arguments");
            } else {
                const auto tool = request.value("tool").toString();
                const auto arguments = request.value("arguments").toObject();
                const auto validation = CapabilityCatalog::validateArguments(tool, arguments);
                result = !validation.isEmpty() ? CapabilityCatalog::failure("invalid_arguments", validation)
                    : m_handler ? m_handler(tool, arguments) : CapabilityCatalog::failure("unavailable", "Application not ready");
            }
            socket->write(json({{"instanceId", m_id}, {"result", result}}) + '\n');
            socket->disconnectFromServer();
        };
        connect(socket, &QLocalSocket::readyRead, this, read);
        read(); // Data can already be buffered before newConnection is delivered.
    }
}

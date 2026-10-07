#ifndef UPDATESERVICE_H
#define UPDATESERVICE_H

#include <QObject>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QProgressDialog;

class UpdateService final : public QObject
{
    Q_OBJECT

public:
    explicit UpdateService(QString currentVersion, QObject *parent = nullptr);

    void checkForUpdates(const QUrl &manifestUrl);

private:
    void handleManifest(QNetworkReply *reply);
    void startDownload(const QUrl &url, const QString &version, qint64 expectedSize,
                       const QByteArray &expectedSha256);
    void finishDownload(QNetworkReply *reply);
    void fail(const QString &message);
    QString updaterScriptPath() const;

    QNetworkAccessManager *m_network;
    QString m_currentVersion;
    QProgressDialog *m_progress = nullptr;
    QString m_downloadPath;
    qint64 m_expectedSize = 0;
    QByteArray m_expectedSha256;
    bool m_busy = false;
};

#endif // UPDATESERVICE_H

#ifndef UPDATESERVICE_H
#define UPDATESERVICE_H

#include "updatemanifest.h"

#include <QObject>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QProgressDialog;
class QSaveFile;
class QTimer;

class UpdateService final : public QObject
{
    Q_OBJECT

public:
    enum class CheckMode { Automatic, Manual };

    explicit UpdateService(QString currentVersion, QObject *parent = nullptr);

    void checkForUpdates(const QUrl &manifestUrl, CheckMode mode = CheckMode::Automatic);

signals:
    // 不打断当前操作的提示。空文本表示清掉状态栏上的“正在检查”。
    void advisory(const QString &text, int milliseconds);

private:
    void handleManifest(QNetworkReply *reply);
    void startDownload(const UpdateOffer &offer);
    void finishDownload(QNetworkReply *reply);
    void discardDownload();
    void armStallTimer(int milliseconds);
    void stopStallTimer();
    void abortStalledTransfer();
    // quietOnAutomatic：启动检查遇到网络或清单问题时只走状态栏，不弹窗。
    void fail(const QString &message, bool quietOnAutomatic = false);
    QString updaterScriptPath() const;
    QString formatSize(qint64 size) const;

    QNetworkAccessManager *m_network;
    QNetworkReply *m_reply = nullptr;
    QTimer *m_stallTimer = nullptr;
    QString m_currentVersion;
    CheckMode m_mode = CheckMode::Automatic;
    QProgressDialog *m_progress = nullptr;
    QSaveFile *m_downloadFile = nullptr;
    QString m_downloadPath;
    qint64 m_expectedSize = 0;
    QByteArray m_expectedSha256;
    bool m_busy = false;
    bool m_downloadWriteFailed = false;
    bool m_stallAborted = false;
};

#endif // UPDATESERVICE_H

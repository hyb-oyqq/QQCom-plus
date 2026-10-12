#include "updateservice.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkProxyQuery>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressBar>
#include <QProgressDialog>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

#include <algorithm>

namespace {

// 清单检查必须很快结束。下载连续 10 秒没有新字节就视为断流：Gitee 不接受 Range，
// 只能丢掉已下内容并从头再试。次数用尽才向用户报失败。
constexpr int ManifestTimeoutMs = 10 * 1000;
constexpr int DownloadStallMs = 10 * 1000;
constexpr int DownloadMaxAttempts = 4;
constexpr int DownloadRetryPauseMs = 400;
constexpr int StatusNoteMs = 8 * 1000;

QNetworkRequest httpsRequest(const QUrl &url, const QString &userAgent, int timeoutMs)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    // VPN 隧道上 Qt 默认的 HTTP/2 容易把流控停住，进度随机卡住。清单和安装包都走 HTTP/1.1。
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setTransferTimeout(timeoutMs);
    return request;
}

// 和更新器共用这一份日志。下载卡住时进度框没有原因，打开这个文件就能看到停在哪。
QString updateLogPath()
{
    const QString overridePath = qEnvironmentVariable("QQCOM_UPDATE_LOG");
    if (!overridePath.isEmpty()) return overridePath;
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation)
           + QStringLiteral("/QQCom-update/update.log");
}

QMutex &updateLogMutex()
{
    static QMutex mutex;
    return mutex;
}

// 界面只报状态。时间、字节数、HTTP 码和 Qt 错误串写入 update.log，不给用户看。
void logUpdateProblem(const QString &detail)
{
    if (detail.isEmpty()) return;
    qWarning().noquote() << "QQCom update:" << detail;
    const QString path = updateLogPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QString line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                         + QLatin1Char(' ') + detail + QLatin1Char('\n');
    const QMutexLocker locker(&updateLogMutex());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    file.write(line.toUtf8());
}

// 重定向地址上的 token 不写进日志。
QString safeUrl(const QUrl &url)
{
    QUrl copy = url;
    copy.setUserInfo({});
    copy.setQuery({});
    copy.setFragment({});
    return copy.toString();
}

QString proxySummary(const QList<QNetworkProxy> &proxies)
{
    if (proxies.isEmpty()) return QStringLiteral("none");
    QStringList parts;
    for (const QNetworkProxy &proxy : proxies) {
        const QString endpoint = proxy.hostName().isEmpty()
                                     ? QString()
                                     : QStringLiteral(" %1:%2").arg(proxy.hostName()).arg(proxy.port());
        switch (proxy.type()) {
        case QNetworkProxy::NoProxy: parts << QStringLiteral("direct"); break;
        case QNetworkProxy::DefaultProxy: parts << QStringLiteral("default"); break;
        case QNetworkProxy::Socks5Proxy: parts << QStringLiteral("socks5") + endpoint; break;
        case QNetworkProxy::HttpProxy: parts << QStringLiteral("http") + endpoint; break;
        case QNetworkProxy::HttpCachingProxy: parts << QStringLiteral("http-cache") + endpoint; break;
        case QNetworkProxy::FtpCachingProxy: parts << QStringLiteral("ftp-cache") + endpoint; break;
        }
    }
    return parts.join(QStringLiteral(", "));
}

void logReplyProblem(QNetworkReply *reply, bool stalled)
{
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QStringList parts;
    if (stalled) parts << QStringLiteral("stalled");
    parts << QStringLiteral("qt %1").arg(int(reply->error()));
    if (http > 0) parts << QStringLiteral("HTTP %1").arg(http);
    if (reply->error() != QNetworkReply::NoError && !reply->errorString().isEmpty())
        parts << reply->errorString();
    else if (reply->error() == QNetworkReply::NoError && !isAllowedUpdateUrl(reply->url()))
        parts << QStringLiteral("final URL is not an allowed update URL");
    logUpdateProblem(parts.join(QLatin1Char(' ')));
}

// 只给更新用。Qt 默认不读系统代理，VPN 开着时代理会拦住直连，下载停在随机进度。
class SystemProxyFactory final : public QNetworkProxyFactory
{
public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery &query) override
    {
        const QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery(query);
        logUpdateProblem(QStringLiteral("proxy %1 -> %2")
                             .arg(safeUrl(query.url()), proxySummary(proxies)));
        if (proxies.isEmpty()) return {QNetworkProxy(QNetworkProxy::NoProxy)};
        return proxies;
    }
};

// 不调用 exec()：事件循环继续跑，超时定时器才能把卡住的传输掐掉。
void showNotice(QMessageBox::Icon icon, const QString &message)
{
    auto *box = new QMessageBox(icon, QObject::tr("QQCom 更新"), message, QMessageBox::Ok);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowModality(Qt::NonModal);
    box->show();
    box->raise();
}

} // namespace

UpdateService::UpdateService(QString currentVersion, QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_currentVersion(std::move(currentVersion))
{
    m_network->setProxyFactory(new SystemProxyFactory);
}

void UpdateService::checkForUpdates(const QUrl &manifestUrl, CheckMode mode)
{
    if (m_busy) {
        emit advisory(tr("正在检查更新"), StatusNoteMs);
        return;
    }
    m_mode = mode;
    if (!isAllowedUpdateUrl(manifestUrl)) {
        logUpdateProblem(QStringLiteral("manifest URL is not an allowed update URL"));
        const bool automatic = mode == CheckMode::Automatic;
        fail(automatic ? tr("检查更新失败") : tr("检查更新失败。"), automatic);
        return;
    }

    m_busy = true;
    m_stallAborted = false;
    logUpdateProblem(QStringLiteral("check %1 as %2")
                         .arg(safeUrl(manifestUrl), m_currentVersion));
    emit advisory(tr("正在检查更新"), ManifestTimeoutMs + StatusNoteMs);
    QNetworkRequest request = httpsRequest(manifestUrl,
                                           QStringLiteral("QQCom/%1").arg(m_currentVersion),
                                           ManifestTimeoutMs);
    // contents 接口按 Accept 决定返回 JSON 还是网页。不声明的话解出来的不是清单。
    request.setRawHeader("Accept", "application/json");
    m_reply = m_network->get(request);
    armStallTimer(ManifestTimeoutMs);
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        stopStallTimer();
        if (!reply) return;
        handleManifest(reply);
        reply->deleteLater();
    });
}

void UpdateService::handleManifest(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError || !isAllowedUpdateUrl(reply->url())) {
        logReplyProblem(reply, m_stallAborted);
        m_stallAborted = false;
        const bool automatic = m_mode == CheckMode::Automatic;
        fail(automatic ? tr("检查更新失败") : tr("检查更新失败。"), automatic);
        return;
    }
    m_stallAborted = false;

    UpdateOffer offer;
    QString reason;
    const ManifestStatus status = evaluateUpdateManifest(unwrapManifestBody(reply->readAll()),
                                                         m_currentVersion, &offer, &reason);
    if (status == ManifestStatus::NotNewer) {
        m_busy = false;
        logUpdateProblem(QStringLiteral("manifest not newer than %1").arg(m_currentVersion));
        emit advisory(tr("已是最新版本"), StatusNoteMs);
        if (m_mode == CheckMode::Manual)
            showNotice(QMessageBox::Information, tr("已是最新版本。"));
        return;
    }
    if (status != ManifestStatus::UpdateAvailable) {
        m_busy = false;
        logUpdateProblem(reason);
        if (m_mode == CheckMode::Automatic) {
            emit advisory(tr("检查更新失败"), StatusNoteMs);
            return;
        }
        fail(tr("检查更新失败。"), false);
        return;
    }

    logUpdateProblem(QStringLiteral("manifest offers %1 size %2")
                         .arg(offer.version)
                         .arg(offer.size));
    emit advisory(QString(), 1);
    if (!m_acceptWithoutPrompt) {
        const QString text = tr("发现新版本 %1（%2）。是否现在更新？")
                                 .arg(offer.version, formatSize(offer.size));
        QWidget *host = qobject_cast<QWidget *>(parent());
        QMessageBox box(QMessageBox::Information, tr("发现新版本"), text,
                        QMessageBox::Yes | QMessageBox::No, host ? host : QApplication::activeWindow());
        if (!offer.notesUrl.isEmpty()) box.setInformativeText(offer.notesUrl);
        if (box.exec() != QMessageBox::Yes) {
            m_busy = false;
            return;
        }
    }
    startDownload(offer);
}

void UpdateService::setAcceptUpdatesWithoutPrompt(bool accept)
{
    m_acceptWithoutPrompt = accept;
}

void UpdateService::startDownload(const UpdateOffer &offer)
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                         + QStringLiteral("/QQCom-update/") + offer.version;
    if (!QDir().mkpath(root)) {
        fail(tr("无法保存更新文件。"));
        return;
    }
    m_downloadPath = root + QStringLiteral("/QQCom-%1-win64.zip").arg(offer.version);
    m_expectedSize = offer.size;
    m_expectedSha256 = offer.sha256;
    m_offer = offer;
    m_downloadAttempt = 0;
    m_userCanceledDownload = false;
    m_bytesTotal = offer.size;
    logUpdateProblem(QStringLiteral("download start %1 size %2 log %3")
                         .arg(safeUrl(offer.packageUrl))
                         .arg(offer.size)
                         .arg(updateLogPath()));

    QWidget *host = qobject_cast<QWidget *>(parent());
    m_progress = new QProgressDialog(tr("正在下载更新"), tr("取消"), 0, 100,
                                     host ? host : QApplication::activeWindow());
    m_progress->setWindowTitle(tr("QQCom 更新"));
    // 只挡住主窗口，不挡住整个进程。不 exec()，事件循环仍能处理超时和取消。
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setAutoClose(false);
    m_progress->setAutoReset(false);
    m_progress->setMinimumDuration(0);
    connect(m_progress, &QProgressDialog::canceled, this, [this] {
        logUpdateProblem(QStringLiteral("download canceled received %1").arg(m_bytesReceived));
        m_userCanceledDownload = true;
        stopStallTimer();
        stopDownloadWatch();
        if (m_retryTimer) m_retryTimer->stop();
        m_stallAborted = false;
        if (m_reply) {
            m_reply->abort();
            return;
        }
        closeDownloadProgress();
        discardDownload();
        m_busy = false;
    });
    beginDownloadAttempt();
}

void UpdateService::beginDownloadAttempt()
{
    if (m_userCanceledDownload || !m_progress) return;
    ++m_downloadAttempt;
    m_downloadWriteFailed = false;
    m_stallAborted = false;
    m_bytesReceived = 0;
    m_nextProgressLog = 0;
    m_lastByteMs = QDateTime::currentMSecsSinceEpoch();
    logUpdateProblem(QStringLiteral("download attempt %1/%2")
                         .arg(m_downloadAttempt)
                         .arg(DownloadMaxAttempts));
    QFile::remove(m_downloadPath);

    m_downloadFile = new QSaveFile(m_downloadPath, this);
    if (!m_downloadFile->open(QIODevice::WriteOnly)) {
        discardDownload();
        closeDownloadProgress();
        fail(tr("无法保存更新文件。"));
        return;
    }

    // 先接上信号再显示。模态进度框一旦 setValue() 就会 processEvents()，
    // 不能在下载回调里走那条路径。
    m_reply = m_network->get(httpsRequest(m_offer.packageUrl,
                                          QStringLiteral("QQCom/%1").arg(m_currentVersion),
                                          DownloadStallMs));
    armStallTimer(DownloadStallMs);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (!m_downloadFile || !m_reply) return;
        const QByteArray chunk = m_reply->readAll();
        if (!chunk.isEmpty() && m_downloadFile->write(chunk) != chunk.size()) {
            m_downloadWriteFailed = true;
            m_reply->abort();
        }
    });
    connect(m_reply, &QNetworkReply::redirected, this, [](const QUrl &url) {
        logUpdateProblem(QStringLiteral("redirect %1").arg(safeUrl(url)));
    });
    connect(m_reply, &QNetworkReply::metaDataChanged, this, [this] {
        if (!m_reply) return;
        const int http = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        logUpdateProblem(QStringLiteral("response HTTP %1 %2")
                             .arg(http)
                             .arg(safeUrl(m_reply->url())));
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        armStallTimer(DownloadStallMs);
        noteDownloadBytes(received, total);
        updateDownloadProgress(received, total);
    });
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        stopStallTimer();
        if (!reply) return;
        finishDownload(reply);
        reply->deleteLater();
    });
    if (!m_watchTimer) {
        m_watchTimer = new QTimer(this);
        m_watchTimer->setInterval(5 * 1000);
        connect(m_watchTimer, &QTimer::timeout, this, &UpdateService::logDownloadWatch);
    }
    m_watchTimer->start();
    noteDownloadBytes(0, m_expectedSize);
    updateDownloadProgress(0, m_expectedSize);
    m_progress->show();
    m_progress->raise();
}

void UpdateService::scheduleDownloadRetry(const QString &reason)
{
    if (m_userCanceledDownload || m_downloadAttempt >= DownloadMaxAttempts) return;
    logUpdateProblem(QStringLiteral("retry %1/%2 after %3 received %4")
                         .arg(m_downloadAttempt + 1)
                         .arg(DownloadMaxAttempts)
                         .arg(reason)
                         .arg(m_bytesReceived));
    if (m_progress) {
        m_progress->setLabelText(tr("连接中断，正在重新下载（第 %1/%2 次）")
                                     .arg(m_downloadAttempt + 1)
                                     .arg(DownloadMaxAttempts));
    }
    if (!m_retryTimer) {
        m_retryTimer = new QTimer(this);
        m_retryTimer->setSingleShot(true);
        connect(m_retryTimer, &QTimer::timeout, this, &UpdateService::beginDownloadAttempt);
    }
    m_retryTimer->start(DownloadRetryPauseMs);
}

void UpdateService::closeDownloadProgress()
{
    if (!m_progress) return;
    // 关掉进度框会发出 canceled。挡住这个信号，避免超时或重试被当成用户取消。
    const QSignalBlocker blocker(m_progress);
    m_progress->close();
    m_progress->deleteLater();
    m_progress = nullptr;
}

void UpdateService::noteDownloadBytes(qint64 received, qint64 total)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (received != m_bytesReceived) m_lastByteMs = now;
    m_bytesReceived = received;
    if (total > 0) m_bytesTotal = total;
    if (m_nextProgressLog > 0 && received < m_nextProgressLog) return;
    logUpdateProblem(QStringLiteral("progress %1/%2").arg(received).arg(m_bytesTotal));
    constexpr qint64 step = 1024 * 1024;
    m_nextProgressLog = (received / step + 1) * step;
}

void UpdateService::logDownloadWatch()
{
    const qint64 idleMs = QDateTime::currentMSecsSinceEpoch() - m_lastByteMs;
    logUpdateProblem(QStringLiteral("watch received %1/%2 idle %3 ms")
                         .arg(m_bytesReceived)
                         .arg(m_bytesTotal)
                         .arg(idleMs));
}

void UpdateService::stopDownloadWatch()
{
    if (m_watchTimer) m_watchTimer->stop();
}

void UpdateService::updateDownloadProgress(qint64 received, qint64 total)
{
    if (!m_progress) return;
    const qint64 denominator = total > 0 ? total : m_expectedSize;
    // QProgressDialog::setValue() 在模态时调用 processEvents()。安装包字节由 Qt 的
    // HTTP 线程投递到主线程；在这个回调里再进事件循环，会打乱“还有数据待投递”的计数，
    // HTTP 线程不再往下送，进度就停在随机位置。
    QProgressBar *bar = m_progress->findChild<QProgressBar *>();
    const QString caption = m_downloadAttempt > 1
                                ? tr("正在重新下载（第 %1/%2 次）").arg(m_downloadAttempt).arg(DownloadMaxAttempts)
                                : tr("正在下载更新");
    if (denominator > 0) {
        const int percent = int(std::min<qint64>(99, received * 100 / denominator));
        m_progress->setRange(0, 100);
        if (bar) bar->setValue(percent);
        m_progress->setLabelText(tr("%1\n%2 / %3")
                                     .arg(caption, formatSize(received), formatSize(denominator)));
        return;
    }
    m_progress->setRange(0, 0);
    m_progress->setLabelText(tr("%1\n已下载 %2").arg(caption, formatSize(received)));
}

void UpdateService::finishDownload(QNetworkReply *reply)
{
    stopDownloadWatch();
    if (m_userCanceledDownload) {
        closeDownloadProgress();
        discardDownload();
        m_busy = false;
        return;
    }
    if (m_downloadWriteFailed) {
        discardDownload();
        closeDownloadProgress();
        fail(tr("无法保存更新文件。"));
        return;
    }
    const bool stalled = m_stallAborted || reply->error() == QNetworkReply::TimeoutError;
    m_stallAborted = false;
    if (stalled || reply->error() != QNetworkReply::NoError || !isAllowedUpdateUrl(reply->url())) {
        logReplyProblem(reply, stalled);
        discardDownload();
        if (m_downloadAttempt < DownloadMaxAttempts) {
            scheduleDownloadRetry(stalled ? QStringLiteral("stall") : QStringLiteral("error"));
            return;
        }
        closeDownloadProgress();
        fail(tr("下载失败。"));
        return;
    }
    if (!m_downloadFile) {
        closeDownloadProgress();
        fail(tr("无法保存更新文件。"));
        return;
    }
    const QByteArray rest = reply->readAll();
    if ((!rest.isEmpty() && m_downloadFile->write(rest) != rest.size()) || !m_downloadFile->commit()) {
        discardDownload();
        closeDownloadProgress();
        fail(tr("无法保存更新文件。"));
        return;
    }
    m_downloadFile->deleteLater();
    m_downloadFile = nullptr;

    logUpdateProblem(QStringLiteral("download body received %1").arg(m_bytesReceived));
    if (!packageFileMatches(m_downloadPath, m_expectedSize, m_expectedSha256)) {
        QFile::remove(m_downloadPath);
        logUpdateProblem(QStringLiteral("package size or SHA-256 mismatch"));
        if (m_downloadAttempt < DownloadMaxAttempts) {
            scheduleDownloadRetry(QStringLiteral("mismatch"));
            return;
        }
        closeDownloadProgress();
        fail(tr("更新文件不完整，已取消安装。"));
        return;
    }

    const QString script = updaterScriptPath();
    if (script.isEmpty()) {
        QFile::remove(m_downloadPath);
        logUpdateProblem(QStringLiteral("updater script missing"));
        closeDownloadProgress();
        fail(tr("无法完成更新。"));
        return;
    }
    QStringList args = {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
                        QStringLiteral("Bypass"), QStringLiteral("-WindowStyle"),
                        QStringLiteral("Hidden"), QStringLiteral("-File"), script,
                        QStringLiteral("-ProcessId"), QString::number(QCoreApplication::applicationPid()),
                        QStringLiteral("-Package"), m_downloadPath,
                        QStringLiteral("-InstallDir"), QCoreApplication::applicationDirPath(),
                        QStringLiteral("-Restart"), QCoreApplication::applicationFilePath()};
    const QString logOverride = qEnvironmentVariable("QQCOM_UPDATE_LOG");
    if (!logOverride.isEmpty()) args << QStringLiteral("-LogPath") << logOverride;
    const QString userDataOverride = qEnvironmentVariable("QQCOM_UPDATE_USER_DATA");
    if (!userDataOverride.isEmpty()) args << QStringLiteral("-UserDataDir") << userDataOverride;
    logUpdateProblem(QStringLiteral("package ok, starting updater"));
    if (!QProcess::startDetached(QStringLiteral("powershell.exe"), args)) {
        QFile::remove(m_downloadPath);
        logUpdateProblem(QStringLiteral("failed to start updater"));
        closeDownloadProgress();
        fail(tr("无法完成更新。"));
        return;
    }
    closeDownloadProgress();
    m_busy = false;
    qApp->quit();
}

void UpdateService::discardDownload()
{
    if (m_downloadFile) {
        m_downloadFile->cancelWriting();
        m_downloadFile->deleteLater();
        m_downloadFile = nullptr;
    }
    if (!m_downloadPath.isEmpty()) QFile::remove(m_downloadPath);
}

void UpdateService::armStallTimer(int milliseconds)
{
    if (!m_stallTimer) {
        m_stallTimer = new QTimer(this);
        m_stallTimer->setSingleShot(true);
        connect(m_stallTimer, &QTimer::timeout, this, &UpdateService::abortStalledTransfer);
    }
    m_stallAborted = false;
    m_stallTimer->start(milliseconds);
}

void UpdateService::stopStallTimer()
{
    if (m_stallTimer) m_stallTimer->stop();
}

void UpdateService::abortStalledTransfer()
{
    const qint64 idleMs = QDateTime::currentMSecsSinceEpoch() - m_lastByteMs;
    logUpdateProblem(QStringLiteral("stall abort received %1/%2 idle %3 ms")
                         .arg(m_bytesReceived)
                         .arg(m_bytesTotal)
                         .arg(idleMs));
    m_stallAborted = true;
    if (m_reply) m_reply->abort();
}

void UpdateService::fail(const QString &message, bool quietOnAutomatic)
{
    m_busy = false;
    stopStallTimer();
    stopDownloadWatch();
    if (quietOnAutomatic && m_mode == CheckMode::Automatic) {
        emit advisory(message, StatusNoteMs);
        return;
    }
    emit advisory(QString(), 1);
    showNotice(QMessageBox::Warning, message);
}

QString UpdateService::updaterScriptPath() const
{
    const QStringList candidates = {
        QCoreApplication::applicationDirPath() + QStringLiteral("/qqcom-updater.ps1"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/scripts/qqcom-updater.ps1")};
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path)) return QDir::toNativeSeparators(path);
    }
    return {};
}

QString UpdateService::formatSize(qint64 size) const
{
    if (size < 1024 * 1024) return tr("%1 KB").arg(size / 1024.0, 0, 'f', 1);
    return tr("%1 MB").arg(size / (1024.0 * 1024.0), 0, 'f', 1);
}

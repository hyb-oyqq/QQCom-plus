#include "updateservice.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressDialog>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

#include <algorithm>

namespace {

// 清单检查必须很快结束。下载允许更长的静默，但只要这段时间没有任何字节，就中止，
// 避免断网时进度框或“正在检查”一直挂着。
constexpr int ManifestTimeoutMs = 10 * 1000;
constexpr int DownloadStallMs = 20 * 1000;
constexpr int StatusNoteMs = 8 * 1000;

QNetworkRequest httpsRequest(const QUrl &url, const QString &userAgent, int timeoutMs)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(timeoutMs);
    return request;
}

// 界面只报状态。HTTP 码、Qt 错误串和清单字段问题写到调试输出，不给用户看。
void logUpdateProblem(const QString &detail)
{
    if (!detail.isEmpty()) qWarning().noquote() << "QQCom update:" << detail;
}

void logReplyProblem(QNetworkReply *reply, bool stalled)
{
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QStringList parts;
    if (stalled) parts << QStringLiteral("stalled");
    if (http > 0) parts << QStringLiteral("HTTP %1").arg(http);
    if (reply->error() != QNetworkReply::NoError && !reply->errorString().isEmpty())
        parts << reply->errorString();
    else if (reply->error() == QNetworkReply::NoError && !isHttpsUrl(reply->url()))
        parts << QStringLiteral("final URL is not HTTPS");
    logUpdateProblem(parts.join(QLatin1Char(' ')));
}

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
}

void UpdateService::checkForUpdates(const QUrl &manifestUrl, CheckMode mode)
{
    if (m_busy) {
        emit advisory(tr("正在检查更新"), StatusNoteMs);
        return;
    }
    m_mode = mode;
    if (!isHttpsUrl(manifestUrl)) {
        logUpdateProblem(QStringLiteral("manifest URL is not HTTPS"));
        const bool automatic = mode == CheckMode::Automatic;
        fail(automatic ? tr("检查更新失败") : tr("检查更新失败。"), automatic);
        return;
    }

    m_busy = true;
    m_stallAborted = false;
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
    if (reply->error() != QNetworkReply::NoError || !isHttpsUrl(reply->url())) {
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

    emit advisory(QString(), 1);
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
    startDownload(offer);
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
    m_downloadWriteFailed = false;
    m_stallAborted = false;
    QFile::remove(m_downloadPath);

    m_downloadFile = new QSaveFile(m_downloadPath, this);
    if (!m_downloadFile->open(QIODevice::WriteOnly)) {
        discardDownload();
        fail(tr("无法保存更新文件。"));
        return;
    }

    QWidget *host = qobject_cast<QWidget *>(parent());
    m_reply = m_network->get(httpsRequest(offer.packageUrl,
                                          QStringLiteral("QQCom/%1").arg(m_currentVersion),
                                          DownloadStallMs));
    armStallTimer(DownloadStallMs);
    m_progress = new QProgressDialog(tr("正在下载更新"), tr("取消"), 0, 100,
                                     host ? host : QApplication::activeWindow());
    m_progress->setWindowTitle(tr("QQCom 更新"));
    // 只挡住主窗口，不挡住整个进程。不 exec()，事件循环仍能处理超时和取消。
    m_progress->setWindowModality(Qt::WindowModal);
    m_progress->setAutoClose(false);
    m_progress->setMinimumDuration(0);
    m_progress->setValue(0);
    connect(m_progress, &QProgressDialog::canceled, this, [this] {
        stopStallTimer();
        m_stallAborted = false;
        if (m_reply) m_reply->abort();
    });
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (!m_downloadFile || !m_reply) return;
        const QByteArray chunk = m_reply->readAll();
        if (!chunk.isEmpty() && m_downloadFile->write(chunk) != chunk.size()) {
            m_downloadWriteFailed = true;
            m_reply->abort();
        }
    });
    connect(m_reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        armStallTimer(DownloadStallMs);
        if (!m_progress) return;
        const qint64 denominator = total > 0 ? total : m_expectedSize;
        if (denominator > 0) {
            m_progress->setRange(0, 100);
            m_progress->setValue(int(std::min<qint64>(99, received * 100 / denominator)));
            m_progress->setLabelText(tr("正在下载更新\n%1 / %2")
                                         .arg(formatSize(received), formatSize(denominator)));
        } else {
            m_progress->setRange(0, 0);
            m_progress->setLabelText(tr("正在下载更新\n已下载 %1").arg(formatSize(received)));
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = m_reply;
        m_reply = nullptr;
        stopStallTimer();
        if (!reply) return;
        finishDownload(reply);
        reply->deleteLater();
    });
}

void UpdateService::finishDownload(QNetworkReply *reply)
{
    // 关掉进度框会发出 canceled。先挡住这个信号，否则超时中止会被当成用户取消。
    if (m_progress) {
        const QSignalBlocker blocker(m_progress);
        m_progress->close();
        m_progress->deleteLater();
        m_progress = nullptr;
    }
    if (m_downloadWriteFailed) {
        discardDownload();
        fail(tr("无法保存更新文件。"));
        return;
    }
    if (m_stallAborted || reply->error() == QNetworkReply::TimeoutError) {
        m_stallAborted = false;
        discardDownload();
        logReplyProblem(reply, true);
        fail(tr("下载失败。"));
        return;
    }
    if (reply->error() == QNetworkReply::OperationCanceledError) {
        discardDownload();
        m_busy = false;
        return;
    }
    if (reply->error() != QNetworkReply::NoError || !isHttpsUrl(reply->url())) {
        discardDownload();
        logReplyProblem(reply, false);
        fail(tr("下载失败。"));
        return;
    }
    if (!m_downloadFile) {
        fail(tr("无法保存更新文件。"));
        return;
    }
    const QByteArray rest = reply->readAll();
    if ((!rest.isEmpty() && m_downloadFile->write(rest) != rest.size()) || !m_downloadFile->commit()) {
        discardDownload();
        fail(tr("无法保存更新文件。"));
        return;
    }
    m_downloadFile->deleteLater();
    m_downloadFile = nullptr;

    if (!packageFileMatches(m_downloadPath, m_expectedSize, m_expectedSha256)) {
        QFile::remove(m_downloadPath);
        logUpdateProblem(QStringLiteral("package size or SHA-256 mismatch"));
        fail(tr("更新文件不完整，已取消安装。"));
        return;
    }

    const QString script = updaterScriptPath();
    if (script.isEmpty()) {
        QFile::remove(m_downloadPath);
        logUpdateProblem(QStringLiteral("updater script missing"));
        fail(tr("无法完成更新。"));
        return;
    }
    const QStringList args = {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
                              QStringLiteral("Bypass"), QStringLiteral("-WindowStyle"),
                              QStringLiteral("Hidden"), QStringLiteral("-File"), script,
                              QStringLiteral("-ProcessId"), QString::number(QCoreApplication::applicationPid()),
                              QStringLiteral("-Package"), m_downloadPath,
                              QStringLiteral("-InstallDir"), QCoreApplication::applicationDirPath(),
                              QStringLiteral("-Restart"), QCoreApplication::applicationFilePath()};
    if (!QProcess::startDetached(QStringLiteral("powershell.exe"), args)) {
        QFile::remove(m_downloadPath);
        logUpdateProblem(QStringLiteral("failed to start updater"));
        fail(tr("无法完成更新。"));
        return;
    }
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
    m_stallAborted = true;
    if (m_reply) m_reply->abort();
}

void UpdateService::fail(const QString &message, bool quietOnAutomatic)
{
    m_busy = false;
    stopStallTimer();
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

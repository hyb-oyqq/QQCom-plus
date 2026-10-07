#include "updateservice.h"

#include "version/version.h"

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressDialog>
#include <QSaveFile>
#include <QStandardPaths>
#include <QVersionNumber>

#include <algorithm>

namespace {

bool isHttps(const QUrl &url)
{
    return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0;
}

QByteArray sha256File(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && !file.atEnd()) return {};
        hash.addData(chunk);
    }
    return hash.result();
}

} // namespace

UpdateService::UpdateService(QString currentVersion, QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_currentVersion(std::move(currentVersion))
{
}

void UpdateService::checkForUpdates(const QUrl &manifestUrl)
{
    if (m_busy || !isHttps(manifestUrl)) return;
    m_busy = true;
    QNetworkRequest request(manifestUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("QQCom/%1").arg(m_currentVersion));
    auto *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        handleManifest(reply);
        reply->deleteLater();
    });
}

void UpdateService::handleManifest(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        m_busy = false;
        return; // 自动检查失败不打扰正常启动
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
    const QJsonObject root = document.object();
    const QJsonObject download = root.value(QStringLiteral("download")).toObject();
    const QString version = root.value(QStringLiteral("version")).toString();
    const QUrl packageUrl(download.value(QStringLiteral("url")).toString());
    const qint64 size = download.value(QStringLiteral("size")).toVariant().toLongLong();
    const QByteArray sha256 = QByteArray::fromHex(
        download.value(QStringLiteral("sha256")).toString().toLatin1());
    const QVersionNumber current = QVersionNumber::fromString(m_currentVersion);
    const QVersionNumber candidate = QVersionNumber::fromString(version);
    if (parseError.error != QJsonParseError::NoError || root.value(QStringLiteral("schema")).toInt() != 1
        || root.value(QStringLiteral("product")).toString() != QStringLiteral("QQCom")
        || root.value(QStringLiteral("channel")).toString().isEmpty()
        || candidate.isNull() || current.isNull() || candidate <= current
        || !isHttps(packageUrl) || download.value(QStringLiteral("type")).toString() != QStringLiteral("zip")
        || size <= 0 || sha256.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256)) {
        m_busy = false;
        return;
    }

    const QString notesUrl = root.value(QStringLiteral("notesUrl")).toString();
    const QString text = tr("发现 QQCom %1。\n\n下载大小：%2 MB\n\n现在下载并安装吗？")
                             .arg(version)
                             .arg(size / (1024.0 * 1024.0), 0, 'f', 1);
    QMessageBox box(QMessageBox::Information, tr("发现新版本"), text,
                    QMessageBox::Yes | QMessageBox::No, QApplication::activeWindow());
    if (!notesUrl.isEmpty()) box.setInformativeText(tr("发布说明：%1").arg(notesUrl));
    if (box.exec() != QMessageBox::Yes) {
        m_busy = false;
        return;
    }
    startDownload(packageUrl, version, size, sha256);
}

void UpdateService::startDownload(const QUrl &url, const QString &version, qint64 expectedSize,
                                  const QByteArray &expectedSha256)
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                         + QStringLiteral("/QQCom-update/") + version;
    if (!QDir().mkpath(root)) {
        fail(tr("无法创建更新临时目录。"));
        return;
    }
    m_downloadPath = root + QStringLiteral("/QQCom-%1-win64.zip").arg(version);
    m_expectedSize = expectedSize;
    m_expectedSha256 = expectedSha256;

    auto *reply = m_network->get(QNetworkRequest(url));
    m_progress = new QProgressDialog(tr("正在下载 QQCom %1…").arg(version), tr("取消"), 0, 100,
                                     QApplication::activeWindow());
    m_progress->setWindowTitle(tr("QQCom 更新"));
    m_progress->setAutoClose(false);
    m_progress->setMinimumDuration(0);
    connect(m_progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                if (!m_progress) return;
                const qint64 denominator = total > 0 ? total : m_expectedSize;
                m_progress->setValue(denominator > 0
                                         ? int(std::min<qint64>(99, received * 100 / denominator))
                                         : 0);
            });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        finishDownload(reply);
        reply->deleteLater();
    });
}

void UpdateService::finishDownload(QNetworkReply *reply)
{
    if (m_progress) {
        m_progress->close();
        m_progress->deleteLater();
        m_progress = nullptr;
    }
    if (reply->error() != QNetworkReply::NoError) {
        QFile::remove(m_downloadPath);
        fail(tr("下载更新失败：%1").arg(reply->errorString()));
        return;
    }

    QSaveFile file(m_downloadPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(reply->readAll()) < 0 || !file.commit()) {
        QFile::remove(m_downloadPath);
        fail(tr("无法保存更新文件。"));
        return;
    }
    const QFileInfo info(m_downloadPath);
    if (info.size() != m_expectedSize || sha256File(m_downloadPath) != m_expectedSha256) {
        QFile::remove(m_downloadPath);
        fail(tr("更新文件校验失败，未进行安装。"));
        return;
    }

    const QString script = updaterScriptPath();
    if (script.isEmpty()) {
        QFile::remove(m_downloadPath);
        fail(tr("找不到更新器脚本，请重新安装当前版本。"));
        return;
    }
    const QString installDir = QCoreApplication::applicationDirPath();
    const QString restart = QCoreApplication::applicationFilePath();
    const QStringList args = {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
                              QStringLiteral("Bypass"), QStringLiteral("-File"), script,
                              QStringLiteral("-ProcessId"), QString::number(QCoreApplication::applicationPid()),
                              QStringLiteral("-Package"), m_downloadPath,
                              QStringLiteral("-InstallDir"), installDir,
                              QStringLiteral("-Restart"), restart};
    if (!QProcess::startDetached(QStringLiteral("powershell.exe"), args)) {
        QFile::remove(m_downloadPath);
        fail(tr("无法启动更新器。"));
        return;
    }
    m_busy = false;
    qApp->quit();
}

void UpdateService::fail(const QString &message)
{
    m_busy = false;
    QMessageBox::warning(QApplication::activeWindow(), tr("QQCom 更新"), message);
}

QString UpdateService::updaterScriptPath() const
{
    const QStringList candidates = {
        QCoreApplication::applicationDirPath() + QStringLiteral("/qqcom-updater.ps1"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/scripts/qqcom-updater.ps1")};
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path)) return path;
    }
    return {};
}

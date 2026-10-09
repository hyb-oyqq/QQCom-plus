#include "updatemanifest.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QVersionNumber>

#include <cmath>
#include <limits>

namespace {

bool isReleaseVersion(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^\\d+\\.\\d+\\.\\d+$"));
    return pattern.match(text).hasMatch();
}

bool isSha256Hex(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
    return pattern.match(text).hasMatch();
}

void reject(QString *reason, const QString &text, ManifestStatus *status)
{
    if (reason) *reason = text;
    *status = ManifestStatus::Rejected;
}

} // namespace

QByteArray unwrapManifestBody(const QByteArray &body)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return body;

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("type")).toString() != QStringLiteral("file")) return body;
    if (root.value(QStringLiteral("encoding")).toString() != QStringLiteral("base64")) return body;
    const QJsonValue content = root.value(QStringLiteral("content"));
    if (!content.isString() || content.toString().isEmpty()) return body;

    QByteArray encoded = content.toString().toLatin1();
    encoded.replace('\n', "");
    encoded.replace('\r', "");
    const QByteArray decoded = QByteArray::fromBase64(encoded);
    return decoded.isEmpty() ? body : decoded;
}

bool isHttpsUrl(const QUrl &url)
{
    return url.isValid() && !url.host().isEmpty()
           && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0;
}

bool packageFileMatches(const QString &path, qint64 expectedSize, const QByteArray &expectedSha256)
{
    if (expectedSize <= 0
        || expectedSha256.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256))
        return false;
    QFileInfo info(path);
    if (!info.isFile() || info.size() != expectedSize) return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty()) return false;
        hash.addData(chunk);
    }
    return hash.result() == expectedSha256;
}

ManifestStatus evaluateUpdateManifest(const QByteArray &json, const QString &currentVersion,
                                      UpdateOffer *offer, QString *reason)
{
    if (offer) *offer = {};
    ManifestStatus status = ManifestStatus::Rejected;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        reject(reason, QStringLiteral("清单不是有效的 JSON 对象。"), &status);
        return status;
    }

    const QJsonObject root = document.object();
    const QJsonValue schema = root.value(QStringLiteral("schema"));
    if (!schema.isDouble() || schema.toInt() != 1) {
        reject(reason, QStringLiteral("不支持的清单协议版本。"), &status);
        return status;
    }
    if (root.value(QStringLiteral("product")).toString() != QStringLiteral("QQCom")) {
        reject(reason, QStringLiteral("清单产品不是 QQCom。"), &status);
        return status;
    }
    const QString channel = root.value(QStringLiteral("channel")).toString();
    if (channel != QStringLiteral("stable") && channel != QStringLiteral("beta")) {
        reject(reason, QStringLiteral("清单通道必须是 stable 或 beta。"), &status);
        return status;
    }
    const QString version = root.value(QStringLiteral("version")).toString();
    if (!isReleaseVersion(version) || !isReleaseVersion(currentVersion)) {
        reject(reason, QStringLiteral("版本号必须是主.次.修订。"), &status);
        return status;
    }

    const QJsonValue downloadValue = root.value(QStringLiteral("download"));
    if (!downloadValue.isObject()) {
        reject(reason, QStringLiteral("清单缺少下载对象。"), &status);
        return status;
    }
    const QJsonObject download = downloadValue.toObject();
    if (download.value(QStringLiteral("type")).toString() != QStringLiteral("zip")) {
        reject(reason, QStringLiteral("只接受 zip 发布包。"), &status);
        return status;
    }
    const QUrl packageUrl(download.value(QStringLiteral("url")).toString());
    if (!isHttpsUrl(packageUrl)) {
        reject(reason, QStringLiteral("下载地址必须是 HTTPS。"), &status);
        return status;
    }
    const QJsonValue sizeValue = download.value(QStringLiteral("size"));
    const double rawSize = sizeValue.toDouble(-1);
    if (!sizeValue.isDouble() || rawSize < 1.0 || rawSize != std::floor(rawSize)
        || rawSize > double(std::numeric_limits<qint64>::max())) {
        reject(reason, QStringLiteral("发布包大小无效。"), &status);
        return status;
    }
    const QString shaText = download.value(QStringLiteral("sha256")).toString();
    if (!isSha256Hex(shaText)) {
        reject(reason, QStringLiteral("SHA-256 无效。"), &status);
        return status;
    }

    const QVersionNumber current = QVersionNumber::fromString(currentVersion);
    const QVersionNumber candidate = QVersionNumber::fromString(version);
    if (current.isNull() || candidate.isNull()) {
        reject(reason, QStringLiteral("版本号无法比较。"), &status);
        return status;
    }

    UpdateOffer parsed;
    parsed.version = version;
    parsed.channel = channel;
    parsed.packageUrl = packageUrl;
    parsed.size = qint64(rawSize);
    parsed.sha256 = QByteArray::fromHex(shaText.toLatin1());
    parsed.notesUrl = root.value(QStringLiteral("notesUrl")).toString();
    parsed.mandatory = root.value(QStringLiteral("mandatory")).toBool(false);
    if (parsed.sha256.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256)) {
        reject(reason, QStringLiteral("SHA-256 无效。"), &status);
        return status;
    }
    if (offer) *offer = parsed;

    if (candidate <= current) {
        if (reason) reason->clear();
        return ManifestStatus::NotNewer;
    }
    if (reason) reason->clear();
    return ManifestStatus::UpdateAvailable;
}

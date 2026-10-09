#ifndef UPDATEMANIFEST_H
#define UPDATEMANIFEST_H

#include <QByteArray>
#include <QString>
#include <QUrl>

// 清单判定与发布包校验。与界面、网络无关，供客户端和测试共用。
struct UpdateOffer {
    QString version;
    QString channel;
    QUrl packageUrl;
    qint64 size = 0;
    QByteArray sha256;
    QString notesUrl;
    bool mandatory = false;
};

enum class ManifestStatus {
    UpdateAvailable, // 清单合法，且目标版本高于当前版本
    NotNewer,        // 清单合法，但目标版本不高于当前版本（含降级）
    Rejected         // 协议、产品、地址或校验字段不符合方案
};

ManifestStatus evaluateUpdateManifest(const QByteArray &json, const QString &currentVersion,
                                      UpdateOffer *offer, QString *reason = nullptr);

// Gitee 的 contents 接口把文件正文放在 base64 的 content 字段里，并带换行。
// 正文本身就是清单 JSON 时原样返回，方便清单地址指向一个普通的 JSON 文件。
QByteArray unwrapManifestBody(const QByteArray &body);

bool isHttpsUrl(const QUrl &url);

// 大小与 SHA-256 必须同时相符。文件打不开或不匹配都返回 false。
bool packageFileMatches(const QString &path, qint64 expectedSize, const QByteArray &expectedSha256);

#endif // UPDATEMANIFEST_H

#include "updatemanifest.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QJsonObject downloadObject(const QJsonObject &overrides = {})
{
    QJsonObject download{{QStringLiteral("type"), QStringLiteral("zip")},
                         {QStringLiteral("url"),
                          QStringLiteral("https://gitee.com/qinjmin/qqcom/releases/download/v0.2.0/"
                                         "QQCom-0.2.0-win64.zip")},
                         {QStringLiteral("size"), 128},
                         {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))}};
    for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it)
        download.insert(it.key(), it.value());
    return download;
}

QByteArray manifestJson(const QJsonObject &overrides = {})
{
    QJsonObject root{{QStringLiteral("schema"), 1},
                     {QStringLiteral("product"), QStringLiteral("QQCom")},
                     {QStringLiteral("channel"), QStringLiteral("stable")},
                     {QStringLiteral("version"), QStringLiteral("0.2.0")},
                     {QStringLiteral("mandatory"), false},
                     {QStringLiteral("download"), downloadObject()},
                     {QStringLiteral("notesUrl"),
                      QStringLiteral("https://gitee.com/qinjmin/qqcom/releases/tag/v0.2.0")}};
    for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it)
        root.insert(it.key(), it.value());
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

ManifestStatus evaluate(const QByteArray &json, const QString &current, UpdateOffer *offer = nullptr,
                        QString *reason = nullptr)
{
    return evaluateUpdateManifest(json, current, offer, reason);
}

} // namespace

class UpdateManifestTest final : public QObject
{
    Q_OBJECT

private slots:
    void acceptsNewerStableRelease();
    void acceptsBetaChannelAndUppercaseHash();
    void ignoresSameVersionAndDowngrade();
    void rejectsProtocolAndPackageFields_data();
    void rejectsProtocolAndPackageFields();
    void packageFileMatchesChecksSizeAndHash();
    void unwrapsGiteeContentsPayload();
    void loopbackOriginDoesNotWidenProductionUrls();
};

void UpdateManifestTest::acceptsNewerStableRelease()
{
    UpdateOffer offer;
    QString reason = QStringLiteral("unchanged");
    QCOMPARE(evaluate(manifestJson(), QStringLiteral("0.1.0"), &offer, &reason),
             ManifestStatus::UpdateAvailable);
    QVERIFY(reason.isEmpty());
    QCOMPARE(offer.version, QStringLiteral("0.2.0"));
    QCOMPARE(offer.channel, QStringLiteral("stable"));
    QCOMPARE(offer.packageUrl.toString(),
             QStringLiteral("https://gitee.com/qinjmin/qqcom/releases/download/v0.2.0/"
                            "QQCom-0.2.0-win64.zip"));
    QCOMPARE(offer.size, 128);
    QCOMPARE(offer.sha256.size(), 32);
    QCOMPARE(offer.notesUrl, QStringLiteral("https://gitee.com/qinjmin/qqcom/releases/tag/v0.2.0"));
    QVERIFY(!offer.mandatory);

    QJsonObject newer{{QStringLiteral("version"), QStringLiteral("0.1.10")}};
    QCOMPARE(evaluate(manifestJson(newer), QStringLiteral("0.1.9")), ManifestStatus::UpdateAvailable);
}

void UpdateManifestTest::acceptsBetaChannelAndUppercaseHash()
{
    UpdateOffer offer;
    const QString hash(64, QLatin1Char('B'));
    QJsonObject overrides{{QStringLiteral("channel"), QStringLiteral("beta")},
                          {QStringLiteral("mandatory"), true},
                          {QStringLiteral("download"), downloadObject({{QStringLiteral("sha256"), hash}})}};
    QCOMPARE(evaluate(manifestJson(overrides), QStringLiteral("0.1.0"), &offer),
             ManifestStatus::UpdateAvailable);
    QCOMPARE(offer.channel, QStringLiteral("beta"));
    QVERIFY(offer.mandatory);
    QCOMPARE(offer.sha256, QByteArray::fromHex(hash.toLatin1()));
}

void UpdateManifestTest::ignoresSameVersionAndDowngrade()
{
    QCOMPARE(evaluate(manifestJson(), QStringLiteral("0.2.0")), ManifestStatus::NotNewer);
    QCOMPARE(evaluate(manifestJson(), QStringLiteral("0.2.1")), ManifestStatus::NotNewer);
    QCOMPARE(evaluate(manifestJson({{QStringLiteral("version"), QStringLiteral("0.1.0")}}),
                      QStringLiteral("0.2.0")),
             ManifestStatus::NotNewer);
}

void UpdateManifestTest::rejectsProtocolAndPackageFields_data()
{
    QTest::addColumn<QJsonObject>("overrides");
    QTest::addColumn<QByteArray>("raw");

    QTest::newRow("schema") << QJsonObject{{QStringLiteral("schema"), 2}} << QByteArray();
    QTest::newRow("schema string") << QJsonObject{{QStringLiteral("schema"), QStringLiteral("1")}}
                                   << QByteArray();
    QTest::newRow("product") << QJsonObject{{QStringLiteral("product"), QStringLiteral("qqcom")}}
                             << QByteArray();
    QTest::newRow("channel") << QJsonObject{{QStringLiteral("channel"), QStringLiteral("gamma")}}
                             << QByteArray();
    QTest::newRow("version") << QJsonObject{{QStringLiteral("version"), QStringLiteral("0.2")}}
                             << QByteArray();
    QTest::newRow("prerelease")
        << QJsonObject{{QStringLiteral("version"), QStringLiteral("0.2.0-beta")}} << QByteArray();
    QTest::newRow("type") << QJsonObject{{QStringLiteral("download"),
                                          downloadObject({{QStringLiteral("type"), QStringLiteral("exe")}})}}
                          << QByteArray();
    QTest::newRow("http") << QJsonObject{{QStringLiteral("download"),
                                          downloadObject({{QStringLiteral("url"),
                                                           QStringLiteral("http://example.com/QQCom.zip")}})}}
                          << QByteArray();
    QTest::newRow("size") << QJsonObject{{QStringLiteral("download"),
                                          downloadObject({{QStringLiteral("size"), 0}})}}
                          << QByteArray();
    QTest::newRow("fractional size")
        << QJsonObject{{QStringLiteral("download"), downloadObject({{QStringLiteral("size"), 1.5}})}}
        << QByteArray();
    QTest::newRow("sha") << QJsonObject{{QStringLiteral("download"),
                                         downloadObject({{QStringLiteral("sha256"), QStringLiteral("abcd")}})}}
                         << QByteArray();
    QTest::newRow("json") << QJsonObject() << QByteArray("{");
    QTest::newRow("array") << QJsonObject() << QByteArray("[]");
}

void UpdateManifestTest::rejectsProtocolAndPackageFields()
{
    QFETCH(QJsonObject, overrides);
    QFETCH(QByteArray, raw);
    const QByteArray json = raw.isEmpty() ? manifestJson(overrides) : raw;
    QString reason;
    QCOMPARE(evaluate(json, QStringLiteral("0.1.0"), nullptr, &reason), ManifestStatus::Rejected);
    QVERIFY2(!reason.isEmpty(), qPrintable(QString::fromUtf8(json)));
}

void UpdateManifestTest::packageFileMatchesChecksSizeAndHash()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("QQCom-0.2.0-win64.zip"));
    const QByteArray bytes = QByteArrayLiteral("qqcom-ota-package");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), qint64(bytes.size()));
    file.close();

    const QByteArray hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    QVERIFY(packageFileMatches(path, bytes.size(), hash));
    QVERIFY(!packageFileMatches(path, bytes.size() + 1, hash));
    QVERIFY(!packageFileMatches(path, bytes.size(), QByteArray(32, '\x11')));
    QVERIFY(!packageFileMatches(path, bytes.size(), QByteArray()));
    QVERIFY(!packageFileMatches(dir.filePath(QStringLiteral("missing.zip")), bytes.size(), hash));
}

void UpdateManifestTest::loopbackOriginDoesNotWidenProductionUrls()
{
    clearLoopbackUpdateOrigin();
    const QUrl package(QStringLiteral("http://127.0.0.1:9/QQCom-0.1.1-win64.zip"));
    QVERIFY(!isAllowedUpdateUrl(package));
    QJsonObject local{{QStringLiteral("download"),
                       downloadObject({{QStringLiteral("url"), package.toString()},
                                       {QStringLiteral("size"), 16}})}};
    QCOMPARE(evaluate(manifestJson(local), QStringLiteral("0.1.0")), ManifestStatus::Rejected);

    setLoopbackUpdateOrigin(QUrl(QStringLiteral("http://127.0.0.1:9/")));
    QVERIFY(loopbackUpdateOriginActive());
    QVERIFY(isAllowedUpdateUrl(package));
    QVERIFY(isAllowedUpdateUrl(QUrl(QStringLiteral("https://gitee.com/qinjmin/qqcom/releases"))));
    QVERIFY(!isAllowedUpdateUrl(QUrl(QStringLiteral("http://127.0.0.1:10/QQCom.zip"))));
    QVERIFY(!isAllowedUpdateUrl(QUrl(QStringLiteral("http://127.0.0.1:9/QQCom.zip?x=1"))));
    QVERIFY(!isAllowedUpdateUrl(QUrl(QStringLiteral("http://localhost:9/QQCom.zip"))));
    QCOMPARE(evaluate(manifestJson(local), QStringLiteral("0.1.0")), ManifestStatus::UpdateAvailable);

    setLoopbackUpdateOrigin(QUrl(QStringLiteral("http://example.com/")));
    QVERIFY(!loopbackUpdateOriginActive());
    QVERIFY(!isAllowedUpdateUrl(package));
    clearLoopbackUpdateOrigin();
}

void UpdateManifestTest::unwrapsGiteeContentsPayload()
{
    const QByteArray manifest = manifestJson();
    QString encoded = QString::fromLatin1(manifest.toBase64());
    encoded.insert(8, QLatin1Char('\n'));
    const QJsonObject envelope{{QStringLiteral("type"), QStringLiteral("file")},
                               {QStringLiteral("encoding"), QStringLiteral("base64")},
                               {QStringLiteral("content"), encoded}};
    const QByteArray wrapped = QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    QCOMPARE(unwrapManifestBody(wrapped), manifest);
    QCOMPARE(unwrapManifestBody(manifest), manifest);
}

QTEST_GUILESS_MAIN(UpdateManifestTest)
#include "tst_update.moc"

#include <QtTest>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QTemporaryDir>

#include "fakehttpserver.h"
#include "hosts.h"
#include "providers.h"
#include "secretstore.h"
#include "sigv4.h"
#include "uploadtestkit.h"

using namespace upload;

namespace {

// The worked examples from the AWS SigV4 documentation for S3.
const sigv4::Credentials kAwsExample{QStringLiteral("AKIAIOSFODNN7EXAMPLE"),
                                     QStringLiteral("wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"),
                                     QStringLiteral("us-east-1"), QStringLiteral("s3")};
const QDateTime kAwsExampleTime(QDate(2013, 5, 24), QTime(0, 0), QTimeZone::UTC);

QByteArray headerValue(const sigv4::Headers &headers, const QByteArray &name) {
    for (const auto &[key, value] : headers) {
        if (key.compare(name, Qt::CaseInsensitive) == 0)
            return value;
    }
    return {};
}

}

class ProviderTests : public QObject {
    Q_OBJECT

private slots:
    void sigv4SignsTheAwsGetObjectExample();
    void sigv4SignsTheAwsPutObjectExample();
    void sigv4SignsTheAwsQueryExamples();
    void sigv4PresignsTheAwsExample();
    void sigv4EncodesLikeAws();
    void s3PutsSmallFilesInOneSignedRequest();
    void s3UploadsLargeFilesInParts();
    void s3AbortsAFailedMultipartUpload();
    void s3ReportsItsErrorMessage();
    void s3BuildsSignedAndCustomDomainLinks();
    void hostsKeepSecretsInTheSecretStore();
    void nextcloudMakesFoldersUploadsAndShares();
    void nextcloudReportsBadCredentials();
    void nextcloudSignInFillsTheFormAndSaves();
    void immichUploadsAndSharesTheAsset();
    void immichCanLinkToTheAssetInstead();
    void xbackboneUploadsWithEitherApi();
    void imgurUploadsVideosAnonymously();
    void everyProviderDescribesItsFields();

private:
    QString writeClip(qint64 size = 0);
    QVariantMap s3Settings(const FakeHttpServer &server) const;
    // Recomputes the signature of a request the fake server received.
    bool signatureMatches(const FakeHttpServer::Request &request, const QString &secretKey) const;

    QTemporaryDir m_dir;
    QNetworkAccessManager m_network;
};

QString ProviderTests::writeClip(qint64 size) {
    const QString path = m_dir.filePath(QStringLiteral("clip_trimmed.mp4"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QByteArray data("fake mp4 bytes");
        if (size > 0) {
            data = QByteArray(size, 'x');
            for (qint64 i = 0; i < data.size(); i += 997)
                data[i] = char('a' + i % 26);
        }
        file.write(data);
    }
    return path;
}

QVariantMap ProviderTests::s3Settings(const FakeHttpServer &server) const {
    return {
        {QStringLiteral("name"), QStringLiteral("Bucket")},
        {QStringLiteral("endpoint"), server.url(QString())},
        {QStringLiteral("region"), QStringLiteral("us-east-1")},
        {QStringLiteral("bucket"), QStringLiteral("clips")},
        {QStringLiteral("accessKeyId"), QStringLiteral("AKIDEXAMPLE")},
        {QStringLiteral("secretAccessKey"), QStringLiteral("secret/key")},
        {QStringLiteral("objectPrefix"), QStringLiteral("omacut/%y")},
        {QStringLiteral("uniqueNames"), false},
        {QStringLiteral("pathStyle"), true},
    };
}

bool ProviderTests::signatureMatches(const FakeHttpServer::Request &request,
                                     const QString &secretKey) const {
    static const QRegularExpression pattern(QString::fromLatin1(
        R"(^AWS4-HMAC-SHA256 Credential=([^/]+)/(\d{8})/([^/]+)/s3/aws4_request, SignedHeaders=([^,]+), Signature=([0-9a-f]{64})$)"));
    const QRegularExpressionMatch match =
        pattern.match(QString::fromLatin1(request.headers.value("authorization")));
    if (!match.hasMatch())
        return false;

    sigv4::Headers headers;
    for (const QString &name : match.captured(4).split(QLatin1Char(';')))
        headers.append({name.toLatin1(), request.headers.value(name.toLatin1())});
    const QUrl url = QUrl::fromEncoded("http://" + request.headers.value("host") + request.target);
    const QByteArray canonical = sigv4::canonicalRequest(
        request.method, url, headers, request.headers.value("x-amz-content-sha256"), nullptr);
    const QDateTime when = QDateTime::fromString(QString::fromLatin1(request.headers.value("x-amz-date")),
                                                 QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
    const sigv4::Credentials credentials{match.captured(1), secretKey, match.captured(3),
                                         QStringLiteral("s3")};
    QDateTime utc = when;
    utc.setTimeZone(QTimeZone::UTC);
    return sigv4::signature(canonical, credentials, utc) == match.captured(5).toLatin1();
}

void ProviderTests::sigv4SignsTheAwsGetObjectExample() {
    const QUrl url = sigv4::buildUrl(QStringLiteral("https"),
                                     QStringLiteral("examplebucket.s3.amazonaws.com"), -1,
                                     QStringLiteral("/test.txt"));
    const sigv4::Headers headers = sigv4::sign("GET", url, {{"Range", "bytes=0-9"}},
                                               sigv4::payloadHash({}), kAwsExample, kAwsExampleTime);
    QCOMPARE(headerValue(headers, "x-amz-content-sha256"),
             QByteArray("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    QCOMPARE(headerValue(headers, "x-amz-date"), QByteArray("20130524T000000Z"));
    QCOMPARE(headerValue(headers, "authorization"),
             QByteArray("AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
                        "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
                        "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"));
}

void ProviderTests::sigv4SignsTheAwsPutObjectExample() {
    const QByteArray body = "Welcome to Amazon S3.";
    QCOMPARE(sigv4::payloadHash(body),
             QByteArray("44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072"));

    const QUrl url = sigv4::buildUrl(QStringLiteral("https"),
                                     QStringLiteral("examplebucket.s3.amazonaws.com"), -1,
                                     QStringLiteral("/test$file.text"));
    QCOMPARE(url.path(QUrl::FullyEncoded), QStringLiteral("/test%24file.text"));
    const sigv4::Headers headers = sigv4::sign(
        "PUT", url,
        {{"Date", "Fri, 24 May 2013 00:00:00 GMT"}, {"x-amz-storage-class", "REDUCED_REDUNDANCY"}},
        sigv4::payloadHash(body), kAwsExample, kAwsExampleTime);
    QVERIFY(headerValue(headers, "authorization").endsWith(
        "SignedHeaders=date;host;x-amz-content-sha256;x-amz-date;x-amz-storage-class, "
        "Signature=98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd"));
}

void ProviderTests::sigv4SignsTheAwsQueryExamples() {
    const QUrl lifecycle = sigv4::buildUrl(QStringLiteral("https"),
                                           QStringLiteral("examplebucket.s3.amazonaws.com"), -1,
                                           QStringLiteral("/"), {{QStringLiteral("lifecycle"), {}}});
    QVERIFY(headerValue(sigv4::sign("GET", lifecycle, {}, sigv4::payloadHash({}), kAwsExample,
                                    kAwsExampleTime),
                        "authorization")
                .endsWith("Signature=fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543"));

    // Sent in any order, the query is signed sorted.
    const QUrl list = sigv4::buildUrl(QStringLiteral("https"),
                                      QStringLiteral("examplebucket.s3.amazonaws.com"), -1,
                                      QStringLiteral("/"),
                                      {{QStringLiteral("prefix"), QStringLiteral("J")},
                                       {QStringLiteral("max-keys"), QStringLiteral("2")}});
    QVERIFY(headerValue(sigv4::sign("GET", list, {}, sigv4::payloadHash({}), kAwsExample,
                                    kAwsExampleTime),
                        "authorization")
                .endsWith("Signature=34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7"));
}

void ProviderTests::sigv4PresignsTheAwsExample() {
    const QUrl url = sigv4::buildUrl(QStringLiteral("https"),
                                     QStringLiteral("examplebucket.s3.amazonaws.com"), -1,
                                     QStringLiteral("/test.txt"));
    const QUrl presigned = sigv4::presign("GET", url, kAwsExample, kAwsExampleTime, 86400);
    const QString query = presigned.query(QUrl::FullyEncoded);
    QVERIFY(query.contains(QStringLiteral(
        "X-Amz-Credential=AKIAIOSFODNN7EXAMPLE%2F20130524%2Fus-east-1%2Fs3%2Faws4_request")));
    QVERIFY(query.contains(QStringLiteral("X-Amz-Expires=86400")));
    QVERIFY(query.endsWith(QStringLiteral(
        "X-Amz-Signature=aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404")));
}

void ProviderTests::sigv4EncodesLikeAws() {
    QCOMPARE(sigv4::uriEncode(QStringLiteral("a b/c~d+é"), false), QByteArray("a%20b/c~d%2B%C3%A9"));
    QCOMPARE(sigv4::uriEncode(QStringLiteral("a/b"), true), QByteArray("a%2Fb"));
}

void ProviderTests::s3PutsSmallFilesInOneSignedRequest() {
    FakeHttpServer server;
    QVERIFY(server.listen());

    const QString path = writeClip();
    const UploadOutcome outcome =
        runJob(s3Provider()->createJob(s3Settings(server), testServices(&m_network), nullptr), path);
    QVERIFY2(outcome.ok, qPrintable(outcome.error));

    const QString year = QDate::currentDate().toString(QStringLiteral("yyyy"));
    QCOMPARE(outcome.url, server.url(QStringLiteral("/clips/omacut/%1/clip_trimmed.mp4").arg(year)));
    QCOMPARE(server.requests.size(), 1);
    const FakeHttpServer::Request &request = server.requests.first();
    QCOMPARE(request.method, QByteArray("PUT"));
    QCOMPARE(request.target, QStringLiteral("/clips/omacut/%1/clip_trimmed.mp4").arg(year).toLatin1());
    QCOMPARE(request.body, QByteArray("fake mp4 bytes"));
    QCOMPARE(request.headers.value("content-type"), QByteArray("video/mp4"));
    QCOMPARE(request.headers.value("x-amz-content-sha256"), sigv4::payloadHash(request.body));
    QVERIFY(!request.headers.contains("x-amz-acl"));
    QVERIFY(!request.headers.contains("x-amz-storage-class"));
    QVERIFY2(signatureMatches(request, QStringLiteral("secret/key")),
             request.headers.value("authorization").constData());

    // Options become headers, all of them signed.
    QVariantMap settings = s3Settings(server);
    settings.insert(QStringLiteral("publicAcl"), true);
    settings.insert(QStringLiteral("storageClass"), QStringLiteral("STANDARD_IA"));
    settings.insert(QStringLiteral("uniqueNames"), true);
    server.requests.clear();
    const UploadOutcome tagged =
        runJob(s3Provider()->createJob(settings, testServices(&m_network), nullptr), path);
    QVERIFY2(tagged.ok, qPrintable(tagged.error));
    const FakeHttpServer::Request &second = server.requests.first();
    QCOMPARE(second.headers.value("x-amz-acl"), QByteArray("public-read"));
    QCOMPARE(second.headers.value("x-amz-storage-class"), QByteArray("STANDARD_IA"));
    QVERIFY(second.headers.value("authorization").contains("x-amz-acl;x-amz-content-sha256"));
    QVERIFY(QRegularExpression(QStringLiteral("/clip_trimmed-[a-z0-9]{6}\\.mp4$"))
                .match(QString::fromLatin1(second.target))
                .hasMatch());
    QVERIFY(signatureMatches(second, QStringLiteral("secret/key")));
}

void ProviderTests::s3UploadsLargeFilesInParts() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.responder = [](const FakeHttpServer::Request &request) -> FakeHttpServer::Response {
        if (request.method == "POST" && request.target.endsWith("?uploads="))
            return {200, "<InitiateMultipartUploadResult><UploadId>up/1</UploadId></InitiateMultipartUploadResult>", {}};
        if (request.method == "PUT") {
            const QByteArray part = request.target.mid(request.target.indexOf("partNumber=") + 11, 1);
            return {200, {}, {{"ETag", "\"etag" + part + "\""}}};
        }
        return {200, "<CompleteMultipartUploadResult/>", {}};
    };

    QVariantMap settings = s3Settings(server);
    settings.insert(QStringLiteral("multipartThreshold"), 1000);
    settings.insert(QStringLiteral("partSize"), 1000);
    QList<qint64> progress;
    Job *job = s3Provider()->createJob(settings, testServices(&m_network), nullptr);
    connect(job, &Job::progress, job, [&progress](qint64 sent, qint64 total) {
        QCOMPARE(total, 2500);
        progress << sent;
    });
    const UploadOutcome outcome = runJob(job, writeClip(2500));
    QVERIFY2(outcome.ok, qPrintable(outcome.error));

    QCOMPARE(server.requests.size(), 5);
    QVERIFY(server.requests.at(0).target.endsWith("?uploads="));
    QCOMPARE(server.requests.at(0).headers.value("content-type"), QByteArray("video/mp4"));
    QVERIFY(server.requests.at(1).target.endsWith("?partNumber=1&uploadId=up%2F1"));
    QCOMPARE(server.requests.at(1).body.size(), 1000);
    QCOMPARE(server.requests.at(3).body.size(), 500);
    const QByteArray complete = server.requests.at(4).body;
    QVERIFY(complete.contains("<Part><PartNumber>1</PartNumber><ETag>&quot;etag1&quot;</ETag></Part>"));
    QVERIFY(complete.contains("<PartNumber>3</PartNumber>"));
    for (const FakeHttpServer::Request &request : std::as_const(server.requests))
        QVERIFY2(signatureMatches(request, QStringLiteral("secret/key")), request.target.constData());
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last(), 2500);
}

void ProviderTests::s3AbortsAFailedMultipartUpload() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.responder = [](const FakeHttpServer::Request &request) -> FakeHttpServer::Response {
        if (request.method == "POST")
            return {200, "<InitiateMultipartUploadResult><UploadId>u1</UploadId></InitiateMultipartUploadResult>", {}};
        if (request.method == "PUT")
            return {500, "<Error><Code>InternalError</Code><Message>Try again</Message></Error>", {}};
        return {204, {}, {}};
    };

    QVariantMap settings = s3Settings(server);
    settings.insert(QStringLiteral("multipartThreshold"), 1000);
    settings.insert(QStringLiteral("partSize"), 1000);
    const UploadOutcome outcome =
        runJob(s3Provider()->createJob(settings, testServices(&m_network), nullptr), writeClip(2500));
    QVERIFY(!outcome.ok);
    QCOMPARE(outcome.error, QStringLiteral("Bucket answered HTTP 500: Try again"));
    QTRY_COMPARE(server.requests.size(), 3);
    QCOMPARE(server.requests.at(2).method, QByteArray("DELETE"));
    QVERIFY(server.requests.at(2).target.endsWith("?uploadId=u1"));
}

void ProviderTests::s3ReportsItsErrorMessage() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.status = 403;
    server.reply = "<?xml version=\"1.0\"?><Error><Code>SignatureDoesNotMatch</Code>"
                   "<Message>The signature does not match.</Message></Error>";
    const UploadOutcome outcome =
        runJob(s3Provider()->createJob(s3Settings(server), testServices(&m_network), nullptr), writeClip());
    QCOMPARE(outcome.error, QStringLiteral("Bucket answered HTTP 403: The signature does not match."));
}

void ProviderTests::s3BuildsSignedAndCustomDomainLinks() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    const QString year = QDate::currentDate().toString(QStringLiteral("yyyy"));

    QVariantMap settings = s3Settings(server);
    settings.insert(QStringLiteral("customDomain"), QStringLiteral("cdn.example.com/"));
    UploadOutcome outcome =
        runJob(s3Provider()->createJob(settings, testServices(&m_network), nullptr), writeClip());
    QCOMPARE(outcome.url, QStringLiteral("https://cdn.example.com/omacut/%1/clip_trimmed.mp4").arg(year));

    settings.insert(QStringLiteral("linkType"), QStringLiteral("Signed link (7 days)"));
    outcome = runJob(s3Provider()->createJob(settings, testServices(&m_network), nullptr), writeClip());
    const QUrl link(outcome.url);
    QCOMPARE(link.path(), QStringLiteral("/clips/omacut/%1/clip_trimmed.mp4").arg(year));
    QVERIFY(link.query().contains(QStringLiteral("X-Amz-Expires=604800")));
    QVERIFY(link.query().contains(QStringLiteral("X-Amz-Signature=")));
}

void ProviderTests::hostsKeepSecretsInTheSecretStore() {
    QTemporaryDir config;
    auto store = std::make_unique<MemorySecretStore>();
    MemorySecretStore *secrets = store.get();
    Hosts hosts(config.path(), std::move(store));

    QVariantMap values{{QStringLiteral("bucket"), QStringLiteral("b")},
                       {QStringLiteral("accessKeyId"), QStringLiteral("AKID")}};
    // The secret is required.
    QVERIFY(hosts.save({}, QStringLiteral("s3"), QStringLiteral("S3"), values).isEmpty());
    QCOMPARE(hosts.lastError(), QStringLiteral("Secret access key is required."));

    values.insert(QStringLiteral("secretAccessKey"), QStringLiteral("hunter2"));
    const QString id = hosts.save({}, QStringLiteral("s3"), QStringLiteral("S3"), values);
    QVERIFY2(!id.isEmpty(), qPrintable(hosts.lastError()));
    QCOMPARE(secrets->read(id + QStringLiteral("/secretAccessKey")), QStringLiteral("hunter2"));

    QFile file(config.filePath(QStringLiteral("hosts.json")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray json = file.readAll();
    QVERIFY(json.contains("AKID"));
    QVERIFY(!json.contains("hunter2"));

    // Editing shows that a secret is stored without revealing it, and a
    // blank secret keeps it.
    const QVariantMap shown = hosts.values(id);
    QCOMPARE(shown.value(QStringLiteral("secretsSet")).toStringList(),
             QStringList{QStringLiteral("secretAccessKey")});
    QVERIFY(!shown.contains(QStringLiteral("secretAccessKey")));
    QCOMPARE(shown.value(QStringLiteral("region")).toString(), QStringLiteral("us-east-1"));
    values.insert(QStringLiteral("secretAccessKey"), QString());
    QCOMPARE(hosts.save(id, QStringLiteral("s3"), QStringLiteral("S3"), values), id);
    QCOMPARE(secrets->read(id + QStringLiteral("/secretAccessKey")), QStringLiteral("hunter2"));

    hosts.remove(id);
    QVERIFY(secrets->read(id + QStringLiteral("/secretAccessKey")).isEmpty());
}

void ProviderTests::nextcloudMakesFoldersUploadsAndShares() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.responder = [&server](const FakeHttpServer::Request &request) -> FakeHttpServer::Response {
        if (request.method == "MKCOL")
            return {request.target.endsWith("/omacut") ? 405 : 201, {}, {}};
        if (request.method == "PUT")
            return {201, {}, {}};
        return {200, R"({"ocs":{"meta":{"statuscode":200},"data":{"url":")"
                         + server.url(QStringLiteral("/s/AbC123")).toLatin1() + R"("}}})", {}};
    };

    const QVariantMap settings{
        {QStringLiteral("name"), QStringLiteral("Cloud")},
        {QStringLiteral("serverUrl"), server.url(QStringLiteral("/"))},
        {QStringLiteral("username"), QStringLiteral("al ice")},
        {QStringLiteral("appPassword"), QStringLiteral("app-pw")},
        {QStringLiteral("folder"), QStringLiteral("/omacut/%y")},
        {QStringLiteral("expireDays"), 3},
        {QStringLiteral("directLink"), true},
    };
    const UploadOutcome outcome =
        runJob(nextcloudProvider()->createJob(settings, testServices(&m_network), nullptr), writeClip());
    QVERIFY2(outcome.ok, qPrintable(outcome.error));
    QCOMPARE(outcome.url, server.url(QStringLiteral("/s/AbC123/download")));

    const QString year = QDate::currentDate().toString(QStringLiteral("yyyy"));
    QCOMPARE(server.requests.size(), 4);
    // An existing folder (405) is fine; each level is made in turn.
    QCOMPARE(server.requests.at(0).method, QByteArray("MKCOL"));
    QCOMPARE(server.requests.at(0).target, QByteArray("/remote.php/dav/files/al%20ice/omacut"));
    QCOMPARE(server.requests.at(1).target,
             QStringLiteral("/remote.php/dav/files/al%20ice/omacut/%1").arg(year).toLatin1());
    const FakeHttpServer::Request &put = server.requests.at(2);
    QCOMPARE(put.method, QByteArray("PUT"));
    QVERIFY(QRegularExpression(QStringLiteral("^/remote.php/dav/files/al%20ice/omacut/\\d{4}/clip_trimmed-[a-z0-9]{6}\\.mp4$"))
                .match(QString::fromLatin1(put.target)).hasMatch());
    QCOMPARE(put.headers.value("authorization"), "Basic " + QByteArray("al ice:app-pw").toBase64());
    QCOMPARE(put.body, QByteArray("fake mp4 bytes"));

    const FakeHttpServer::Request &share = server.requests.at(3);
    QCOMPARE(share.target, QByteArray("/ocs/v2.php/apps/files_sharing/api/v1/shares?format=json"));
    QCOMPARE(share.headers.value("ocs-apirequest"), QByteArray("true"));
    const QUrlQuery form(QString::fromUtf8(share.body));
    QCOMPARE(form.queryItemValue(QStringLiteral("shareType")), QStringLiteral("3"));
    QVERIFY(form.queryItemValue(QStringLiteral("path"), QUrl::FullyDecoded)
                .startsWith(QStringLiteral("/omacut/%1/clip_trimmed-").arg(year)));
    QCOMPARE(form.queryItemValue(QStringLiteral("expireDate")),
             QDate::currentDate().addDays(3).toString(Qt::ISODate));
}

void ProviderTests::nextcloudReportsBadCredentials() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.status = 401;
    const QVariantMap settings{
        {QStringLiteral("name"), QStringLiteral("Cloud")},
        {QStringLiteral("serverUrl"), server.url(QString())},
        {QStringLiteral("username"), QStringLiteral("a")},
        {QStringLiteral("appPassword"), QStringLiteral("wrong")},
        {QStringLiteral("folder"), QStringLiteral("x")},
    };
    const UploadOutcome outcome =
        runJob(nextcloudProvider()->createJob(settings, testServices(&m_network), nullptr), writeClip());
    QCOMPARE(outcome.error, QStringLiteral("Cloud answered HTTP 401: check the user name and app password"));
}

void ProviderTests::nextcloudSignInFillsTheFormAndSaves() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.responder = [&server](const FakeHttpServer::Request &request) -> FakeHttpServer::Response {
        if (request.target == "/index.php/login/v2") {
            return {200, R"({"poll":{"token":"tok","endpoint":")"
                             + server.url(QStringLiteral("/login/v2/poll")).toLatin1()
                             + R"("},"login":")" + server.url(QStringLiteral("/login/v2/flow/x")).toLatin1()
                             + R"("})", {}};
        }
        return {200, R"({"server":")" + server.url(QString()).toLatin1()
                         + R"(","loginName":"alice","appPassword":"granted"})", {}};
    };

    QTemporaryDir config;
    auto store = std::make_unique<MemorySecretStore>();
    MemorySecretStore *secrets = store.get();
    Hosts hosts(config.path(), std::move(store));
    hosts.setNetwork(&m_network);
    QList<QUrl> opened;
    hosts.setUrlOpener([&opened](const QUrl &url) {
        opened << url;
        return true;
    });

    QSignalSpy finished(&hosts, &Hosts::authorizeFinished);
    const QVariantMap draft{{QStringLiteral("serverUrl"), server.url(QString())}};
    hosts.authorize({}, QStringLiteral("nextcloud"), draft);
    QVERIFY(hosts.authorizing());
    QTRY_COMPARE(finished.count(), 1);
    QVERIFY(!hosts.authorizing());
    QCOMPARE(finished.first().at(0).toString(), QString());
    QCOMPARE(finished.first().at(1).toString(), QStringLiteral("Signed in as alice"));
    const QVariantMap formValues = finished.first().at(2).toMap();
    QCOMPARE(formValues.value(QStringLiteral("username")).toString(), QStringLiteral("alice"));
    QVERIFY(!formValues.contains(QStringLiteral("appPassword")));
    QCOMPARE(formValues.value(QStringLiteral("secretsSet")).toStringList(),
             QStringList{QStringLiteral("appPassword")});
    QCOMPARE(opened, QList<QUrl>{QUrl(server.url(QStringLiteral("/login/v2/flow/x")))});
    QCOMPARE(server.requests.at(1).body, QByteArray("token=tok"));

    // The app password was never typed; saving the form stores it anyway.
    QVariantMap values = draft;
    values.insert(QStringLiteral("username"), QStringLiteral("alice"));
    const QString id = hosts.save({}, QStringLiteral("nextcloud"), QStringLiteral("Cloud"), values);
    QVERIFY2(!id.isEmpty(), qPrintable(hosts.lastError()));
    QCOMPARE(secrets->read(id + QStringLiteral("/appPassword")), QStringLiteral("granted"));

    // Used once: another host of the same kind doesn't inherit it.
    values.insert(QStringLiteral("username"), QStringLiteral("bob"));
    QVERIFY(hosts.save({}, QStringLiteral("nextcloud"), QStringLiteral("Other"), values).isEmpty());
    QCOMPARE(hosts.lastError(), QStringLiteral("App password is required."));
}

void ProviderTests::immichUploadsAndSharesTheAsset() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.responder = [](const FakeHttpServer::Request &request) -> FakeHttpServer::Response {
        if (request.target == "/api/assets")
            return {201, R"({"id":"asset-1","status":"created"})", {}};
        return {201, R"({"id":"link-1","key":"KEY","slug":null})", {}};
    };

    const QVariantMap settings{
        {QStringLiteral("name"), QStringLiteral("Photos")},
        {QStringLiteral("serverUrl"), server.url(QStringLiteral("/api"))},
        {QStringLiteral("apiKey"), QStringLiteral("key-1")},
        {QStringLiteral("shareLink"), true},
        {QStringLiteral("expireDays"), 2},
        {QStringLiteral("publicUrl"), QStringLiteral("https://share.example.com/")},
    };
    const UploadOutcome outcome =
        runJob(immichProvider()->createJob(settings, testServices(&m_network), nullptr), writeClip());
    QVERIFY2(outcome.ok, qPrintable(outcome.error));
    QCOMPARE(outcome.url, QStringLiteral("https://share.example.com/share/KEY"));
    QCOMPARE(outcome.thumbnailUrl, server.url(QStringLiteral("/api/assets/asset-1/thumbnail")));

    const FakeHttpServer::Request &upload = server.requests.at(0);
    QCOMPARE(upload.headers.value("x-api-key"), QByteArray("key-1"));
    QVERIFY(upload.body.contains("name=\"deviceId\"\r\n\r\nomacut"));
    QVERIFY(upload.body.contains("name=\"assetData\"; filename=\"clip_trimmed.mp4\""));
    QVERIFY(upload.body.contains("name=\"fileCreatedAt\""));

    const QJsonObject link = QJsonDocument::fromJson(server.requests.at(1).body).object();
    QCOMPARE(server.requests.at(1).target, QByteArray("/api/shared-links"));
    QCOMPARE(link.value(QStringLiteral("type")).toString(), QStringLiteral("INDIVIDUAL"));
    QCOMPARE(link.value(QStringLiteral("assetIds")).toArray().first().toString(), QStringLiteral("asset-1"));
    QVERIFY(link.contains(QStringLiteral("expiresAt")));
}

void ProviderTests::immichCanLinkToTheAssetInstead() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.status = 201;
    server.reply = R"({"id":"asset-2","status":"duplicate"})";

    const QVariantMap settings{
        {QStringLiteral("name"), QStringLiteral("Photos")},
        {QStringLiteral("serverUrl"), server.url(QString())},
        {QStringLiteral("apiKey"), QStringLiteral("k")},
        {QStringLiteral("shareLink"), false},
    };
    const UploadOutcome outcome =
        runJob(immichProvider()->createJob(settings, testServices(&m_network), nullptr), writeClip());
    QCOMPARE(outcome.url, server.url(QStringLiteral("/photos/asset-2")));
    QCOMPARE(server.requests.size(), 1);

    server.status = 400;
    server.reply = R"({"message":["assetData must be a file"],"statusCode":400})";
    const UploadOutcome failed =
        runJob(immichProvider()->createJob(settings, testServices(&m_network), nullptr), writeClip());
    QCOMPARE(failed.error, QStringLiteral("Photos answered HTTP 400: assetData must be a file"));
}

void ProviderTests::xbackboneUploadsWithEitherApi() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.reply = R"({"message":"OK","url":"https://x.example/ab/cd","raw_url":"https://x.example/ab/cd/raw"})";

    QVariantMap settings{
        {QStringLiteral("name"), QStringLiteral("XBB")},
        {QStringLiteral("serverUrl"), server.url(QString())},
        {QStringLiteral("token"), QStringLiteral("token_1")},
    };
    UploadOutcome outcome =
        runJob(xbackboneProvider()->createJob(settings, testServices(&m_network), nullptr), writeClip());
    QVERIFY2(outcome.ok, qPrintable(outcome.error));
    QCOMPARE(outcome.url, QStringLiteral("https://x.example/ab/cd"));
    QCOMPARE(outcome.thumbnailUrl, QStringLiteral("https://x.example/ab/cd/raw"));
    QCOMPARE(server.requests.at(0).target, QByteArray("/upload"));
    QVERIFY(server.requests.at(0).body.contains("name=\"token\"\r\n\r\ntoken_1"));
    QVERIFY(server.requests.at(0).body.contains("name=\"upload\"; filename="));

    server.reply = R"({"data":{"preview_ext_url":"https://x.example/p.mp4","raw_url":"https://x.example/r","deletion_url":"https://x.example/d"}})";
    settings.insert(QStringLiteral("api"), QStringLiteral("API v1 (/api/v1/upload)"));
    outcome = runJob(xbackboneProvider()->createJob(settings, testServices(&m_network), nullptr), writeClip());
    QVERIFY2(outcome.ok, qPrintable(outcome.error));
    QCOMPARE(outcome.url, QStringLiteral("https://x.example/p.mp4"));
    QCOMPARE(outcome.deletionUrl, QStringLiteral("https://x.example/d"));
    QCOMPARE(server.requests.at(1).target, QByteArray("/api/v1/upload"));
    QCOMPARE(server.requests.at(1).headers.value("authorization"), QByteArray("Bearer token_1"));
    QVERIFY(server.requests.at(1).body.contains("name=\"file\"; filename="));

    server.status = 401;
    server.reply = R"({"message":"Token not found."})";
    outcome = runJob(xbackboneProvider()->createJob(settings, testServices(&m_network), nullptr), writeClip());
    QCOMPARE(outcome.error, QStringLiteral("XBB answered HTTP 401: Token not found."));
}

void ProviderTests::imgurUploadsVideosAnonymously() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.reply = R"({"data":{"id":"a1","link":"https://i.imgur.com/a1.mp4","deletehash":"dh"},"success":true,"status":200})";

    QVariantMap settings{
        {QStringLiteral("name"), QStringLiteral("Imgur")},
        {QStringLiteral("clientId"), QStringLiteral(" cid ")},
        {QStringLiteral("apiUrl"), server.url(QString())},
    };
    UploadOutcome outcome =
        runJob(imgurProvider()->createJob(settings, testServices(&m_network), nullptr), writeClip());
    QVERIFY2(outcome.ok, qPrintable(outcome.error));
    QCOMPARE(outcome.url, QStringLiteral("https://i.imgur.com/a1.mp4"));
    QCOMPARE(outcome.deletionUrl, QStringLiteral("https://imgur.com/delete/dh"));
    QCOMPARE(server.requests.at(0).target, QByteArray("/3/upload"));
    QCOMPARE(server.requests.at(0).headers.value("authorization"), QByteArray("Client-ID cid"));
    QVERIFY(server.requests.at(0).body.contains("name=\"video\"; filename=\"clip_trimmed.mp4\""));

    server.status = 400;
    server.reply = R"({"data":{"error":{"message":"File is over the size limit"}},"success":false})";
    outcome = runJob(imgurProvider()->createJob(settings, testServices(&m_network), nullptr), writeClip());
    QCOMPARE(outcome.error, QStringLiteral("Imgur answered HTTP 400: File is over the size limit"));
}

void ProviderTests::everyProviderDescribesItsFields() {
    QStringList ids;
    for (const Provider *provider : providers()) {
        ids << provider->id();
        QVERIFY(!provider->name().isEmpty());
        QVERIFY(!provider->description().isEmpty());
        QSet<QString> keys;
        for (const Field &field : provider->fields()) {
            QVERIFY2(!keys.contains(field.key), qPrintable(field.key));
            keys.insert(field.key);
            QVERIFY(!field.label.isEmpty());
            if (field.type == Field::Choice)
                QVERIFY(field.choices.contains(field.defaultValue.toString()));
        }
        QCOMPARE(findProvider(provider->id()), provider);
    }
    QCOMPARE(ids.size(), QSet<QString>(ids.begin(), ids.end()).size());
    QVERIFY(!findProvider(QStringLiteral("missing")));
}

QTEST_GUILESS_MAIN(ProviderTests)
#include "provider_tests.moc"

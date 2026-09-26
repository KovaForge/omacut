#include <QtTest>

#include <QCryptographicHash>
#include <QRegularExpression>
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

QTEST_GUILESS_MAIN(ProviderTests)
#include "provider_tests.moc"

#include <QtTest>

#include <QTcpServer>
#include <QTemporaryDir>

#include "fakehttpserver.h"
#include "sxcu.h"
#include "hosts.h"
#include "providers.h"
#include "secretstore.h"
#include "uploadtestkit.h"

using namespace upload;

class UploaderTests : public QObject {
    Q_OBJECT

private slots:
    void sxcuReadsAMultipartUploader();
    void sxcuReadsABinaryUploader();
    void sxcuInfersMultipartFromAFileFormName();
    void sxcuRejectsUploadersThatCantCarryAFile();
    void expandLeavesPlainTextAlone();
    void expandReadsTheResponse();
    void expandReadsJson();
    void expandReadsRegexGroups();
    void expandReadsHeadersCaseInsensitively();
    void expandNestsAndEscapes();
    void expandDropsUnknownFunctions();
    void jsonPathHandlesValueTypes();
    void builtInHostsAreValid();
    void sxcuFilesBecomeReadOnlyHosts();
    void hostsSaveEditAndRemove();
    void hostsValidateBeforeSaving();
    void hostsRefuseToEditReadOnlyHosts();
    void hostsMakeJobsWithSecretsFilledIn();
    void fileSecretStoreIsPrivate();
    void multipartUploadSendsArgumentsAndFile();
    void binaryUploadSendsTheFileAsTheBody();
    void uploadReadsLinksFromTheResponse();
    void uploadReportsHttpErrors();
    void uploadRejectsResponsesWithoutALink();
    void uploadReportsUnreachableHosts();
    void uploadReportsUnreadableFiles();
    void uploadCanBeCancelled();

private:
    QString writeClip(const QByteArray &content = "fake mp4 bytes");
    sxcu::Destination destinationFor(const FakeHttpServer &server);
    UploadOutcome runUpload(const sxcu::Destination &destination, const QString &path);
    Job *sxcuJob(const sxcu::Destination &destination);

    QTemporaryDir m_dir;
    QNetworkAccessManager m_network;
};

Job *UploaderTests::sxcuJob(const sxcu::Destination &destination) {
    return sxcuProvider()->createJob({{QStringLiteral("definition"), sxcuJson(destination)}},
                                     testServices(&m_network), nullptr);
}

UploadOutcome UploaderTests::runUpload(const sxcu::Destination &destination, const QString &path) {
    return runJob(sxcuJob(destination), path);
}

QString UploaderTests::writeClip(const QByteArray &content) {
    const QString path = m_dir.filePath(QStringLiteral("clip_trimmed.mp4"));
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(content);
    return path;
}

sxcu::Destination UploaderTests::destinationFor(const FakeHttpServer &server) {
    sxcu::Destination destination;
    destination.name = QStringLiteral("Fake");
    destination.requestUrl = server.url();
    destination.fileFormName = QStringLiteral("fileToUpload");
    return destination;
}

void UploaderTests::sxcuReadsAMultipartUploader() {
    sxcu::Destination d;
    QString error;
    const QByteArray json = R"({
        "Version": "15.0.0",
        "Name": "Example",
        "DestinationType": "ImageUploader, FileUploader",
        "RequestMethod": "post",
        "RequestURL": "https://example.com/upload",
        "Parameters": { "expires": "24" },
        "Headers": { "Authorization": "Bearer abc" },
        "Body": "MultipartFormData",
        "Arguments": { "reqtype": "fileupload", "n": 3 },
        "FileFormName": "file",
        "URL": "{json:data.link}",
        "DeletionURL": "{json:data.delete}",
        "ErrorMessage": "{json:error}"
    })";
    QVERIFY2(sxcu::fromJson(json, &d, &error), qPrintable(error));

    QCOMPARE(d.name, QStringLiteral("Example"));
    QCOMPARE(d.requestMethod, QStringLiteral("POST"));
    QCOMPARE(d.requestUrl, QStringLiteral("https://example.com/upload"));
    QCOMPARE(d.body, QStringLiteral("MultipartFormData"));
    QCOMPARE(d.fileFormName, QStringLiteral("file"));
    QCOMPARE(d.parameters, (sxcu::Pairs{{QStringLiteral("expires"), QStringLiteral("24")}}));
    QCOMPARE(d.headers, (sxcu::Pairs{{QStringLiteral("Authorization"), QStringLiteral("Bearer abc")}}));
    QCOMPARE(d.arguments, (sxcu::Pairs{{QStringLiteral("n"), QStringLiteral("3")},
                                       {QStringLiteral("reqtype"), QStringLiteral("fileupload")}}));
    QCOMPARE(d.url, QStringLiteral("{json:data.link}"));
    QCOMPARE(d.deletionUrl, QStringLiteral("{json:data.delete}"));
    QCOMPARE(d.errorMessage, QStringLiteral("{json:error}"));
}

void UploaderTests::sxcuReadsABinaryUploader() {
    sxcu::Destination d;
    const QByteArray json = R"({"RequestMethod": "PUT", "Body": "Binary",
                               "RequestURL": "https://files.example.org/{filename}"})";
    QVERIFY(sxcu::fromJson(json, &d));
    QCOMPARE(d.body, QStringLiteral("Binary"));
    QCOMPARE(d.requestMethod, QStringLiteral("PUT"));
    // Nameless configs are named after their host.
    QCOMPARE(d.name, QStringLiteral("files.example.org"));
}

void UploaderTests::sxcuInfersMultipartFromAFileFormName() {
    sxcu::Destination d;
    const QByteArray json = R"({"RequestURL": "https://example.com", "FileFormName": "f"})";
    QVERIFY(sxcu::fromJson(json, &d));
    QCOMPARE(d.body, QStringLiteral("MultipartFormData"));
    QCOMPARE(d.requestMethod, QStringLiteral("POST"));
}

void UploaderTests::sxcuRejectsUploadersThatCantCarryAFile() {
    QString error;
    QVERIFY(!sxcu::fromJson("not json", nullptr, &error));
    QVERIFY(!error.isEmpty());

    const QByteArray noUrl = R"({"Name": "x", "FileFormName": "f"})";
    QVERIFY(!sxcu::fromJson(noUrl, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("RequestURL")));

    const QByteArray noField = R"({"RequestURL": "https://example.com", "Body": "MultipartFormData"})";
    QVERIFY(!sxcu::fromJson(noField, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("FileFormName")));

    // A URL shortener, say: nothing to put the video in.
    const QByteArray shortener = R"({"RequestURL": "https://example.com", "Body": "JSON",
                                     "Data": "{\"url\": \"{input}\"}"})";
    QVERIFY(!sxcu::fromJson(shortener, nullptr, &error));
    QVERIFY(error.contains(QStringLiteral("JSON")));
}

void UploaderTests::expandLeavesPlainTextAlone() {
    QCOMPARE(sxcu::expand(QStringLiteral("https://example.com/a?b=c"), {}),
             QStringLiteral("https://example.com/a?b=c"));
    // Outside a function, the argument delimiters are just characters.
    QCOMPARE(sxcu::expand(QStringLiteral("a|b}c"), {}), QStringLiteral("a|b}c"));
    QCOMPARE(sxcu::expand(QString(), {}), QString());
}

void UploaderTests::expandReadsTheResponse() {
    sxcu::Response response;
    response.body = "https://files.example.com/abc.mp4\n";
    response.url = QUrl(QStringLiteral("https://example.com/upload"));
    const sxcu::Context context{&response, QStringLiteral("clip_trimmed.mp4")};

    QCOMPARE(sxcu::expand(QStringLiteral("{response}"), context),
             QStringLiteral("https://files.example.com/abc.mp4\n"));
    QCOMPARE(sxcu::expand(QStringLiteral("{responseurl}"), context),
             QStringLiteral("https://example.com/upload"));
    QCOMPARE(sxcu::expand(QStringLiteral("https://x/{filename}"), context),
             QStringLiteral("https://x/clip_trimmed.mp4"));
    // No response yet (e.g. while building the request) expands to nothing.
    QCOMPARE(sxcu::expand(QStringLiteral("[{response}]"), {}), QStringLiteral("[]"));
}

void UploaderTests::expandReadsJson() {
    sxcu::Response response;
    response.body = R"({"success": true, "files": [{"url": "https://u.example/a.mp4", "size": 42}]})";
    const sxcu::Context context{&response, {}};

    QCOMPARE(sxcu::expand(QStringLiteral("{json:files[0].url}"), context),
             QStringLiteral("https://u.example/a.mp4"));
    QCOMPARE(sxcu::expand(QStringLiteral("{json:$.files[0].size}"), context), QStringLiteral("42"));
    QCOMPARE(sxcu::expand(QStringLiteral("{json:files[3].url}"), context), QString());
    // Literal braces in an argument are escaped, as in ShareX.
    QCOMPARE(sxcu::expand(QStringLiteral("{json:\\{\"a\":\\{\"b\":\"c\"\\}\\}|a.b}"), {}), QStringLiteral("c"));
}

void UploaderTests::expandReadsRegexGroups() {
    sxcu::Response response;
    response.body = R"(<a href="https://h.example/v/xyz">xyz</a>)";
    const sxcu::Context context{&response, {}};

    QCOMPARE(sxcu::expand(QStringLiteral("{regex:(?<=href=\").+?(?=\")}"), context),
             QStringLiteral("https://h.example/v/xyz"));
    QCOMPARE(sxcu::expand(QStringLiteral("{regex:/v/([a-z]+)|1}"), context), QStringLiteral("xyz"));
    QCOMPARE(sxcu::expand(QStringLiteral("{regex:/v/(?<id>[a-z]+)|id}"), context), QStringLiteral("xyz"));
    // The template's own escape eats one backslash, so regex classes take two.
    QCOMPARE(sxcu::expand(QStringLiteral("{regex:abc123|\\\\d+|0}"), {}), QStringLiteral("123"));
    QCOMPARE(sxcu::expand(QStringLiteral("{regex:nomatch}"), context), QString());
}

void UploaderTests::expandReadsHeadersCaseInsensitively() {
    sxcu::Response response;
    response.headers.insert("x-token", "s3cret");
    const sxcu::Context context{&response, {}};

    QCOMPARE(sxcu::expand(QStringLiteral("{header:X-Token}"), context), QStringLiteral("s3cret"));
    QCOMPARE(sxcu::expand(QStringLiteral("{header:Missing}"), context), QString());
}

void UploaderTests::expandNestsAndEscapes() {
    sxcu::Response response;
    response.body = R"({"id": "q1w2"})";
    const sxcu::Context context{&response, {}};

    QCOMPARE(sxcu::expand(QStringLiteral("https://h.example/{json:{response}|id}.mp4"), context),
             QStringLiteral("https://h.example/q1w2.mp4"));
    QCOMPARE(sxcu::expand(QStringLiteral("\\{json:id\\}"), context), QStringLiteral("{json:id}"));
    QCOMPARE(sxcu::expand(QStringLiteral("{base64:user\\:pass}"), {}),
             QString::fromLatin1(QByteArray("user:pass").toBase64()));

    const QString picked = sxcu::expand(QStringLiteral("{random:a.example|b.example}"), {});
    QVERIFY(picked == QStringLiteral("a.example") || picked == QStringLiteral("b.example"));
}

void UploaderTests::expandDropsUnknownFunctions() {
    QCOMPARE(sxcu::expand(QStringLiteral("a{nope:x|y}b"), {}), QStringLiteral("ab"));
    // An unterminated call still ends cleanly.
    QCOMPARE(sxcu::expand(QStringLiteral("a{filename"), {nullptr, QStringLiteral("f")}),
             QStringLiteral("af"));
}

void UploaderTests::jsonPathHandlesValueTypes() {
    const QByteArray json = R"({"s": "x", "n": 1.5, "i": 7, "b": false, "o": {"k": 1}, "a": [1, 2]})";
    QCOMPARE(sxcu::jsonPath(json, QStringLiteral("s")), QStringLiteral("x"));
    QCOMPARE(sxcu::jsonPath(json, QStringLiteral("n")), QStringLiteral("1.5"));
    QCOMPARE(sxcu::jsonPath(json, QStringLiteral("i")), QStringLiteral("7"));
    QCOMPARE(sxcu::jsonPath(json, QStringLiteral("b")), QStringLiteral("false"));
    QCOMPARE(sxcu::jsonPath(json, QStringLiteral("o")), QString::fromUtf8(R"({"k":1})"));
    QCOMPARE(sxcu::jsonPath(json, QStringLiteral("a")), QStringLiteral("[1,2]"));
    QCOMPARE(sxcu::jsonPath(json, QStringLiteral("a[1]")), QStringLiteral("2"));
    QCOMPARE(sxcu::jsonPath("[{\"u\": \"top\"}]", QStringLiteral("[0].u")), QStringLiteral("top"));
    QCOMPARE(sxcu::jsonPath("not json", QStringLiteral("a")), QString());
    QCOMPARE(sxcu::jsonPath(json, QStringLiteral("s.deeper")), QString());
}

void UploaderTests::builtInHostsAreValid() {
    QTemporaryDir config;
    Hosts hosts(config.path(), std::make_unique<MemorySecretStore>());
    QStringList names;
    QStringList ids;
    for (const Host &host : hosts.all()) {
        names << host.name;
        ids << host.id;
        QVERIFY(host.readOnly);
        QCOMPARE(host.provider, QStringLiteral("sxcu"));
        QVERIFY(sxcuProvider()->validate(host.settings).isEmpty());
    }
    QCOMPARE(names, (QStringList{QStringLiteral("Litterbox (72 hours)"), QStringLiteral("Catbox"),
                                 QStringLiteral("Uguu (3 hours)")}));
    QCOMPARE(ids, (QStringList{QStringLiteral("builtin:litterbox"), QStringLiteral("builtin:catbox"),
                               QStringLiteral("builtin:uguu")}));
}

void UploaderTests::sxcuFilesBecomeReadOnlyHosts() {
    QTemporaryDir config;
    const QString dir = config.filePath(QStringLiteral("uploaders"));
    QVERIFY(QDir().mkpath(dir));
    const auto write = [&](const QString &name, const QByteArray &content) {
        QFile file(QDir(dir).filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(content);
    };
    write(QStringLiteral("z.sxcu"), R"({"Name": "alpha", "RequestURL": "https://a.example", "FileFormName": "f"})");
    write(QStringLiteral("a.sxcu"), R"({"Name": "Beta", "RequestURL": "https://b.example", "Body": "Binary"})");
    write(QStringLiteral("broken.sxcu"), "{");
    write(QStringLiteral("ignored.json"), R"({"RequestURL": "https://c.example", "FileFormName": "f"})");

    Hosts hosts(config.path(), std::make_unique<MemorySecretStore>());
    QCOMPARE(hosts.uploadersDir(), dir);
    const QList<Host> all = hosts.all();
    QCOMPARE(all.size(), 5);
    // Files come after the built-ins, in file-name order.
    QCOMPARE(all.at(3).id, QStringLiteral("file:a.sxcu"));
    QCOMPARE(all.at(3).name, QStringLiteral("Beta"));
    QCOMPARE(all.at(4).id, QStringLiteral("file:z.sxcu"));
    QVERIFY(all.at(4).readOnly);

    // New files show up on reload.
    write(QStringLiteral("m.sxcu"), R"({"Name": "Mid", "RequestURL": "https://m.example", "FileFormName": "f"})");
    QSignalSpy changed(&hosts, &Hosts::changed);
    hosts.reload();
    QCOMPARE(changed.count(), 1);
    QCOMPARE(hosts.all().size(), 6);
}

void UploaderTests::hostsSaveEditAndRemove() {
    QTemporaryDir config;
    QString id;
    {
        Hosts hosts(config.path(), std::make_unique<MemorySecretStore>());
        const QString definition = QString::fromUtf8(R"({"RequestURL": "https://x.example", "FileFormName": "f"})");
        id = hosts.save({}, QStringLiteral("sxcu"), QStringLiteral("  Mine  "),
                        {{QStringLiteral("definition"), definition},
                         {QStringLiteral("stray"), QStringLiteral("dropped")}});
        QVERIFY2(!id.isEmpty(), qPrintable(hosts.lastError()));
        const Host *host = hosts.find(id);
        QVERIFY(host);
        QCOMPARE(host->name, QStringLiteral("Mine"));
        QVERIFY(!host->readOnly);
        QVERIFY(!host->settings.contains(QStringLiteral("stray")));
        QCOMPARE(hosts.values(id).value(QStringLiteral("definition")).toString(), definition);

        // Saved hosts sit after the built-ins.
        QCOMPARE(hosts.all().at(3).id, id);
        QCOMPARE(hosts.list().at(3).toMap().value(QStringLiteral("providerName")).toString(),
                 QStringLiteral("Custom uploader (.sxcu)"));

        const QString renamed = hosts.save(id, QStringLiteral("sxcu"), QStringLiteral("Renamed"),
                                           {{QStringLiteral("definition"), definition}});
        QCOMPARE(renamed, id);
    }

    // hosts.json carries them over to the next start.
    Hosts reopened(config.path(), std::make_unique<MemorySecretStore>());
    QVERIFY(reopened.find(id));
    QCOMPARE(reopened.find(id)->name, QStringLiteral("Renamed"));
    reopened.remove(id);
    QVERIFY(!reopened.find(id));
    Hosts afterRemove(config.path(), std::make_unique<MemorySecretStore>());
    QVERIFY(!afterRemove.find(id));
}

void UploaderTests::hostsValidateBeforeSaving() {
    QTemporaryDir config;
    Hosts hosts(config.path(), std::make_unique<MemorySecretStore>());

    QVERIFY(hosts.save({}, QStringLiteral("nope"), QStringLiteral("x"), {}).isEmpty());
    QCOMPARE(hosts.lastError(), QStringLiteral("Unknown kind of host."));

    QVERIFY(hosts.save({}, QStringLiteral("sxcu"), QStringLiteral(" "), {}).isEmpty());
    QCOMPARE(hosts.lastError(), QStringLiteral("Give the host a name."));

    QVERIFY(hosts.save({}, QStringLiteral("sxcu"), QStringLiteral("x"), {}).isEmpty());
    QCOMPARE(hosts.lastError(), QStringLiteral("Uploader config is required."));

    const QVariantMap unusable{{QStringLiteral("definition"), QString::fromUtf8(R"({"RequestURL": "https://x"})")}};
    QVERIFY(hosts.save({}, QStringLiteral("sxcu"), QStringLiteral("x"), unusable).isEmpty());
    QVERIFY(hosts.lastError().startsWith(QStringLiteral("The uploader config can't be used")));
    QCOMPARE(hosts.all().size(), 3);
}

void UploaderTests::hostsRefuseToEditReadOnlyHosts() {
    QTemporaryDir config;
    Hosts hosts(config.path(), std::make_unique<MemorySecretStore>());
    const QString definition = hosts.values(QStringLiteral("builtin:catbox"))
                                   .value(QStringLiteral("definition"))
                                   .toString();
    const QVariantMap values{{QStringLiteral("definition"), definition}};
    QVERIFY(hosts.save(QStringLiteral("builtin:catbox"), QStringLiteral("sxcu"),
                       QStringLiteral("Mine now"), values).isEmpty());
    QCOMPARE(hosts.lastError(), QStringLiteral("This host can't be edited."));
    hosts.remove(QStringLiteral("builtin:catbox"));
    QVERIFY(hosts.find(QStringLiteral("builtin:catbox")));
}

void UploaderTests::hostsMakeJobsWithSecretsFilledIn() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.reply = "https://files.example/x.mp4";

    QTemporaryDir config;
    Hosts hosts(config.path(), std::make_unique<MemorySecretStore>());
    hosts.setNetwork(&m_network);
    const QString id = hosts.save({}, QStringLiteral("sxcu"), QStringLiteral("Mine"),
                                  {{QStringLiteral("definition"), sxcuJson(destinationFor(server))}});
    QVERIFY(!id.isEmpty());

    const UploadOutcome outcome = runJob(hosts.createJob(id, nullptr), writeClip());
    QVERIFY2(outcome.ok, qPrintable(outcome.error));
    QCOMPARE(outcome.url, QStringLiteral("https://files.example/x.mp4"));

    QVERIFY(!hosts.createJob(QStringLiteral("gone"), nullptr));
    QCOMPARE(hosts.lastError(), QStringLiteral("That host no longer exists."));
}

void UploaderTests::fileSecretStoreIsPrivate() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("nested/secrets.json"));
    FileSecretStore store(path);
    QVERIFY(store.read(QStringLiteral("a")).isEmpty());
    QVERIFY(store.write(QStringLiteral("a"), QStringLiteral("1")));
    QVERIFY(store.write(QStringLiteral("b"), QStringLiteral("2")));
    QCOMPARE(store.read(QStringLiteral("a")), QStringLiteral("1"));
    QCOMPARE(QFileInfo(path).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
             QFileDevice::Permissions());

    FileSecretStore reopened(path);
    QCOMPARE(reopened.read(QStringLiteral("b")), QStringLiteral("2"));
    reopened.remove(QStringLiteral("a"));
    reopened.remove(QStringLiteral("b"));
    QVERIFY(!QFileInfo::exists(path));
}

void UploaderTests::multipartUploadSendsArgumentsAndFile() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.reply = "https://files.example/abc.mp4\n";

    sxcu::Destination destination = destinationFor(server);
    destination.parameters = {{QStringLiteral("name"), QStringLiteral("{filename}")}};
    destination.headers = {{QStringLiteral("Authorization"), QStringLiteral("Bearer token")}};
    destination.arguments = {{QStringLiteral("reqtype"), QStringLiteral("fileupload")}};

    const UploadOutcome outcome = runUpload(destination, writeClip());
    QVERIFY2(outcome.ok, qPrintable(outcome.error));
    // A plain-text response is the link itself, trimmed.
    QCOMPARE(outcome.url, QStringLiteral("https://files.example/abc.mp4"));
    QVERIFY(outcome.deletionUrl.isEmpty());

    QCOMPARE(server.requests.size(), 1);
    const FakeHttpServer::Request &request = server.requests.first();
    QCOMPARE(request.method, QByteArray("POST"));
    QCOMPARE(request.target, QByteArray("/upload?name=clip_trimmed.mp4"));
    QCOMPARE(request.headers.value("authorization"), QByteArray("Bearer token"));
    QCOMPARE(request.headers.value("user-agent"), QByteArray("omacut"));
    QVERIFY(request.headers.value("content-type").startsWith("multipart/form-data"));
    QVERIFY(request.body.contains("name=\"reqtype\"\r\n\r\nfileupload"));
    QVERIFY(request.body.contains("name=\"fileToUpload\"; filename=\"clip_trimmed.mp4\""));
    QVERIFY(request.body.toLower().contains("content-type: video/mp4"));
    QVERIFY(request.body.contains("\r\n\r\nfake mp4 bytes\r\n"));
}

void UploaderTests::binaryUploadSendsTheFileAsTheBody() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.status = 201;

    sxcu::Destination destination = destinationFor(server);
    destination.requestMethod = QStringLiteral("PUT");
    destination.requestUrl = server.url(QStringLiteral("/files/{filename}"));
    destination.body = QStringLiteral("Binary");
    destination.url = QStringLiteral("https://cdn.example/{filename}");

    const UploadOutcome outcome = runUpload(destination, writeClip("raw bytes"));
    QVERIFY2(outcome.ok, qPrintable(outcome.error));
    QCOMPARE(outcome.url, QStringLiteral("https://cdn.example/clip_trimmed.mp4"));

    const FakeHttpServer::Request &request = server.requests.first();
    QCOMPARE(request.method, QByteArray("PUT"));
    QCOMPARE(request.target, QByteArray("/files/clip_trimmed.mp4"));
    QCOMPARE(request.headers.value("content-type"), QByteArray("video/mp4"));
    QCOMPARE(request.body, QByteArray("raw bytes"));
}

void UploaderTests::uploadReadsLinksFromTheResponse() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.reply = R"({"files": [{"url": "https://u.example/a.mp4"}], "thumb": "https://u.example/a.jpg"})";
    server.replyHeaders = {{"X-Token", "tok123"}};

    sxcu::Destination destination = destinationFor(server);
    destination.url = QStringLiteral("{json:files[0].url}");
    destination.thumbnailUrl = QStringLiteral("{json:thumb}");
    destination.deletionUrl = QStringLiteral("https://u.example/delete?token={header:x-token}");

    const UploadOutcome outcome = runUpload(destination, writeClip());
    QVERIFY2(outcome.ok, qPrintable(outcome.error));
    QCOMPARE(outcome.url, QStringLiteral("https://u.example/a.mp4"));
    QCOMPARE(outcome.thumbnailUrl, QStringLiteral("https://u.example/a.jpg"));
    QCOMPARE(outcome.deletionUrl, QStringLiteral("https://u.example/delete?token=tok123"));
}

void UploaderTests::uploadReportsHttpErrors() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.status = 413;
    server.reply = R"({"error": "File too large"})";

    sxcu::Destination destination = destinationFor(server);
    destination.errorMessage = QStringLiteral("{json:error}");
    UploadOutcome outcome = runUpload(destination, writeClip());
    QVERIFY(!outcome.ok);
    QCOMPARE(outcome.error, QStringLiteral("Fake answered HTTP 413: File too large"));

    // Without an ErrorMessage template, the body itself explains.
    server.reply = "<h1>Too big</h1>";
    destination.errorMessage.clear();
    outcome = runUpload(destination, writeClip());
    QCOMPARE(outcome.error, QStringLiteral("Fake answered HTTP 413: <h1>Too big</h1>"));
}

void UploaderTests::uploadRejectsResponsesWithoutALink() {
    FakeHttpServer server;
    QVERIFY(server.listen());
    server.reply = "Uploads are disabled right now.";

    const UploadOutcome outcome = runUpload(destinationFor(server), writeClip());
    QVERIFY(!outcome.ok);
    QCOMPARE(outcome.error, QStringLiteral("Fake didn't return a link: Uploads are disabled right now."));
}

void UploaderTests::uploadReportsUnreachableHosts() {
    // Grab a free port, then close it again so nothing is listening there.
    QTcpServer closed;
    QVERIFY(closed.listen(QHostAddress::LocalHost));
    const quint16 port = closed.serverPort();
    closed.close();

    sxcu::Destination destination;
    destination.name = QStringLiteral("Gone");
    destination.requestUrl = QStringLiteral("http://127.0.0.1:%1/").arg(port);
    destination.fileFormName = QStringLiteral("f");
    const UploadOutcome outcome = runUpload(destination, writeClip());
    QVERIFY(!outcome.ok);
    QVERIFY(!outcome.error.isEmpty());
}

void UploaderTests::uploadReportsUnreadableFiles() {
    sxcu::Destination destination;
    destination.requestUrl = QStringLiteral("http://127.0.0.1:1/");
    destination.fileFormName = QStringLiteral("f");

    const UploadOutcome outcome = runUpload(destination, m_dir.filePath(QStringLiteral("missing.mp4")));
    QVERIFY(!outcome.ok);
    QVERIFY(outcome.error.startsWith(QStringLiteral("Could not read missing.mp4")));
}

void UploaderTests::uploadCanBeCancelled() {
    // A server that accepts the connection and never answers.
    QTcpServer silent;
    QVERIFY(silent.listen(QHostAddress::LocalHost));

    sxcu::Destination destination;
    destination.name = QStringLiteral("Silent");
    destination.requestUrl = QStringLiteral("http://127.0.0.1:%1/").arg(silent.serverPort());
    destination.fileFormName = QStringLiteral("f");

    std::unique_ptr<Job> job(sxcuJob(destination));
    QSignalSpy failed(job.get(), &Job::failed);
    QSignalSpy finished(job.get(), &Job::finished);
    job->start(writeClip());
    QTRY_VERIFY(silent.hasPendingConnections());

    job->cancel();
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.first().first().toString(), QStringLiteral("Upload cancelled."));
    QCOMPARE(finished.count(), 0);

    // Cancelling again, or late, doesn't report twice.
    job->cancel();
    QTest::qWait(50);
    QCOMPARE(failed.count(), 1);
}

QTEST_GUILESS_MAIN(UploaderTests)
#include "uploader_tests.moc"

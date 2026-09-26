#include <QtTest>

#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

#include "sxcu.h"
#include "uploader.h"

// Answers every request on localhost with a canned response and keeps what
// was sent, so uploads can be checked without touching a real host.
class FakeHttpServer : public QObject {
    Q_OBJECT

public:
    struct Request {
        QByteArray method;
        QByteArray target;
        QHash<QByteArray, QByteArray> headers;
        QByteArray body;
    };

    int status = 200;
    QByteArray reply;
    QList<QPair<QByteArray, QByteArray>> replyHeaders;
    QList<Request> requests;

    FakeHttpServer() {
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost); }
    QString url(const QString &path = QStringLiteral("/upload")) const {
        return QStringLiteral("http://127.0.0.1:%1%2").arg(m_server.serverPort()).arg(path);
    }

private:
    void read(QTcpSocket *socket) {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;

        Request request;
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
        request.method = requestLine.value(0);
        request.target = requestLine.value(1);
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const qsizetype colon = lines[i].indexOf(':');
            if (colon > 0)
                request.headers.insert(lines[i].left(colon).trimmed().toLower(),
                                       lines[i].mid(colon + 1).trimmed());
        }
        const qsizetype length = request.headers.value("content-length").toLongLong();
        if (buffer.size() < headerEnd + 4 + length)
            return;
        request.body = buffer.mid(headerEnd + 4, length);
        m_buffers.remove(socket);
        requests << request;

        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " Whatever\r\n";
        for (const auto &[name, value] : replyHeaders)
            response += name + ": " + value + "\r\n";
        response += "Content-Length: " + QByteArray::number(reply.size())
            + "\r\nConnection: close\r\n\r\n" + reply;
        socket->write(response);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

struct UploadOutcome {
    bool done = false;
    bool ok = false;
    QString url;
    QString thumbnailUrl;
    QString deletionUrl;
    QString error;
};

// Runs one upload to completion.
static UploadOutcome runUpload(const sxcu::Destination &destination, const QString &path) {
    Uploader uploader;
    UploadOutcome outcome;
    QObject::connect(&uploader, &Uploader::finished, &uploader,
                     [&](const QString &url, const QString &thumb, const QString &deletion) {
                         outcome = {true, true, url, thumb, deletion, {}};
                     });
    QObject::connect(&uploader, &Uploader::failed, &uploader, [&](const QString &message) {
        outcome = {true, false, {}, {}, {}, message};
    });
    uploader.upload(destination, path);
    if (!QTest::qWaitFor([&] { return outcome.done; }, 10000))
        outcome.error = QStringLiteral("timed out");
    return outcome;
}

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
    void builtInDestinationsAreValid();
    void userDestinationsLoadSortedAndSkipBadFiles();
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

    QTemporaryDir m_dir;
};

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
    QCOMPARE(sxcu::jsonPath(json, QStringLiteral("o")), QStringLiteral(R"({"k":1})"));
    QCOMPARE(sxcu::jsonPath(json, QStringLiteral("a")), QStringLiteral("[1,2]"));
    QCOMPARE(sxcu::jsonPath(json, QStringLiteral("a[1]")), QStringLiteral("2"));
    QCOMPARE(sxcu::jsonPath("[{\"u\": \"top\"}]", QStringLiteral("[0].u")), QStringLiteral("top"));
    QCOMPARE(sxcu::jsonPath("not json", QStringLiteral("a")), QString());
    QCOMPARE(sxcu::jsonPath(json, QStringLiteral("s.deeper")), QString());
}

void UploaderTests::builtInDestinationsAreValid() {
    const QList<sxcu::Destination> destinations = uploads::builtInDestinations();
    QStringList names;
    for (const sxcu::Destination &d : destinations) {
        names << d.name;
        QVERIFY(d.requestUrl.startsWith(QStringLiteral("https://")));
    }
    QCOMPARE(names, (QStringList{QStringLiteral("Litterbox (72 hours)"), QStringLiteral("Catbox"),
                                 QStringLiteral("Uguu (3 hours)")}));
}

void UploaderTests::userDestinationsLoadSortedAndSkipBadFiles() {
    QTemporaryDir dir;
    const auto write = [&](const QString &name, const QByteArray &content) {
        QFile file(dir.filePath(name));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(content);
    };
    write(QStringLiteral("z.sxcu"), R"({"Name": "alpha", "RequestURL": "https://a.example", "FileFormName": "f"})");
    write(QStringLiteral("a.sxcu"), R"({"Name": "Beta", "RequestURL": "https://b.example", "Body": "Binary"})");
    write(QStringLiteral("broken.sxcu"), "{");
    write(QStringLiteral("ignored.json"), R"({"RequestURL": "https://c.example", "FileFormName": "f"})");

    QStringList warnings;
    const QList<sxcu::Destination> destinations = uploads::loadDestinations(dir.path(), &warnings);
    QCOMPARE(destinations.size(), 2);
    QCOMPARE(destinations.at(0).name, QStringLiteral("alpha"));
    QCOMPARE(destinations.at(1).name, QStringLiteral("Beta"));
    QCOMPARE(warnings.size(), 1);
    QVERIFY(warnings.first().startsWith(QStringLiteral("broken.sxcu: ")));

    QVERIFY(uploads::loadDestinations(dir.filePath(QStringLiteral("missing"))).isEmpty());
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

    Uploader uploader;
    QSignalSpy failed(&uploader, &Uploader::failed);
    uploader.upload(destination, m_dir.filePath(QStringLiteral("missing.mp4")));
    QCOMPARE(failed.count(), 1);
    QVERIFY(failed.first().first().toString().startsWith(QStringLiteral("Could not read missing.mp4")));
    QVERIFY(!uploader.busy());
}

void UploaderTests::uploadCanBeCancelled() {
    // A server that accepts the connection and never answers.
    QTcpServer silent;
    QVERIFY(silent.listen(QHostAddress::LocalHost));

    sxcu::Destination destination;
    destination.name = QStringLiteral("Silent");
    destination.requestUrl = QStringLiteral("http://127.0.0.1:%1/").arg(silent.serverPort());
    destination.fileFormName = QStringLiteral("f");

    Uploader uploader;
    QSignalSpy failed(&uploader, &Uploader::failed);
    QSignalSpy finished(&uploader, &Uploader::finished);
    uploader.upload(destination, writeClip());
    QVERIFY(uploader.busy());
    QTRY_VERIFY(silent.hasPendingConnections());

    uploader.cancel();
    QTRY_COMPARE(failed.count(), 1);
    QCOMPARE(failed.first().first().toString(), QStringLiteral("Upload cancelled."));
    QCOMPARE(finished.count(), 0);
    QVERIFY(!uploader.busy());
}

QTEST_GUILESS_MAIN(UploaderTests)
#include "uploader_tests.moc"

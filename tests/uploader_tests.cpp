#include <QtTest>

#include "sxcu.h"

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
};

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

QTEST_GUILESS_MAIN(UploaderTests)
#include "uploader_tests.moc"

#include "uploader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QMimeDatabase>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrlQuery>

#include <algorithm>

namespace {

// Written as .sxcu so they read the same as the configs users add themselves.
const char *const kBuiltIns[] = {
    R"sxcu({
        "Name": "Litterbox (72 hours)",
        "RequestURL": "https://litterbox.catbox.moe/resources/internals/api.php",
        "Body": "MultipartFormData",
        "Arguments": { "reqtype": "fileupload", "time": "72h" },
        "FileFormName": "fileToUpload"
    })sxcu",
    R"sxcu({
        "Name": "Catbox",
        "RequestURL": "https://catbox.moe/user/api.php",
        "Body": "MultipartFormData",
        "Arguments": { "reqtype": "fileupload" },
        "FileFormName": "fileToUpload"
    })sxcu",
    R"sxcu({
        "Name": "Uguu (3 hours)",
        "RequestURL": "https://uguu.se/upload",
        "Body": "MultipartFormData",
        "FileFormName": "files[]",
        "URL": "{json:files[0].url}",
        "ErrorMessage": "{json:description}"
    })sxcu",
};

constexpr int kStallTimeoutMs = 60000;
constexpr int kSnippetLength = 200;

QByteArray quoted(const QString &value) {
    QByteArray bytes = value.toUtf8();
    bytes.replace('\\', "\\\\").replace('"', "\\\"");
    return '"' + bytes + '"';
}

// A short, single-line piece of a response for error messages.
QString snippet(const QByteArray &body) {
    QString text = QString::fromUtf8(body).simplified();
    if (text.size() > kSnippetLength)
        text = text.left(kSnippetLength) + QStringLiteral("…");
    return text;
}

bool isWebLink(const QString &text) {
    const QUrl url(text, QUrl::StrictMode);
    return url.isValid() && !url.host().isEmpty()
        && (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http"));
}

}

namespace uploads {

QList<sxcu::Destination> builtInDestinations() {
    QList<sxcu::Destination> destinations;
    for (const char *json : kBuiltIns) {
        sxcu::Destination destination;
        if (sxcu::fromJson(json, &destination))
            destinations << destination;
    }
    return destinations;
}

QString userDestinationsDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/omacut/uploaders");
}

QList<sxcu::Destination> loadDestinations(const QString &dir, QStringList *warnings) {
    QList<sxcu::Destination> destinations;
    const QFileInfoList files = QDir(dir).entryInfoList(
        {QStringLiteral("*.sxcu")}, QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &info : files) {
        QFile file(info.filePath());
        sxcu::Destination destination;
        QString error;
        if (!file.open(QIODevice::ReadOnly))
            error = file.errorString();
        else if (sxcu::fromJson(file.readAll(), &destination, &error)) {
            if (destination.name.isEmpty())
                destination.name = info.completeBaseName();
            destinations << destination;
            continue;
        }
        if (warnings)
            *warnings << QStringLiteral("%1: %2").arg(info.fileName(), error);
    }
    std::stable_sort(destinations.begin(), destinations.end(),
                     [](const sxcu::Destination &a, const sxcu::Destination &b) {
                         return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
                     });
    return destinations;
}

}

Uploader::Uploader(QObject *parent) : QObject(parent) {}

Uploader::~Uploader() {
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
}

void Uploader::upload(const sxcu::Destination &destination, const QString &filePath) {
    if (busy())
        return;

    const QString fileName = QFileInfo(filePath).fileName();
    auto *file = new QFile(filePath);
    if (!file->open(QIODevice::ReadOnly)) {
        const QString error = file->errorString();
        delete file;
        emit failed(QStringLiteral("Could not read %1: %2").arg(fileName, error));
        return;
    }

    // Everything the request sends may use the syntax too, e.g. {filename}
    // in the URL or {random:…} to spread uploads over mirrors.
    const sxcu::Context context{nullptr, fileName};
    QUrl url(sxcu::expand(destination.requestUrl, context));
    if (!destination.parameters.isEmpty()) {
        QUrlQuery query(url);
        for (const auto &[key, value] : destination.parameters)
            query.addQueryItem(key, sxcu::expand(value, context));
        url.setQuery(query);
    }

    QNetworkRequest request(url);
    request.setTransferTimeout(kStallTimeoutMs);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("omacut"));
    for (const auto &[key, value] : destination.headers)
        request.setRawHeader(key.toUtf8(), sxcu::expand(value, context).toUtf8());

    const QString mimeType = QMimeDatabase().mimeTypeForFile(filePath).name();
    const QByteArray method = destination.requestMethod.toUtf8();
    QNetworkReply *reply = nullptr;

    if (destination.body == QStringLiteral("Binary")) {
        if (!request.hasRawHeader("Content-Type"))
            request.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
        reply = m_network.sendCustomRequest(request, method, file);
        file->setParent(reply);
    } else {
        auto *multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        for (const auto &[key, value] : destination.arguments) {
            QHttpPart part;
            part.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QByteArray("form-data; name=") + quoted(key));
            part.setBody(sxcu::expand(value, context).toUtf8());
            multipart->append(part);
        }
        QHttpPart filePart;
        filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QByteArray("form-data; name=") + quoted(destination.fileFormName)
                               + "; filename=" + quoted(fileName));
        filePart.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
        filePart.setBodyDevice(file);
        file->setParent(multipart);
        multipart->append(filePart);

        reply = m_network.sendCustomRequest(request, method, multipart);
        multipart->setParent(reply);
    }

    m_reply = reply;
    m_cancelled = false;
    connect(reply, &QNetworkReply::uploadProgress, this, &Uploader::progress);
    connect(reply, &QNetworkReply::finished, this, [this, reply, destination, fileName] {
        handleReply(reply, destination, fileName);
    });
}

void Uploader::cancel() {
    if (!m_reply)
        return;
    m_cancelled = true;
    m_reply->abort();
}

void Uploader::handleReply(QNetworkReply *reply, const sxcu::Destination &destination,
                           const QString &fileName) {
    reply->deleteLater();
    if (reply != m_reply)
        return;
    m_reply = nullptr;

    if (m_cancelled) {
        emit failed(QStringLiteral("Upload cancelled."));
        return;
    }

    sxcu::Response response;
    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.body = reply->readAll();
    response.url = reply->url();
    for (const auto &[name, value] : reply->rawHeaderPairs())
        response.headers.insert(name.toLower(), value);

    // No HTTP answer at all: DNS, refused connection, TLS, a stalled transfer.
    if (response.status == 0) {
        emit failed(reply->errorString());
        return;
    }

    const sxcu::Context context{&response, fileName};
    if (response.status < 200 || response.status >= 300) {
        QString message = sxcu::expand(destination.errorMessage, context).trimmed();
        if (message.isEmpty())
            message = snippet(response.body);
        emit failed(QStringLiteral("%1 answered HTTP %2%3")
                        .arg(destination.name)
                        .arg(response.status)
                        .arg(message.isEmpty() ? QString() : QStringLiteral(": ") + message));
        return;
    }

    const QString url = destination.url.isEmpty()
        ? QString::fromUtf8(response.body).trimmed()
        : sxcu::expand(destination.url, context).trimmed();
    if (!isWebLink(url)) {
        const QString detail = snippet(response.body);
        emit failed(QStringLiteral("%1 didn't return a link%2")
                        .arg(destination.name,
                             detail.isEmpty() ? QString() : QStringLiteral(": ") + detail));
        return;
    }

    emit finished(url, sxcu::expand(destination.thumbnailUrl, context).trimmed(),
                  sxcu::expand(destination.deletionUrl, context).trimmed());
}

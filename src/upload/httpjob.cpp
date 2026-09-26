#include "httpjob.h"

#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QMimeDatabase>
#include <QRandomGenerator>
#include <QNetworkAccessManager>
#include <QNetworkReply>

namespace upload {

namespace {
constexpr int kStallTimeoutMs = 60000;
}

QString snippet(const QByteArray &body, int length) {
    QString text = QString::fromUtf8(body).simplified();
    if (text.size() > length)
        text = text.left(length) + QStringLiteral("…");
    return text;
}

QString normalizeServerUrl(const QString &url) {
    QString text = url.trimmed();
    if (text.isEmpty())
        return {};
    if (!text.contains(QStringLiteral("://")))
        text.prepend(QStringLiteral("https://"));
    QUrl parsed(text);
    parsed.setQuery(QString());
    parsed.setFragment(QString());
    QString out = parsed.toString();
    while (out.endsWith(QLatin1Char('/')))
        out.chop(1);
    return out;
}

QString expandDateTokens(QString text, const QDateTime &now) {
    const QList<QPair<QString, QString>> tokens = {
        {QStringLiteral("%mo"), now.toString(QStringLiteral("MM"))},
        {QStringLiteral("%mi"), now.toString(QStringLiteral("mm"))},
        {QStringLiteral("%y"), now.toString(QStringLiteral("yyyy"))},
        {QStringLiteral("%d"), now.toString(QStringLiteral("dd"))},
        {QStringLiteral("%h"), now.toString(QStringLiteral("HH"))},
        {QStringLiteral("%s"), now.toString(QStringLiteral("ss"))},
    };
    for (const auto &[token, value] : tokens)
        text.replace(token, value);
    return text;
}

QString taggedFileName(const QString &fileName) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    QString tag;
    for (int i = 0; i < 6; ++i)
        tag += QLatin1Char(alphabet[QRandomGenerator::global()->bounded(36)]);
    const QFileInfo info(fileName);
    const QString suffix = info.completeSuffix();
    return info.baseName() + QLatin1Char('-') + tag
        + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
}

QString mimeTypeFor(const QString &path) {
    return QMimeDatabase().mimeTypeForFile(path).name();
}

namespace {
QByteArray quoted(const QString &value) {
    QByteArray bytes = value.toUtf8();
    bytes.replace('\\', "\\\\").replace('"', "\\\"");
    return '"' + bytes + '"';
}
}

QHttpMultiPart *formData(const QList<QPair<QString, QString>> &fields, const QString &fileField,
                         const QString &fileName, QIODevice *file, const QString &mimeType) {
    auto *multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    for (const auto &[key, value] : fields) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QByteArray("form-data; name=") + quoted(key));
        part.setBody(value.toUtf8());
        multipart->append(part);
    }
    QHttpPart filePart;
    filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QByteArray("form-data; name=") + quoted(fileField) + "; filename="
                           + quoted(fileName));
    filePart.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
    filePart.setBodyDevice(file);
    file->setParent(multipart);
    multipart->append(filePart);
    return multipart;
}

bool isWebLink(const QString &text) {
    const QUrl url(text, QUrl::StrictMode);
    return url.isValid() && !url.host().isEmpty()
        && (url.scheme() == QStringLiteral("https") || url.scheme() == QStringLiteral("http"));
}

HttpJob::HttpJob(const QString &hostName, const Services &services, QObject *parent)
    : Job(parent), m_hostName(hostName), m_services(services) {}

HttpJob::~HttpJob() {
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
}

QNetworkRequest HttpJob::newRequest(const QUrl &url) const {
    QNetworkRequest request(url);
    request.setTransferTimeout(kStallTimeoutMs);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("omacut"));
    return request;
}

QIODevice *HttpJob::openFile(const QString &path) {
    auto *file = new QFile(path, this);
    if (file->open(QIODevice::ReadOnly))
        return file;
    fail(QStringLiteral("Could not read %1: %2").arg(QFileInfo(path).fileName(), file->errorString()));
    delete file;
    return nullptr;
}

void HttpJob::send(QNetworkRequest request, const QByteArray &method, const QByteArray &body,
                   Handler handler, Progress onProgress) {
    if (m_done)
        return;
    track(m_services.network->sendCustomRequest(request, method, body), std::move(handler),
          std::move(onProgress));
}

void HttpJob::send(QNetworkRequest request, const QByteArray &method, QIODevice *body,
                   Handler handler, Progress onProgress) {
    if (m_done)
        return;
    QNetworkReply *reply = m_services.network->sendCustomRequest(request, method, body);
    if (body)
        body->setParent(reply);
    track(reply, std::move(handler), std::move(onProgress));
}

void HttpJob::send(QNetworkRequest request, const QByteArray &method, QHttpMultiPart *body,
                   Handler handler, Progress onProgress) {
    if (m_done)
        return;
    QNetworkReply *reply = m_services.network->sendCustomRequest(request, method, body);
    body->setParent(reply);
    track(reply, std::move(handler), std::move(onProgress));
}

void HttpJob::track(QNetworkReply *reply, Handler handler, Progress onProgress) {
    m_reply = reply;
    if (onProgress)
        connect(reply, &QNetworkReply::uploadProgress, this, onProgress);
    connect(reply, &QNetworkReply::finished, this, [this, reply, handler] {
        reply->deleteLater();
        if (reply == m_reply)
            m_reply = nullptr;
        if (m_done)
            return;
        if (m_cancelled) {
            fail(QStringLiteral("Upload cancelled."));
            return;
        }

        Reply result;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.body = reply->readAll();
        result.url = reply->url();
        for (const auto &[name, value] : reply->rawHeaderPairs())
            result.headers.insert(name.toLower(), value);
        if (result.status == 0)
            result.networkError = reply->errorString();
        handler(result);
    });
}

QString HttpJob::httpError(const Reply &reply, const QString &why) const {
    const QString detail = why.isEmpty() ? snippet(reply.body) : why;
    return QStringLiteral("%1 answered HTTP %2%3")
        .arg(m_hostName)
        .arg(reply.status)
        .arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail);
}

QString HttpJob::requestError(const Reply &reply, const QString &why) const {
    if (reply.status == 0)
        return QStringLiteral("%1: %2").arg(m_hostName, reply.networkError);
    return httpError(reply, why);
}

void HttpJob::succeed(const Outcome &outcome) {
    if (m_done)
        return;
    m_done = true;
    emit finished(outcome);
}

void HttpJob::fail(const QString &message) {
    if (m_done)
        return;
    m_done = true;
    emit failed(message);
}

void HttpJob::cancel() {
    if (m_done || m_cancelled)
        return;
    m_cancelled = true;
    onCancelled();
    if (m_reply)
        m_reply->abort();
    else
        fail(QStringLiteral("Upload cancelled."));
}

}

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>

#include "httpjob.h"
#include "providers.h"

namespace upload {

namespace {

const QString kDefaultRedirect = QStringLiteral("http://127.0.0.1:52475/oauth2/callback");
// Dropbox takes up to 150 MB per request; bigger files go in sessions.
constexpr qint64 kSingleUploadLimit = 150LL * 1024 * 1024;
constexpr qint64 kChunkSize = 32LL * 1024 * 1024;
constexpr int kSignInTimeoutMs = 5 * 60 * 1000;

struct Endpoints {
    QString web = QStringLiteral("https://www.dropbox.com");
    QString api = QStringLiteral("https://api.dropboxapi.com");
    QString content = QStringLiteral("https://content.dropboxapi.com");
};

// Tests point everything at one local server.
Endpoints endpointsFor(const QVariantMap &settings) {
    Endpoints endpoints;
    const QString base = settings.value(QStringLiteral("baseUrl")).toString();
    if (!base.isEmpty())
        endpoints = {base, base, base};
    return endpoints;
}

// Dropbox-API-Arg is an HTTP header, so anything outside ASCII is escaped.
QByteArray asciiJson(const QJsonObject &object) {
    const QString json = QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
    QByteArray out;
    for (const QChar c : json) {
        if (c.unicode() < 0x80)
            out += char(c.unicode());
        else
            out += QStringLiteral("\\u%1").arg(uint(c.unicode()), 4, 16, QLatin1Char('0')).toLatin1();
    }
    return out;
}

QString errorSummary(const QByteArray &body) {
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    QString summary = object.value(QStringLiteral("error_summary")).toString();
    if (summary.isEmpty())
        summary = object.value(QStringLiteral("error_description")).toString();
    // "path/insufficient_space/.." -> "path/insufficient_space"
    while (summary.endsWith(QLatin1Char('.')) || summary.endsWith(QLatin1Char('/')))
        summary.chop(1);
    return summary;
}

QString randomToken(int length) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    QString token;
    for (int i = 0; i < length; ++i)
        token += QLatin1Char(alphabet[QRandomGenerator::system()->bounded(66)]);
    return token;
}

class DropboxJob : public HttpJob {
public:
    DropboxJob(const QVariantMap &settings, const Services &services, QObject *parent)
        : HttpJob(settings.value(QStringLiteral("name")).toString(), services, parent),
          m_settings(settings), m_endpoints(endpointsFor(settings)) {
        // Tests shrink both the single-request limit and the chunks.
        const qint64 chunkSize = settings.value(QStringLiteral("chunkSize")).toLongLong();
        if (chunkSize > 0) {
            m_chunkSize = chunkSize;
            m_singleLimit = chunkSize;
        }
    }

    void start(const QString &filePath) override {
        m_file = openFile(filePath);
        if (!m_file)
            return;
        m_size = m_file->size();

        QString folder = expandDateTokens(m_settings.value(QStringLiteral("folder")).toString(),
                                          QDateTime::currentDateTime()).trimmed();
        while (folder.endsWith(QLatin1Char('/')))
            folder.chop(1);
        if (!folder.startsWith(QLatin1Char('/')))
            folder.prepend(QLatin1Char('/'));
        m_path = (folder == QStringLiteral("/") ? QString() : folder) + QLatin1Char('/')
            + QFileInfo(filePath).fileName();
        refreshToken();
    }

private:
    QNetworkRequest apiRequest(const QString &url, const QByteArray &contentType) const {
        QNetworkRequest request = newRequest(QUrl(url));
        request.setRawHeader("Authorization", "Bearer " + m_accessToken.toUtf8());
        request.setRawHeader("Content-Type", contentType);
        return request;
    }

    QString failure(const Reply &reply) const {
        return requestError(reply, errorSummary(reply.body));
    }

    // Access tokens last hours; the stored refresh token gets a fresh one.
    void refreshToken() {
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
        form.addQueryItem(QStringLiteral("refresh_token"),
                          m_settings.value(QStringLiteral("refreshToken")).toString());
        form.addQueryItem(QStringLiteral("client_id"),
                          m_settings.value(QStringLiteral("appKey")).toString().trimmed());
        QNetworkRequest request = newRequest(QUrl(m_endpoints.api + QStringLiteral("/oauth2/token")));
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
        send(request, "POST", form.toString(QUrl::FullyEncoded).toUtf8(), [this](const Reply &reply) {
            m_accessToken = QJsonDocument::fromJson(reply.body).object()
                                .value(QStringLiteral("access_token")).toString();
            if (!reply.ok() || m_accessToken.isEmpty()) {
                fail(reply.status == 400 || reply.status == 401
                         ? QStringLiteral("%1 needs signing in again.").arg(m_hostName)
                         : failure(reply));
                return;
            }
            if (m_size > m_singleLimit)
                startSession();
            else
                uploadWhole();
        });
    }

    QJsonObject commitInfo() const {
        return {{QStringLiteral("path"), m_path},
                {QStringLiteral("mode"), QStringLiteral("add")},
                {QStringLiteral("autorename"), true},
                {QStringLiteral("mute"), true}};
    }

    Progress progressFrom(qint64 offset) {
        return [this, offset](qint64 sent, qint64 total) {
            if (total > 0)
                emit progress(offset + sent, m_size);
        };
    }

    void uploadWhole() {
        QNetworkRequest request = apiRequest(m_endpoints.content + QStringLiteral("/2/files/upload"),
                                             "application/octet-stream");
        request.setRawHeader("Dropbox-API-Arg", asciiJson(commitInfo()));
        send(request, "POST", m_file, [this](const Reply &reply) { uploaded(reply); },
             progressFrom(0));
        m_file = nullptr;
    }

    void startSession() {
        QNetworkRequest request = apiRequest(
            m_endpoints.content + QStringLiteral("/2/files/upload_session/start"),
            "application/octet-stream");
        request.setRawHeader("Dropbox-API-Arg", asciiJson({{QStringLiteral("close"), false}}));
        const QByteArray chunk = m_file->read(m_chunkSize);
        send(request, "POST", chunk, [this, length = chunk.size()](const Reply &reply) {
            m_sessionId = QJsonDocument::fromJson(reply.body).object()
                              .value(QStringLiteral("session_id")).toString();
            if (!reply.ok() || m_sessionId.isEmpty()) {
                fail(failure(reply));
                return;
            }
            appendChunk(length);
        }, progressFrom(0));
    }

    // Sends the chunk at offset, or finishes the session with the last one.
    void appendChunk(qint64 offset) {
        const QByteArray chunk = m_file->read(m_chunkSize);
        const bool last = offset + chunk.size() >= m_size;
        const QJsonObject cursor{{QStringLiteral("session_id"), m_sessionId},
                                 {QStringLiteral("offset"), offset}};
        QNetworkRequest request = apiRequest(
            m_endpoints.content
                + (last ? QStringLiteral("/2/files/upload_session/finish")
                        : QStringLiteral("/2/files/upload_session/append_v2")),
            "application/octet-stream");
        request.setRawHeader("Dropbox-API-Arg",
                             asciiJson(last ? QJsonObject{{QStringLiteral("cursor"), cursor},
                                                          {QStringLiteral("commit"), commitInfo()}}
                                            : QJsonObject{{QStringLiteral("cursor"), cursor}}));
        send(request, "POST", chunk, [this, last, next = offset + chunk.size()](const Reply &reply) {
            if (last) {
                uploaded(reply);
                return;
            }
            if (!reply.ok()) {
                fail(failure(reply));
                return;
            }
            appendChunk(next);
        }, progressFrom(offset));
    }

    void uploaded(const Reply &reply) {
        const QJsonObject metadata = QJsonDocument::fromJson(reply.body).object();
        // autorename may have changed the name.
        const QString path = metadata.value(QStringLiteral("path_lower")).toString();
        if (!reply.ok() || path.isEmpty()) {
            fail(failure(reply));
            return;
        }
        share(path);
    }

    void share(const QString &path) {
        QNetworkRequest request = apiRequest(
            m_endpoints.api + QStringLiteral("/2/sharing/create_shared_link_with_settings"),
            "application/json");
        const QJsonObject body{{QStringLiteral("path"), path}};
        send(request, "POST", QJsonDocument(body).toJson(QJsonDocument::Compact),
             [this](const Reply &reply) {
                 const QJsonObject object = QJsonDocument::fromJson(reply.body).object();
                 QString url = object.value(QStringLiteral("url")).toString();
                 // A second upload to the same path already has a link.
                 if (url.isEmpty()) {
                     url = object.value(QStringLiteral("error")).toObject()
                               .value(QStringLiteral("shared_link_already_exists")).toObject()
                               .value(QStringLiteral("metadata")).toObject()
                               .value(QStringLiteral("url")).toString();
                 }
                 if (!isWebLink(url)) {
                     fail(failure(reply));
                     return;
                 }
                 succeed({directLink(url), {}, {}});
             });
    }

    QString directLink(const QString &url) const {
        if (!m_settings.value(QStringLiteral("directLink")).toBool())
            return url;
        QUrl link(url);
        QUrlQuery query(link);
        query.removeAllQueryItems(QStringLiteral("dl"));
        query.addQueryItem(QStringLiteral("raw"), QStringLiteral("1"));
        link.setQuery(query);
        return link.toString(QUrl::FullyEncoded);
    }

    const QVariantMap m_settings;
    const Endpoints m_endpoints;
    qint64 m_chunkSize = kChunkSize;
    qint64 m_singleLimit = kSingleUploadLimit;
    QIODevice *m_file = nullptr;
    qint64 m_size = 0;
    QString m_path;
    QString m_accessToken;
    QString m_sessionId;
};

// OAuth 2 with PKCE: the browser signs in and Dropbox redirects back to a
// one-off listener on the loopback address with a code to trade for a
// refresh token. No client secret is involved.
class DropboxSignIn : public Authorization {
public:
    DropboxSignIn(const QVariantMap &settings, const Services &services, QObject *parent)
        : Authorization(parent), m_services(services), m_endpoints(endpointsFor(settings)),
          m_appKey(settings.value(QStringLiteral("appKey")).toString().trimmed()),
          m_redirect(settings.value(QStringLiteral("redirectUri"), kDefaultRedirect).toString()) {
        m_timeout.setSingleShot(true);
        m_timeout.setInterval(kSignInTimeoutMs);
        connect(&m_timeout, &QTimer::timeout, this, [this] {
            finish(QStringLiteral("The Dropbox sign-in timed out."));
        });
        connect(&m_listener, &QTcpServer::newConnection, this, &DropboxSignIn::accept);
    }

    void start() override {
        if (m_appKey.isEmpty()) {
            emit failed(QStringLiteral("Enter your Dropbox app key first."));
            return;
        }
        const QUrl redirect(m_redirect);
        if (!m_listener.listen(QHostAddress::LocalHost, quint16(redirect.port()))) {
            emit failed(QStringLiteral("Could not listen on %1 for the sign-in: %2")
                            .arg(m_redirect, m_listener.errorString()));
            return;
        }

        m_verifier = randomToken(64);
        m_state = randomToken(24);
        const QByteArray challenge =
            QCryptographicHash::hash(m_verifier.toLatin1(), QCryptographicHash::Sha256)
                .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);

        QUrl authorize(m_endpoints.web + QStringLiteral("/oauth2/authorize"));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("client_id"), m_appKey);
        query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
        query.addQueryItem(QStringLiteral("token_access_type"), QStringLiteral("offline"));
        query.addQueryItem(QStringLiteral("code_challenge"), QString::fromLatin1(challenge));
        query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
        query.addQueryItem(QStringLiteral("redirect_uri"), m_redirect);
        query.addQueryItem(QStringLiteral("state"), m_state);
        authorize.setQuery(query);

        m_timeout.start();
        m_services.openUrl(authorize);
        emit status(QStringLiteral("Finish signing in to Dropbox in your browser…"));
    }

    void cancel() override { finish(QStringLiteral("Sign-in cancelled.")); }

private:
    void accept() {
        while (QTcpSocket *socket = m_listener.nextPendingConnection()) {
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                QByteArray &buffer = m_buffers[socket];
                buffer += socket->readAll();
                if (!buffer.contains("\r\n\r\n"))
                    return;
                const QByteArray target = buffer.left(buffer.indexOf("\r\n")).split(' ').value(1);
                m_buffers.remove(socket);
                handleRedirect(socket, QUrl::fromEncoded("http://localhost" + target));
            });
        }
    }

    void respond(QTcpSocket *socket, const QString &message) {
        const QByteArray html = "<!doctype html><meta charset=utf-8><title>omacut</title>"
                                "<body style=\"font:16px sans-serif;margin:3em\">"
            + message.toHtmlEscaped().toUtf8() + "</body>";
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: "
                      + QByteArray::number(html.size()) + "\r\nConnection: close\r\n\r\n" + html);
        socket->disconnectFromHost();
    }

    void handleRedirect(QTcpSocket *socket, const QUrl &url) {
        const QUrlQuery query(url);
        if (url.path() != QUrl(m_redirect).path() || m_exchanging) {
            respond(socket, QStringLiteral("Nothing to see here."));
            return;
        }
        if (query.queryItemValue(QStringLiteral("state")) != m_state) {
            respond(socket, QStringLiteral("This sign-in doesn't match the one omacut started."));
            return;
        }
        const QString code = query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
        if (code.isEmpty()) {
            const QString error = query.queryItemValue(QStringLiteral("error_description"),
                                                       QUrl::FullyDecoded);
            respond(socket, QStringLiteral("Dropbox wasn't connected. You can close this tab."));
            finish(error.isEmpty() ? QStringLiteral("Dropbox access wasn't granted.")
                                   : QStringLiteral("Dropbox: %1").arg(error));
            return;
        }
        respond(socket, QStringLiteral("Dropbox is connected. You can close this tab and go back to omacut."));
        m_listener.close();
        exchange(code);
    }

    void exchange(const QString &code) {
        m_exchanging = true;
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
        form.addQueryItem(QStringLiteral("code"), code);
        form.addQueryItem(QStringLiteral("client_id"), m_appKey);
        form.addQueryItem(QStringLiteral("redirect_uri"), m_redirect);
        form.addQueryItem(QStringLiteral("code_verifier"), m_verifier);
        QNetworkRequest request(QUrl(m_endpoints.api + QStringLiteral("/oauth2/token")));
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
        m_reply = m_services.network->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
        connect(m_reply, &QNetworkReply::finished, this, [this] {
            QNetworkReply *reply = m_reply;
            m_reply = nullptr;
            reply->deleteLater();
            const QByteArray body = reply->readAll();
            const QJsonObject token = QJsonDocument::fromJson(body).object();
            const QString refresh = token.value(QStringLiteral("refresh_token")).toString();
            if (refresh.isEmpty()) {
                const QString why = errorSummary(body);
                finish(QStringLiteral("Dropbox didn't grant access: %1")
                           .arg(why.isEmpty() ? reply->errorString() : why));
                return;
            }
            m_refreshToken = refresh;
            fetchAccount(token.value(QStringLiteral("access_token")).toString());
        });
    }

    // Only for the "Connected as …" line; failing it doesn't matter.
    void fetchAccount(const QString &accessToken) {
        QNetworkRequest request(QUrl(m_endpoints.api + QStringLiteral("/2/users/get_current_account")));
        request.setRawHeader("Authorization", "Bearer " + accessToken.toUtf8());
        m_reply = m_services.network->post(request, QByteArray());
        connect(m_reply, &QNetworkReply::finished, this, [this] {
            QNetworkReply *reply = m_reply;
            m_reply = nullptr;
            reply->deleteLater();
            const QString name = QJsonDocument::fromJson(reply->readAll()).object()
                                     .value(QStringLiteral("name")).toObject()
                                     .value(QStringLiteral("display_name")).toString();
            m_timeout.stop();
            m_done = true;
            emit finished({{QStringLiteral("refreshToken"), m_refreshToken}},
                          name.isEmpty() ? QStringLiteral("Connected to Dropbox")
                                         : QStringLiteral("Connected as %1").arg(name));
        });
    }

    void finish(const QString &error) {
        if (m_done)
            return;
        m_done = true;
        m_timeout.stop();
        m_listener.close();
        if (m_reply) {
            m_reply->disconnect(this);
            m_reply->abort();
            m_reply->deleteLater();
            m_reply = nullptr;
        }
        emit failed(error);
    }

    Services m_services;
    const Endpoints m_endpoints;
    const QString m_appKey;
    const QString m_redirect;
    QTcpServer m_listener;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QTimer m_timeout;
    QNetworkReply *m_reply = nullptr;
    QString m_verifier;
    QString m_state;
    QString m_refreshToken;
    bool m_exchanging = false;
    bool m_done = false;
};

class DropboxProvider : public Provider {
public:
    QString id() const override { return QStringLiteral("dropbox"); }
    QString name() const override { return QStringLiteral("Dropbox"); }
    QString description() const override {
        return QStringLiteral("Save the clip to your Dropbox and share a link to it.");
    }

    QList<Field> fields() const override {
        QList<Field> fields;
        Field f;

        f = {};
        f.key = QStringLiteral("appKey");
        f.label = QStringLiteral("App key");
        f.required = true;
        f.help = QStringLiteral("Create an app at dropbox.com/developers/apps with the "
                                "files.content.write and sharing.write permissions, and add %1 "
                                "as its redirect URI. Then sign in.")
                     .arg(kDefaultRedirect);
        fields << f;

        f = {};
        f.key = QStringLiteral("folder");
        f.label = QStringLiteral("Folder");
        f.defaultValue = QStringLiteral("/omacut");
        f.help = QStringLiteral("Inside the app's folder when it has App folder access.");
        fields << f;

        f = {};
        f.key = QStringLiteral("directLink");
        f.label = QStringLiteral("Link straight to the video file");
        f.type = Field::Toggle;
        f.defaultValue = false;
        fields << f;

        f = {};
        f.key = QStringLiteral("refreshToken");
        f.label = QStringLiteral("Dropbox sign-in");
        f.type = Field::Secret;
        f.hidden = true;
        fields << f;

        for (const char *key : {"redirectUri", "baseUrl", "chunkSize"}) {
            f = {};
            f.key = QString::fromLatin1(key);
            f.label = f.key;
            f.hidden = true;
            fields << f;
        }
        return fields;
    }

    QString validate(const QVariantMap &settings) const override {
        const QString missing = Provider::validate(settings);
        if (!missing.isEmpty())
            return missing;
        if (settings.value(QStringLiteral("refreshToken")).toString().isEmpty())
            return QStringLiteral("Sign in with Dropbox first.");
        return {};
    }

    Job *createJob(const QVariantMap &settings, const Services &services,
                   QObject *parent) const override {
        return new DropboxJob(settings, services, parent);
    }

    QString authorizeLabel() const override { return QStringLiteral("Sign in with Dropbox"); }

    Authorization *authorize(const QVariantMap &settings, const Services &services,
                             QObject *parent) const override {
        return new DropboxSignIn(settings, services, parent);
    }
};

}

const Provider *dropboxProvider() {
    static const DropboxProvider provider;
    return &provider;
}

}

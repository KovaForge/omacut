#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QUrlQuery>

#include "httpjob.h"
#include "providers.h"
#include "sigv4.h"

namespace upload {

namespace {

// Nextcloud's login tokens last 20 minutes.
constexpr int kLoginTimeoutMs = 20 * 60 * 1000;
constexpr int kLoginPollMs = 2000;

QByteArray basicAuth(const QString &user, const QString &password) {
    return "Basic " + (user + QLatin1Char(':') + password).toUtf8().toBase64();
}

// Each segment encoded, the slashes between them kept.
QString encodePath(const QString &path) {
    return QString::fromLatin1(sigv4::uriEncode(path, false));
}

QStringList folderSegments(const QString &folder) {
    QStringList segments;
    for (const QString &segment : folder.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (segment != QStringLiteral(".") && segment != QStringLiteral(".."))
            segments << segment;
    }
    return segments;
}

class NextcloudJob : public HttpJob {
public:
    NextcloudJob(const QVariantMap &settings, const Services &services, QObject *parent)
        : HttpJob(settings.value(QStringLiteral("name")).toString(), services, parent),
          m_settings(settings),
          m_server(normalizeServerUrl(settings.value(QStringLiteral("serverUrl")).toString())),
          m_user(settings.value(QStringLiteral("username")).toString().trimmed()),
          m_auth(basicAuth(m_user, settings.value(QStringLiteral("appPassword")).toString())) {}

    void start(const QString &filePath) override {
        m_file = openFile(filePath);
        if (!m_file)
            return;
        m_mimeType = mimeTypeFor(filePath);
        m_folders = folderSegments(expandDateTokens(
            m_settings.value(QStringLiteral("folder")).toString(), QDateTime::currentDateTime()));
        m_fileName = taggedFileName(QFileInfo(filePath).fileName());
        makeFolder(0);
    }

private:
    QString davUrl(const QString &relativePath) const {
        return m_server + QStringLiteral("/remote.php/dav/files/")
            + QString::fromLatin1(sigv4::uriEncode(m_user, true)) + QLatin1Char('/')
            + encodePath(relativePath);
    }

    QNetworkRequest authed(const QString &url) const {
        QNetworkRequest request = newRequest(QUrl::fromEncoded(url.toUtf8()));
        request.setRawHeader("Authorization", m_auth);
        return request;
    }

    // Creates each folder in turn; one that already exists answers 405.
    void makeFolder(int depth) {
        if (depth >= m_folders.size()) {
            put();
            return;
        }
        const QString path = m_folders.mid(0, depth + 1).join(QLatin1Char('/'));
        send(authed(davUrl(path)), "MKCOL", QByteArray(), [this, depth](const Reply &reply) {
            if (!reply.ok() && reply.status != 405) {
                fail(requestError(reply, reply.status == 401
                                             ? QStringLiteral("check the user name and app password")
                                             : QString()));
                return;
            }
            makeFolder(depth + 1);
        });
    }

    QString relativeFilePath() const {
        return (m_folders + QStringList{m_fileName}).join(QLatin1Char('/'));
    }

    void put() {
        QNetworkRequest request = authed(davUrl(relativeFilePath()));
        request.setHeader(QNetworkRequest::ContentTypeHeader, m_mimeType);
        send(request, "PUT", m_file, [this](const Reply &reply) {
            if (!reply.ok()) {
                fail(requestError(reply));
                return;
            }
            share();
        }, [this](qint64 sent, qint64 total) { emit progress(sent, total); });
        m_file = nullptr;
    }

    void share() {
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("path"), QLatin1Char('/') + relativeFilePath());
        form.addQueryItem(QStringLiteral("shareType"), QStringLiteral("3"));
        form.addQueryItem(QStringLiteral("permissions"), QStringLiteral("1"));
        const int days = m_settings.value(QStringLiteral("expireDays")).toInt();
        if (days > 0) {
            form.addQueryItem(QStringLiteral("expireDate"),
                              QDate::currentDate().addDays(days).toString(Qt::ISODate));
        }

        QNetworkRequest request = authed(
            m_server + QStringLiteral("/ocs/v2.php/apps/files_sharing/api/v1/shares?format=json"));
        request.setRawHeader("OCS-APIRequest", "true");
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
        send(request, "POST", form.toString(QUrl::FullyEncoded).toUtf8(), [this](const Reply &reply) {
            const QJsonObject ocs = QJsonDocument::fromJson(reply.body).object()
                                        .value(QStringLiteral("ocs")).toObject();
            QString url = ocs.value(QStringLiteral("data")).toObject()
                              .value(QStringLiteral("url")).toString();
            if (!reply.ok() || !isWebLink(url)) {
                const QString message = ocs.value(QStringLiteral("meta")).toObject()
                                            .value(QStringLiteral("message")).toString();
                fail(requestError(reply, message.isEmpty()
                                             ? QStringLiteral("the file was uploaded but not shared")
                                             : message));
                return;
            }
            if (m_settings.value(QStringLiteral("directLink")).toBool())
                url += QStringLiteral("/download");
            succeed({url, {}, {}});
        });
    }

    const QVariantMap m_settings;
    const QString m_server;
    const QString m_user;
    const QByteArray m_auth;
    QIODevice *m_file = nullptr;
    QString m_mimeType;
    QStringList m_folders;
    QString m_fileName;
};

// Nextcloud's Login Flow v2: the browser signs in and grants an app
// password, which is polled for here.
class NextcloudLogin : public Authorization {
public:
    NextcloudLogin(const QString &server, const Services &services, QObject *parent)
        : Authorization(parent), m_server(server), m_services(services) {
        m_pollTimer.setInterval(kLoginPollMs);
        connect(&m_pollTimer, &QTimer::timeout, this, &NextcloudLogin::poll);
        m_deadline.setSingleShot(true);
        m_deadline.setInterval(kLoginTimeoutMs);
        connect(&m_deadline, &QTimer::timeout, this, [this] {
            stop();
            emit failed(QStringLiteral("The Nextcloud sign-in timed out."));
        });
    }

    void start() override {
        if (m_server.isEmpty()) {
            emit failed(QStringLiteral("Enter the server address first."));
            return;
        }
        QNetworkRequest request(QUrl(m_server + QStringLiteral("/index.php/login/v2")));
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("omacut"));
        m_reply = m_services.network->post(request, QByteArray());
        connect(m_reply, &QNetworkReply::finished, this, [this] {
            QNetworkReply *reply = m_reply;
            m_reply = nullptr;
            reply->deleteLater();
            const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
            const QJsonObject pollInfo = body.value(QStringLiteral("poll")).toObject();
            m_token = pollInfo.value(QStringLiteral("token")).toString();
            m_endpoint = QUrl(pollInfo.value(QStringLiteral("endpoint")).toString());
            const QUrl login(body.value(QStringLiteral("login")).toString());
            if (reply->error() != QNetworkReply::NoError || m_token.isEmpty() || !login.isValid()) {
                emit failed(QStringLiteral("%1 didn't start a sign-in: %2")
                                .arg(m_server, reply->errorString()));
                return;
            }
            m_services.openUrl(login);
            emit status(QStringLiteral("Finish signing in to Nextcloud in your browser…"));
            m_deadline.start();
            m_pollTimer.start();
            poll();
        });
    }

    void cancel() override {
        stop();
        emit failed(QStringLiteral("Sign-in cancelled."));
    }

private:
    void stop() {
        m_pollTimer.stop();
        m_deadline.stop();
        if (m_reply) {
            m_reply->disconnect(this);
            m_reply->abort();
            m_reply->deleteLater();
            m_reply = nullptr;
        }
    }

    void poll() {
        if (m_reply)
            return;
        QNetworkRequest request(m_endpoint);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/x-www-form-urlencoded"));
        m_reply = m_services.network->post(
            request, "token=" + QUrl::toPercentEncoding(m_token));
        connect(m_reply, &QNetworkReply::finished, this, [this] {
            QNetworkReply *reply = m_reply;
            m_reply = nullptr;
            reply->deleteLater();
            // 404 until the user has granted access.
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
                return;
            const QJsonObject body = QJsonDocument::fromJson(reply->readAll()).object();
            const QString user = body.value(QStringLiteral("loginName")).toString();
            const QString password = body.value(QStringLiteral("appPassword")).toString();
            if (user.isEmpty() || password.isEmpty())
                return;
            stop();
            QVariantMap changes{{QStringLiteral("username"), user},
                                {QStringLiteral("appPassword"), password}};
            const QString server = body.value(QStringLiteral("server")).toString();
            if (!server.isEmpty())
                changes.insert(QStringLiteral("serverUrl"), normalizeServerUrl(server));
            emit finished(changes, QStringLiteral("Signed in as %1").arg(user));
        });
    }

    const QString m_server;
    Services m_services;
    QNetworkReply *m_reply = nullptr;
    QString m_token;
    QUrl m_endpoint;
    QTimer m_pollTimer;
    QTimer m_deadline;
};

class NextcloudProvider : public Provider {
public:
    QString id() const override { return QStringLiteral("nextcloud"); }
    QString name() const override { return QStringLiteral("Nextcloud"); }
    QString description() const override {
        return QStringLiteral("Upload to your Nextcloud and share a public link.");
    }

    QList<Field> fields() const override {
        QList<Field> fields;
        Field f;

        f = {};
        f.key = QStringLiteral("serverUrl");
        f.label = QStringLiteral("Server");
        f.placeholder = QStringLiteral("https://cloud.example.com");
        f.required = true;
        fields << f;

        f = {};
        f.key = QStringLiteral("username");
        f.label = QStringLiteral("User name");
        f.required = true;
        f.help = QStringLiteral("Or save the server and use \"Sign in with Nextcloud\".");
        fields << f;

        f = {};
        f.key = QStringLiteral("appPassword");
        f.label = QStringLiteral("App password");
        f.type = Field::Secret;
        f.required = true;
        f.help = QStringLiteral("Create one under Settings › Security in Nextcloud.");
        fields << f;

        f = {};
        f.key = QStringLiteral("folder");
        f.label = QStringLiteral("Folder");
        f.defaultValue = QStringLiteral("omacut/%y-%mo");
        fields << f;

        f = {};
        f.key = QStringLiteral("expireDays");
        f.label = QStringLiteral("Links expire after (days, 0 = never)");
        f.type = Field::Number;
        f.defaultValue = 0;
        fields << f;

        f = {};
        f.key = QStringLiteral("directLink");
        f.label = QStringLiteral("Link straight to the video file");
        f.type = Field::Toggle;
        f.defaultValue = false;
        fields << f;

        return fields;
    }

    Job *createJob(const QVariantMap &settings, const Services &services,
                   QObject *parent) const override {
        return new NextcloudJob(settings, services, parent);
    }

    QString authorizeLabel() const override { return QStringLiteral("Sign in with Nextcloud"); }

    Authorization *authorize(const QVariantMap &settings, const Services &services,
                             QObject *parent) const override {
        return new NextcloudLogin(
            normalizeServerUrl(settings.value(QStringLiteral("serverUrl")).toString()), services,
            parent);
    }
};

}

const Provider *nextcloudProvider() {
    static const NextcloudProvider provider;
    return &provider;
}

}

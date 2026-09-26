#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "httpjob.h"
#include "providers.h"

namespace upload {

namespace {

QString jsonMessage(const QByteArray &body) {
    const QJsonValue message = QJsonDocument::fromJson(body).object().value(QStringLiteral("message"));
    if (message.isArray()) {
        QStringList parts;
        for (const QJsonValue &part : message.toArray())
            parts << part.toString();
        return parts.join(QStringLiteral("; "));
    }
    return message.toString();
}

class ImmichJob : public HttpJob {
public:
    ImmichJob(const QVariantMap &settings, const Services &services, QObject *parent)
        : HttpJob(settings.value(QStringLiteral("name")).toString(), services, parent),
          m_settings(settings),
          m_server(normalizeServerUrl(settings.value(QStringLiteral("serverUrl")).toString())) {
        // People paste the API base as often as the server itself.
        if (m_server.endsWith(QStringLiteral("/api")))
            m_server.chop(4);
    }

    void start(const QString &filePath) override {
        QIODevice *file = openFile(filePath);
        if (!file)
            return;

        const QFileInfo info(filePath);
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        const QList<QPair<QString, QString>> fields = {
            {QStringLiteral("deviceAssetId"),
             QStringLiteral("omacut-%1-%2").arg(info.fileName()).arg(QDateTime::currentMSecsSinceEpoch())},
            {QStringLiteral("deviceId"), QStringLiteral("omacut")},
            {QStringLiteral("fileCreatedAt"), now},
            {QStringLiteral("fileModifiedAt"), now},
            {QStringLiteral("filename"), info.fileName()},
        };
        QHttpMultiPart *body = formData(fields, QStringLiteral("assetData"), info.fileName(), file,
                                        mimeTypeFor(filePath));
        send(request(QStringLiteral("/api/assets")), "POST", body, [this](const Reply &reply) {
            const QString id = QJsonDocument::fromJson(reply.body).object()
                                   .value(QStringLiteral("id")).toString();
            if (!reply.ok() || id.isEmpty()) {
                fail(requestError(reply, jsonMessage(reply.body)));
                return;
            }
            if (m_settings.value(QStringLiteral("shareLink"), true).toBool())
                share(id);
            else
                succeed({m_server + QStringLiteral("/photos/") + id, {}, {}});
        }, [this](qint64 sent, qint64 total) { emit progress(sent, total); });
    }

private:
    QNetworkRequest request(const QString &path) const {
        QNetworkRequest request = newRequest(QUrl(m_server + path));
        request.setRawHeader("x-api-key", m_settings.value(QStringLiteral("apiKey")).toString().toUtf8());
        request.setRawHeader("Accept", "application/json");
        return request;
    }

    void share(const QString &assetId) {
        QJsonObject link{
            {QStringLiteral("type"), QStringLiteral("INDIVIDUAL")},
            {QStringLiteral("assetIds"), QJsonArray{assetId}},
            {QStringLiteral("allowDownload"), true},
            {QStringLiteral("showMetadata"), true},
        };
        const int days = m_settings.value(QStringLiteral("expireDays")).toInt();
        if (days > 0) {
            link.insert(QStringLiteral("expiresAt"),
                        QDateTime::currentDateTimeUtc().addDays(days).toString(Qt::ISODateWithMs));
        }

        QNetworkRequest req = request(QStringLiteral("/api/shared-links"));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        send(req, "POST", QJsonDocument(link).toJson(QJsonDocument::Compact),
             [this, assetId](const Reply &reply) {
                 const QJsonObject body = QJsonDocument::fromJson(reply.body).object();
                 const QString slug = body.value(QStringLiteral("slug")).toString();
                 const QString key = body.value(QStringLiteral("key")).toString();
                 if (!reply.ok() || (slug.isEmpty() && key.isEmpty())) {
                     fail(requestError(reply, jsonMessage(reply.body)));
                     return;
                 }
                 QString base = normalizeServerUrl(m_settings.value(QStringLiteral("publicUrl")).toString());
                 if (base.isEmpty())
                     base = m_server;
                 succeed({slug.isEmpty() ? base + QStringLiteral("/share/") + key
                                         : base + QStringLiteral("/s/") + slug,
                          m_server + QStringLiteral("/api/assets/") + assetId + QStringLiteral("/thumbnail"),
                          {}});
             });
    }

    const QVariantMap m_settings;
    QString m_server;
};

class ImmichProvider : public Provider {
public:
    QString id() const override { return QStringLiteral("immich"); }
    QString name() const override { return QStringLiteral("Immich"); }
    QString description() const override {
        return QStringLiteral("Add the clip to your Immich library and share a link to it.");
    }

    QList<Field> fields() const override {
        QList<Field> fields;
        Field f;

        f = {};
        f.key = QStringLiteral("serverUrl");
        f.label = QStringLiteral("Server");
        f.placeholder = QStringLiteral("https://photos.example.com");
        f.required = true;
        fields << f;

        f = {};
        f.key = QStringLiteral("apiKey");
        f.label = QStringLiteral("API key");
        f.type = Field::Secret;
        f.required = true;
        f.help = QStringLiteral("Create one under Account Settings › API Keys, with asset upload "
                                "and shared link permissions.");
        fields << f;

        f = {};
        f.key = QStringLiteral("shareLink");
        f.label = QStringLiteral("Create a public share link");
        f.type = Field::Toggle;
        f.defaultValue = true;
        f.help = QStringLiteral("Otherwise the link opens the clip in Immich, for signed-in users.");
        fields << f;

        f = {};
        f.key = QStringLiteral("expireDays");
        f.label = QStringLiteral("Share links expire after (days, 0 = never)");
        f.type = Field::Number;
        f.defaultValue = 0;
        fields << f;

        f = {};
        f.key = QStringLiteral("publicUrl");
        f.label = QStringLiteral("Public address");
        f.placeholder = QStringLiteral("https://share.example.com");
        f.help = QStringLiteral("For share links, when Immich is reached differently from outside.");
        fields << f;

        return fields;
    }

    Job *createJob(const QVariantMap &settings, const Services &services,
                   QObject *parent) const override {
        return new ImmichJob(settings, services, parent);
    }
};

}

const Provider *immichProvider() {
    static const ImmichProvider provider;
    return &provider;
}

}

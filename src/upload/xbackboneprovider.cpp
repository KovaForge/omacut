#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include "httpjob.h"
#include "providers.h"

namespace upload {

namespace {

const QString kApiV1 = QStringLiteral("API v1 (/api/v1/upload)");

class XBackBoneJob : public HttpJob {
public:
    XBackBoneJob(const QVariantMap &settings, const Services &services, QObject *parent)
        : HttpJob(settings.value(QStringLiteral("name")).toString(), services, parent),
          m_server(normalizeServerUrl(settings.value(QStringLiteral("serverUrl")).toString())),
          m_token(settings.value(QStringLiteral("token")).toString()),
          m_apiV1(settings.value(QStringLiteral("api")).toString() == kApiV1) {}

    void start(const QString &filePath) override {
        QIODevice *file = openFile(filePath);
        if (!file)
            return;
        const QString fileName = QFileInfo(filePath).fileName();

        // XBackBone 3 takes the token as a form field next to "upload"; the
        // newer API wants a bearer token and "file".
        QNetworkRequest request = newRequest(
            QUrl(m_server + (m_apiV1 ? QStringLiteral("/api/v1/upload") : QStringLiteral("/upload"))));
        request.setRawHeader("Accept", "application/json");
        QHttpMultiPart *body = nullptr;
        if (m_apiV1) {
            request.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
            body = formData({}, QStringLiteral("file"), fileName, file, mimeTypeFor(filePath));
        } else {
            body = formData({{QStringLiteral("token"), m_token}}, QStringLiteral("upload"), fileName,
                            file, mimeTypeFor(filePath));
        }

        send(request, "POST", body, [this](const Reply &reply) {
            const QJsonObject payload = QJsonDocument::fromJson(reply.body).object();
            const QJsonObject data = payload.value(QStringLiteral("data")).toObject();
            const QString url = m_apiV1 ? data.value(QStringLiteral("preview_ext_url")).toString()
                                        : payload.value(QStringLiteral("url")).toString();
            if (!reply.ok() || !isWebLink(url)) {
                fail(requestError(reply, payload.value(QStringLiteral("message")).toString()));
                return;
            }
            const QJsonObject links = m_apiV1 ? data : payload;
            succeed({url, links.value(QStringLiteral("raw_url")).toString(),
                     links.value(QStringLiteral("deletion_url")).toString()});
        }, [this](qint64 sent, qint64 total) { emit progress(sent, total); });
    }

private:
    const QString m_server;
    const QString m_token;
    const bool m_apiV1;
};

class XBackBoneProvider : public Provider {
public:
    QString id() const override { return QStringLiteral("xbackbone"); }
    QString name() const override { return QStringLiteral("XBackBone"); }
    QString description() const override {
        return QStringLiteral("Your self-hosted XBackBone file manager.");
    }

    QList<Field> fields() const override {
        QList<Field> fields;
        Field f;

        f = {};
        f.key = QStringLiteral("serverUrl");
        f.label = QStringLiteral("Server");
        f.placeholder = QStringLiteral("https://x.example.com");
        f.required = true;
        fields << f;

        f = {};
        f.key = QStringLiteral("token");
        f.label = QStringLiteral("Upload token");
        f.type = Field::Secret;
        f.required = true;
        f.help = QStringLiteral("Shown on your XBackBone profile page.");
        fields << f;

        f = {};
        f.key = QStringLiteral("api");
        f.label = QStringLiteral("API");
        f.type = Field::Choice;
        f.choices = {QStringLiteral("XBackBone 3 (/upload)"), kApiV1};
        f.defaultValue = QStringLiteral("XBackBone 3 (/upload)");
        fields << f;

        return fields;
    }

    Job *createJob(const QVariantMap &settings, const Services &services,
                   QObject *parent) const override {
        return new XBackBoneJob(settings, services, parent);
    }
};

}

const Provider *xbackboneProvider() {
    static const XBackBoneProvider provider;
    return &provider;
}

}

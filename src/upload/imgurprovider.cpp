#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include "httpjob.h"
#include "providers.h"

namespace upload {

namespace {

const QString kImgurApi = QStringLiteral("https://api.imgur.com");

class ImgurJob : public HttpJob {
public:
    ImgurJob(const QVariantMap &settings, const Services &services, QObject *parent)
        : HttpJob(settings.value(QStringLiteral("name")).toString(), services, parent),
          m_api(settings.value(QStringLiteral("apiUrl"), kImgurApi).toString()),
          m_clientId(settings.value(QStringLiteral("clientId")).toString().trimmed()) {}

    void start(const QString &filePath) override {
        QIODevice *file = openFile(filePath);
        if (!file)
            return;
        const QString mimeType = mimeTypeFor(filePath);
        // Imgur takes videos in "video" and pictures in "image".
        const QString field = mimeType.startsWith(QStringLiteral("video/")) ? QStringLiteral("video")
                                                                             : QStringLiteral("image");

        QNetworkRequest request = newRequest(QUrl(m_api + QStringLiteral("/3/upload")));
        request.setRawHeader("Authorization", "Client-ID " + m_clientId.toUtf8());
        QHttpMultiPart *body = formData({{QStringLiteral("type"), QStringLiteral("file")}}, field,
                                        QFileInfo(filePath).fileName(), file, mimeType);
        send(request, "POST", body, [this](const Reply &reply) {
            const QJsonObject data = QJsonDocument::fromJson(reply.body).object()
                                         .value(QStringLiteral("data")).toObject();
            const QString link = data.value(QStringLiteral("link")).toString();
            if (!reply.ok() || !isWebLink(link)) {
                // Imgur puts its reason in data.error, as a string or an object.
                const QJsonValue error = data.value(QStringLiteral("error"));
                fail(requestError(reply, error.isObject()
                                             ? error.toObject().value(QStringLiteral("message")).toString()
                                             : error.toString()));
                return;
            }
            const QString deleteHash = data.value(QStringLiteral("deletehash")).toString();
            succeed({link, {},
                     deleteHash.isEmpty() ? QString() : QStringLiteral("https://imgur.com/delete/") + deleteHash});
        }, [this](qint64 sent, qint64 total) { emit progress(sent, total); });
    }

private:
    const QString m_api;
    const QString m_clientId;
};

class ImgurProvider : public Provider {
public:
    QString id() const override { return QStringLiteral("imgur"); }
    QString name() const override { return QStringLiteral("Imgur"); }
    QString description() const override {
        return QStringLiteral("Anonymous Imgur uploads (videos up to 200 MB and 60 seconds).");
    }

    QList<Field> fields() const override {
        Field clientId;
        clientId.key = QStringLiteral("clientId");
        clientId.label = QStringLiteral("Client ID");
        clientId.required = true;
        clientId.help = QStringLiteral("Register an application at api.imgur.com/oauth2/addclient "
                                       "(anonymous usage) to get one.");

        Field api;
        api.key = QStringLiteral("apiUrl");
        api.label = QStringLiteral("API");
        api.defaultValue = kImgurApi;
        api.hidden = true;
        return {clientId, api};
    }

    Job *createJob(const QVariantMap &settings, const Services &services,
                   QObject *parent) const override {
        return new ImgurJob(settings, services, parent);
    }
};

}

const Provider *imgurProvider() {
    static const ImgurProvider provider;
    return &provider;
}

}

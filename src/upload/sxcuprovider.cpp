#include <QFileInfo>
#include <QHttpMultiPart>
#include <QMimeDatabase>
#include <QUrlQuery>

#include "httpjob.h"
#include "providers.h"
#include "sxcu.h"

namespace upload {

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

QByteArray quoted(const QString &value) {
    QByteArray bytes = value.toUtf8();
    bytes.replace('\\', "\\\\").replace('"', "\\\"");
    return '"' + bytes + '"';
}

class SxcuJob : public HttpJob {
public:
    SxcuJob(const sxcu::Destination &destination, const Services &services, QObject *parent)
        : HttpJob(destination.name, services, parent), m_destination(destination) {}

    void start(const QString &filePath) override {
        QIODevice *file = openFile(filePath);
        if (!file)
            return;

        // Everything the request sends may use the syntax too, e.g. {filename}
        // in the URL or {random:…} to spread uploads over mirrors.
        const QString fileName = QFileInfo(filePath).fileName();
        const sxcu::Context context{nullptr, fileName};
        QUrl url(sxcu::expand(m_destination.requestUrl, context));
        if (!m_destination.parameters.isEmpty()) {
            QUrlQuery query(url);
            for (const auto &[key, value] : m_destination.parameters)
                query.addQueryItem(key, sxcu::expand(value, context));
            url.setQuery(query);
        }

        QNetworkRequest request = newRequest(url);
        for (const auto &[key, value] : m_destination.headers)
            request.setRawHeader(key.toUtf8(), sxcu::expand(value, context).toUtf8());

        const QString mimeType = QMimeDatabase().mimeTypeForFile(filePath).name();
        const QByteArray method = m_destination.requestMethod.toUtf8();
        const auto handler = [this, fileName](const Reply &reply) { handle(reply, fileName); };
        const auto onProgress = [this](qint64 sent, qint64 total) { emit progress(sent, total); };

        if (m_destination.body == QStringLiteral("Binary")) {
            if (!request.hasRawHeader("Content-Type"))
                request.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
            send(request, method, file, handler, onProgress);
            return;
        }

        auto *multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
        for (const auto &[key, value] : m_destination.arguments) {
            QHttpPart part;
            part.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QByteArray("form-data; name=") + quoted(key));
            part.setBody(sxcu::expand(value, context).toUtf8());
            multipart->append(part);
        }
        QHttpPart filePart;
        filePart.setHeader(QNetworkRequest::ContentDispositionHeader,
                           QByteArray("form-data; name=") + quoted(m_destination.fileFormName)
                               + "; filename=" + quoted(fileName));
        filePart.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
        filePart.setBodyDevice(file);
        file->setParent(multipart);
        multipart->append(filePart);
        send(request, method, multipart, handler, onProgress);
    }

private:
    void handle(const Reply &reply, const QString &fileName) {
        if (reply.status == 0) {
            fail(reply.networkError);
            return;
        }

        sxcu::Response response;
        response.status = reply.status;
        response.body = reply.body;
        response.url = reply.url;
        response.headers = reply.headers;
        const sxcu::Context context{&response, fileName};

        if (!reply.ok()) {
            fail(httpError(reply, sxcu::expand(m_destination.errorMessage, context).trimmed()));
            return;
        }

        const QString url = m_destination.url.isEmpty()
            ? QString::fromUtf8(reply.body).trimmed()
            : sxcu::expand(m_destination.url, context).trimmed();
        if (!isWebLink(url)) {
            const QString detail = snippet(reply.body);
            fail(QStringLiteral("%1 didn't return a link%2")
                     .arg(m_hostName,
                          detail.isEmpty() ? QString() : QStringLiteral(": ") + detail));
            return;
        }

        succeed({url, sxcu::expand(m_destination.thumbnailUrl, context).trimmed(),
                 sxcu::expand(m_destination.deletionUrl, context).trimmed()});
    }

    const sxcu::Destination m_destination;
};

class SxcuProvider : public Provider {
public:
    QString id() const override { return QStringLiteral("sxcu"); }
    QString name() const override { return QStringLiteral("Custom uploader (.sxcu)"); }
    QString description() const override {
        return QStringLiteral("Any host with a ShareX custom uploader config.");
    }

    QList<Field> fields() const override {
        Field definition;
        definition.key = QStringLiteral("definition");
        definition.label = QStringLiteral("Uploader config");
        definition.type = Field::Multiline;
        definition.required = true;
        definition.placeholder = QStringLiteral(R"({ "RequestURL": "https://…", "FileFormName": "file" })");
        definition.help = QStringLiteral("Paste the contents of a .sxcu file. Files dropped in "
                                         "~/.config/omacut/uploaders are picked up as hosts too.");
        return {definition};
    }

    QString validate(const QVariantMap &settings) const override {
        const QString missing = Provider::validate(settings);
        if (!missing.isEmpty())
            return missing;
        QString error;
        if (!sxcu::fromJson(settings.value(QStringLiteral("definition")).toString().toUtf8(),
                            nullptr, &error))
            return QStringLiteral("The uploader config can't be used: %1.").arg(error);
        return {};
    }

    Job *createJob(const QVariantMap &settings, const Services &services,
                   QObject *parent) const override {
        sxcu::Destination destination;
        sxcu::fromJson(settings.value(QStringLiteral("definition")).toString().toUtf8(),
                       &destination);
        // The host's own name wins over the config's, as it's what the user sees.
        const QString name = settings.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            destination.name = name;
        return new SxcuJob(destination, services, parent);
    }
};

}

const Provider *sxcuProvider() {
    static const SxcuProvider provider;
    return &provider;
}

QList<QByteArray> builtInDefinitions() {
    QList<QByteArray> definitions;
    for (const char *json : kBuiltIns)
        definitions << QByteArray(json);
    return definitions;
}

}

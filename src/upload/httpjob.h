#pragma once

#include <QByteArray>
#include <QHash>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>

#include <functional>

#include "provider.h"

class QHttpMultiPart;
class QIODevice;
class QNetworkReply;

namespace upload {

// What came back from one request.
struct Reply {
    int status = 0;
    QByteArray body;
    QUrl url;
    // Keyed by lower-cased header name.
    QHash<QByteArray, QByteArray> headers;
    // Set when there was no HTTP answer at all (DNS, TLS, refused, stalled).
    QString networkError;

    bool ok() const { return status >= 200 && status < 300; }
    QByteArray header(const QByteArray &name) const { return headers.value(name.toLower()); }
};

// A single line of a response body, for error messages.
QString snippet(const QByteArray &body, int length = 200);
bool isWebLink(const QString &text);

// A job made of HTTP requests run one after another. Subclasses chain them
// with send(), and end with succeed() or fail(); cancelling, stalls and the
// one-outcome rule are handled here.
class HttpJob : public Job {
    Q_OBJECT

public:
    HttpJob(const QString &hostName, const Services &services, QObject *parent);
    ~HttpJob() override;

    void cancel() override;

protected:
    using Handler = std::function<void(const Reply &)>;
    using Progress = std::function<void(qint64 sent, qint64 total)>;

    // Sends one request. body may be a QByteArray, a QIODevice (taken over)
    // or a QHttpMultiPart (taken over); onProgress, when given, reports the
    // upload side of this request.
    void send(QNetworkRequest request, const QByteArray &method, const QByteArray &body,
              Handler handler, Progress onProgress = {});
    void send(QNetworkRequest request, const QByteArray &method, QIODevice *body,
              Handler handler, Progress onProgress = {});
    void send(QNetworkRequest request, const QByteArray &method, QHttpMultiPart *body,
              Handler handler, Progress onProgress = {});

    // "<host> answered HTTP 404: <why>", with why from the body if not given.
    QString httpError(const Reply &reply, const QString &why = QString()) const;
    // httpError, or the network error when there was no answer.
    QString requestError(const Reply &reply, const QString &why = QString()) const;

    void succeed(const Outcome &outcome);
    void fail(const QString &message);
    bool isDone() const { return m_done; }
    // Runs once when cancel() stops a job that's still going (e.g. to abort
    // a multipart upload on the server).
    virtual void onCancelled() {}

    QNetworkRequest newRequest(const QUrl &url) const;
    // Opens the file to upload; on failure it has already failed the job.
    QIODevice *openFile(const QString &path);

    const QString m_hostName;
    Services m_services;

private:
    void track(QNetworkReply *reply, Handler handler, Progress onProgress);

    QPointer<QNetworkReply> m_reply;
    bool m_done = false;
    bool m_cancelled = false;
};

}

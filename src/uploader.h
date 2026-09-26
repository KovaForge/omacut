#pragma once

#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "sxcu.h"

class QNetworkReply;

namespace uploads {

// The hosts omacut offers out of the box: anonymous, no account needed.
QList<sxcu::Destination> builtInDestinations();

// Where users drop their own .sxcu files: ~/.config/omacut/uploaders.
QString userDestinationsDir();

// Every .sxcu in dir, sorted by name. Files that can't be used are skipped
// and described in warnings.
QList<sxcu::Destination> loadDestinations(const QString &dir, QStringList *warnings = nullptr);

}

// Sends one file to one destination and reads the link back out of the
// response, following the destination's .sxcu templates.
class Uploader : public QObject {
    Q_OBJECT

public:
    explicit Uploader(QObject *parent = nullptr);
    ~Uploader() override;

    bool busy() const { return m_reply != nullptr; }

    // Starts the upload; exactly one of finished or failed follows.
    void upload(const sxcu::Destination &destination, const QString &filePath);
    void cancel();

signals:
    void progress(qint64 sent, qint64 total);
    void finished(const QString &url, const QString &thumbnailUrl, const QString &deletionUrl);
    void failed(const QString &message);

private:
    void handleReply(QNetworkReply *reply, const sxcu::Destination &destination,
                     const QString &fileName);

    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    bool m_cancelled = false;
};

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include "httpjob.h"
#include "providers.h"
#include "sigv4.h"

namespace upload {

namespace {

const QString kSftp = QStringLiteral("SFTP");
const QString kFtp = QStringLiteral("FTP");
const QString kExplicitTls = QStringLiteral("FTPS (explicit TLS)");
const QString kImplicitTls = QStringLiteral("FTPS (implicit TLS)");
// curl gives up when less than a byte a second moves for this long.
constexpr int kStallSeconds = 60;

// A value for curl's config file: quoted, with \ and " escaped.
QByteArray configValue(const QString &value) {
    QByteArray bytes = value.toUtf8();
    bytes.replace('\\', "\\\\").replace('"', "\\\"").replace('\n', "\\n");
    return '"' + bytes + '"';
}

// Uploads through curl, which speaks FTP, FTPS and SFTP; Qt doesn't.
class CurlJob : public Job {
public:
    CurlJob(const QVariantMap &settings, QObject *parent)
        : Job(parent), m_settings(settings),
          m_hostName(settings.value(QStringLiteral("name")).toString()) {}

    ~CurlJob() override {
        if (m_process) {
            m_process->disconnect(this);
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }

    void start(const QString &filePath) override {
        const QString curl = QStandardPaths::findExecutable(QStringLiteral("curl"));
        if (curl.isEmpty()) {
            done(QStringLiteral("`curl` was not found on your PATH; it's needed for FTP and SFTP."));
            return;
        }
        const QFileInfo file(filePath);
        if (!file.isReadable()) {
            done(QStringLiteral("Could not read %1.").arg(file.fileName()));
            return;
        }
        m_size = file.size();

        const QString fileName = m_settings.value(QStringLiteral("uniqueNames"), true).toBool()
            ? taggedFileName(file.fileName())
            : file.fileName();
        const QString protocol = m_settings.value(QStringLiteral("protocol")).toString();
        const QString host = m_settings.value(QStringLiteral("host")).toString().trimmed();
        const int port = m_settings.value(QStringLiteral("port")).toInt();
        QString directory = expandDateTokens(m_settings.value(QStringLiteral("directory")).toString(),
                                             QDateTime::currentDateTime()).trimmed();
        while (directory.endsWith(QLatin1Char('/')))
            directory.chop(1);
        if (!directory.startsWith(QLatin1Char('/')))
            directory.prepend(QLatin1Char('/'));
        const QString remotePath = (directory == QStringLiteral("/") ? QString() : directory)
            + QLatin1Char('/') + fileName;

        const QString scheme = protocol == kSftp ? QStringLiteral("sftp")
            : protocol == kImplicitTls           ? QStringLiteral("ftps")
                                                 : QStringLiteral("ftp");
        // SFTP paths are absolute from the root; "/~/" would mean home.
        const QString url = scheme + QStringLiteral("://") + host
            + (port > 0 ? QLatin1Char(':') + QString::number(port) : QString())
            + QString::fromLatin1(sigv4::uriEncode(remotePath, false));

        // Everything, the password included, goes in over stdin so it
        // never appears in the process list.
        QByteArray config;
        config += "url = " + configValue(url) + '\n';
        config += "upload-file = " + configValue(filePath) + '\n';
        config += "user = " + configValue(m_settings.value(QStringLiteral("username")).toString()
                                          + QLatin1Char(':')
                                          + m_settings.value(QStringLiteral("password")).toString())
            + '\n';
        config += "ftp-create-dirs\nshow-error\nprogress-bar\n";
        config += "speed-limit = 1\nspeed-time = " + QByteArray::number(kStallSeconds) + '\n';
        if (protocol == kExplicitTls)
            config += "ssl-reqd\n";
        const QString key = m_settings.value(QStringLiteral("privateKey")).toString().trimmed();
        if (protocol == kSftp && !key.isEmpty())
            config += "key = " + configValue(QDir::fromNativeSeparators(expandHome(key))) + '\n';

        QString publicUrl = m_settings.value(QStringLiteral("publicUrl")).toString().trimmed();
        while (publicUrl.endsWith(QLatin1Char('/')))
            publicUrl.chop(1);
        m_link = publicUrl.isEmpty()
            ? url
            : publicUrl + QLatin1Char('/') + QString::fromLatin1(sigv4::uriEncode(fileName, true));

        m_process = new QProcess(this);
        connect(m_process, &QProcess::readyReadStandardError, this, [this] { readProgress(); });
        connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            readProgress();
            const QString error = curlError();
            m_process->deleteLater();
            m_process = nullptr;
            if (m_cancelled)
                done(QStringLiteral("Upload cancelled."));
            else if (status != QProcess::NormalExit || code != 0)
                done(QStringLiteral("%1: %2").arg(m_hostName,
                                                  error.isEmpty() ? QStringLiteral("curl failed (%1)").arg(code)
                                                                  : error));
            else
                done({}, Outcome{m_link, {}, {}});
        });
        connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart)
                return;
            const QString message = m_process->errorString();
            m_process->deleteLater();
            m_process = nullptr;
            done(QStringLiteral("Could not start curl: %1").arg(message));
        });
        m_process->start(curl, {QStringLiteral("--config"), QStringLiteral("-")});
        m_process->write(config);
        m_process->closeWriteChannel();
    }

    void cancel() override {
        if (m_finished || m_cancelled)
            return;
        m_cancelled = true;
        if (m_process)
            m_process->kill();
        else
            done(QStringLiteral("Upload cancelled."));
    }

private:
    static QString expandHome(const QString &path) {
        return path.startsWith(QStringLiteral("~/")) ? QDir::homePath() + path.mid(1) : path;
    }

    // --progress-bar redraws "#####   42.5%" with carriage returns.
    void readProgress() {
        if (!m_process)
            return;
        m_stderr += m_process->readAllStandardError();
        static const QRegularExpression percent(QStringLiteral("(\\d+(?:\\.\\d+)?)%"));
        QRegularExpressionMatchIterator it = percent.globalMatch(QString::fromUtf8(m_stderr.right(64)));
        double last = -1;
        while (it.hasNext())
            last = it.next().captured(1).toDouble();
        if (last >= 0 && m_size > 0)
            emit progress(qint64(m_size * qMin(last, 100.0) / 100.0), m_size);
    }

    // curl's own "curl: (67) Access denied: 530" line, without the prefix.
    QString curlError() const {
        const QStringList lines = QString::fromUtf8(m_stderr)
                                      .split(QRegularExpression(QStringLiteral("[\\r\\n]")),
                                             Qt::SkipEmptyParts);
        for (qsizetype i = lines.size() - 1; i >= 0; --i) {
            const QString line = lines.at(i).trimmed();
            if (line.startsWith(QStringLiteral("curl: ")))
                return line.mid(6).remove(QRegularExpression(QStringLiteral("^\\(\\d+\\) ")));
        }
        return {};
    }

    void done(const QString &error, const Outcome &outcome = {}) {
        if (m_finished)
            return;
        m_finished = true;
        if (error.isEmpty())
            emit finished(outcome);
        else
            emit failed(error);
    }

    const QVariantMap m_settings;
    const QString m_hostName;
    QProcess *m_process = nullptr;
    QByteArray m_stderr;
    QString m_link;
    qint64 m_size = 0;
    bool m_cancelled = false;
    bool m_finished = false;
};

class FtpProvider : public Provider {
public:
    QString id() const override { return QStringLiteral("ftp"); }
    QString name() const override { return QStringLiteral("FTP / SFTP"); }
    QString description() const override {
        return QStringLiteral("Your own server over SFTP, FTP or FTPS (uses curl).");
    }

    QList<Field> fields() const override {
        QList<Field> fields;
        Field f;

        f = {};
        f.key = QStringLiteral("protocol");
        f.label = QStringLiteral("Protocol");
        f.type = Field::Choice;
        f.choices = {kSftp, kExplicitTls, kImplicitTls, kFtp};
        f.defaultValue = kSftp;
        fields << f;

        f = {};
        f.key = QStringLiteral("host");
        f.label = QStringLiteral("Server");
        f.placeholder = QStringLiteral("example.com");
        f.required = true;
        f.help = QStringLiteral("For SFTP the server must already be in ~/.ssh/known_hosts.");
        fields << f;

        f = {};
        f.key = QStringLiteral("port");
        f.label = QStringLiteral("Port (0 = the protocol's default)");
        f.type = Field::Number;
        f.defaultValue = 0;
        fields << f;

        f = {};
        f.key = QStringLiteral("username");
        f.label = QStringLiteral("User name");
        f.required = true;
        fields << f;

        f = {};
        f.key = QStringLiteral("password");
        f.label = QStringLiteral("Password");
        f.type = Field::Secret;
        f.help = QStringLiteral("Leave empty for SFTP with a key.");
        fields << f;

        f = {};
        f.key = QStringLiteral("privateKey");
        f.label = QStringLiteral("SFTP private key");
        f.placeholder = QStringLiteral("~/.ssh/id_ed25519");
        fields << f;

        f = {};
        f.key = QStringLiteral("directory");
        f.label = QStringLiteral("Folder");
        f.defaultValue = QStringLiteral("/omacut");
        f.help = QStringLiteral("Created when missing. %y, %mo and %d become the date.");
        fields << f;

        f = {};
        f.key = QStringLiteral("publicUrl");
        f.label = QStringLiteral("Web address of the folder");
        f.placeholder = QStringLiteral("https://example.com/clips");
        f.help = QStringLiteral("Links point here; without it they're the server's own address.");
        fields << f;

        f = {};
        f.key = QStringLiteral("uniqueNames");
        f.label = QStringLiteral("Add a random tag to file names");
        f.type = Field::Toggle;
        f.defaultValue = true;
        fields << f;

        return fields;
    }

    Job *createJob(const QVariantMap &settings, const Services &,
                   QObject *parent) const override {
        return new CurlJob(settings, parent);
    }
};

}

const Provider *ftpProvider() {
    static const FtpProvider provider;
    return &provider;
}

}

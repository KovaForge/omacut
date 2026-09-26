#include <QPointer>

#include "providers.h"
#include "provider.h"

namespace upload {

namespace {

// Tries each chosen host in turn until one takes the upload.
class AutoJob : public Job {
public:
    AutoJob(const QStringList &hostIds, const Services &services, QObject *parent)
        : Job(parent), m_hostIds(hostIds), m_services(services) {}

    void start(const QString &filePath) override {
        m_filePath = filePath;
        next();
    }

    void cancel() override {
        m_cancelled = true;
        if (m_current)
            m_current->cancel();
    }

private:
    // Each reason as a sentence, whatever punctuation it came with.
    void noteFailure(const QString &message) {
        m_errors << (message.endsWith(QLatin1Char('.')) ? message : message + QLatin1Char('.'));
    }

    void next() {
        if (m_cancelled) {
            emit failed(QStringLiteral("Upload cancelled."));
            return;
        }
        if (m_index >= m_hostIds.size()) {
            emit failed(m_errors.isEmpty() ? QStringLiteral("No hosts are chosen to try.")
                                           : QStringLiteral("Every host failed. %1")
                                                 .arg(m_errors.join(QStringLiteral(" "))));
            return;
        }

        QString error;
        Job *job = m_services.createJob(m_hostIds.at(m_index++), this, &error);
        if (!job) {
            noteFailure(error);
            next();
            return;
        }
        m_current = job;
        connect(job, &Job::progress, this, &Job::progress);
        connect(job, &Job::finished, this, [this, job](const Outcome &outcome) {
            job->deleteLater();
            m_current = nullptr;
            emit finished(outcome);
        });
        connect(job, &Job::failed, this, [this, job](const QString &message) {
            job->deleteLater();
            m_current = nullptr;
            noteFailure(message);
            next();
        });
        job->start(m_filePath);
    }

    const QStringList m_hostIds;
    Services m_services;
    QString m_filePath;
    qsizetype m_index = 0;
    QPointer<Job> m_current;
    QStringList m_errors;
    bool m_cancelled = false;
};

class AutoProvider : public Provider {
public:
    QString id() const override { return autoProviderId(); }
    QString name() const override { return QStringLiteral("Auto (try several)"); }
    QString description() const override {
        return QStringLiteral("Tries your hosts in order and uses the first one that works.");
    }

    QList<Field> fields() const override {
        Field hosts;
        hosts.key = QStringLiteral("hosts");
        hosts.label = QStringLiteral("Hosts to try, in order");
        hosts.type = Field::Hosts;
        hosts.required = true;
        return {hosts};
    }

    Job *createJob(const QVariantMap &settings, const Services &services,
                   QObject *parent) const override {
        return new AutoJob(settings.value(QStringLiteral("hosts")).toStringList(), services, parent);
    }
};

}

QString autoProviderId() {
    return QStringLiteral("auto");
}

const Provider *autoProvider() {
    static const AutoProvider provider;
    return &provider;
}

}

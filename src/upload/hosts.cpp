#include "hosts.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

#include "providers.h"
#include "sxcu.h"

namespace upload {

namespace {

const QString kSxcu = QStringLiteral("sxcu");
const QString kDefinition = QStringLiteral("definition");

const Field *findField(const QList<Field> &fields, const QString &key) {
    for (const Field &field : fields) {
        if (field.key == key)
            return &field;
    }
    return nullptr;
}

}

Hosts::Hosts(const QString &configDir, std::unique_ptr<SecretStore> secrets, QObject *parent)
    : QObject(parent), m_configDir(configDir), m_secrets(std::move(secrets)),
      m_openUrl([](const QUrl &url) { return QDesktopServices::openUrl(url); }) {
    reload();
}

Hosts::~Hosts() {
    if (m_authorization) {
        m_authorization->disconnect(this);
        m_authorization->cancel();
    }
}

QString Hosts::uploadersDir() const {
    return QDir(m_configDir).filePath(QStringLiteral("uploaders"));
}

QString Hosts::secretKey(const QString &hostId, const QString &field) const {
    return hostId + QLatin1Char('/') + field;
}

void Hosts::reload() {
    QList<Host> hosts;

    for (const QByteArray &definition : builtInDefinitions()) {
        sxcu::Destination destination;
        if (!sxcu::fromJson(definition, &destination))
            continue;
        const QString slug = destination.name.section(QLatin1Char(' '), 0, 0).toLower();
        hosts << Host{QStringLiteral("builtin:") + slug, kSxcu, destination.name,
                      {{kDefinition, QString::fromUtf8(definition)}}, true};
    }

    QFile file(QDir(m_configDir).filePath(QStringLiteral("hosts.json")));
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonArray saved = QJsonDocument::fromJson(file.readAll())
                                     .object()
                                     .value(QStringLiteral("hosts"))
                                     .toArray();
        for (const QJsonValue &value : saved) {
            const QJsonObject object = value.toObject();
            Host host{object.value(QStringLiteral("id")).toString(),
                      object.value(QStringLiteral("provider")).toString(),
                      object.value(QStringLiteral("name")).toString(),
                      object.value(QStringLiteral("settings")).toObject().toVariantMap(),
                      false};
            if (!host.id.isEmpty() && findProvider(host.provider))
                hosts << host;
        }
    }

    // .sxcu files dropped in the uploaders folder, as before hosts.json.
    const QFileInfoList files = QDir(uploadersDir()).entryInfoList(
        {QStringLiteral("*.sxcu")}, QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &info : files) {
        QFile sxcuFile(info.filePath());
        if (!sxcuFile.open(QIODevice::ReadOnly))
            continue;
        const QByteArray definition = sxcuFile.readAll();
        sxcu::Destination destination;
        QString error;
        if (!sxcu::fromJson(definition, &destination, &error)) {
            qWarning("omacut: skipping uploader %s: %s", qPrintable(info.fileName()),
                     qPrintable(error));
            continue;
        }
        hosts << Host{QStringLiteral("file:") + info.fileName(), kSxcu,
                      destination.name.isEmpty() ? info.completeBaseName() : destination.name,
                      {{kDefinition, QString::fromUtf8(definition)}}, true};
    }

    m_hosts = hosts;
    emit changed();
}

const Host *Hosts::find(const QString &id) const {
    for (const Host &host : m_hosts) {
        if (host.id == id)
            return &host;
    }
    return nullptr;
}

QVariantList Hosts::list() const {
    QVariantList list;
    for (const Host &host : m_hosts) {
        const Provider *provider = findProvider(host.provider);
        list << QVariantMap{
            {QStringLiteral("id"), host.id},
            {QStringLiteral("name"), host.name},
            {QStringLiteral("provider"), host.provider},
            {QStringLiteral("providerName"), provider ? provider->name() : host.provider},
            {QStringLiteral("readOnly"), host.readOnly},
        };
    }
    return list;
}

QVariantList Hosts::providerList() const {
    QVariantList list;
    for (const Provider *provider : providers()) {
        list << QVariantMap{
            {QStringLiteral("id"), provider->id()},
            {QStringLiteral("name"), provider->name()},
            {QStringLiteral("description"), provider->description()},
            {QStringLiteral("authorizeLabel"), provider->authorizeLabel()},
        };
    }
    return list;
}

QVariantList Hosts::fields(const QString &providerId) const {
    QVariantList list;
    if (const Provider *provider = findProvider(providerId)) {
        for (const Field &field : provider->fields()) {
            if (!field.hidden)
                list << field.toVariant();
        }
    }
    return list;
}

QVariantMap Hosts::values(const QString &id) const {
    const Host *host = find(id);
    if (!host)
        return {};
    const Provider *provider = findProvider(host->provider);
    if (!provider)
        return {};

    QVariantMap values = provider->withDefaults(host->settings);
    QStringList secretsSet;
    for (const Field &field : provider->fields()) {
        if (field.type == Field::Secret && !m_secrets->read(secretKey(id, field.key)).isEmpty())
            secretsSet << field.key;
    }
    values.insert(QStringLiteral("secretsSet"), secretsSet);
    return values;
}

QVariantMap Hosts::resolvedSettings(const Host &host) const {
    const Provider *provider = findProvider(host.provider);
    QVariantMap settings = provider ? provider->withDefaults(host.settings) : host.settings;
    if (provider) {
        for (const Field &field : provider->fields()) {
            if (field.type == Field::Secret)
                settings.insert(field.key, m_secrets->read(secretKey(host.id, field.key)));
        }
    }
    settings.insert(QStringLiteral("name"), host.name);
    return settings;
}

QString Hosts::save(const QString &id, const QString &providerId, const QString &name,
                    const QVariantMap &values) {
    m_lastError.clear();
    const Provider *provider = findProvider(providerId);
    if (!provider) {
        m_lastError = QStringLiteral("Unknown kind of host.");
        return {};
    }
    const QString trimmedName = name.trimmed();
    if (trimmedName.isEmpty()) {
        m_lastError = QStringLiteral("Give the host a name.");
        return {};
    }

    const Host *existing = id.isEmpty() ? nullptr : find(id);
    if (!id.isEmpty() && (!existing || existing->readOnly || existing->provider != providerId)) {
        m_lastError = QStringLiteral("This host can't be edited.");
        return {};
    }
    const QString hostId = existing ? id : QUuid::createUuid().toString(QUuid::WithoutBraces);

    // Only the provider's own fields are kept; blank secrets keep what's
    // stored. A sign-in done in this form fills in what it returned.
    Host host{hostId, providerId, trimmedName, existing ? existing->settings : QVariantMap(), false};
    QVariantMap secrets;
    QVariantMap check = existing ? resolvedSettings(*existing) : QVariantMap();
    const QVariantMap pending = m_pendingProvider == providerId ? m_pending : QVariantMap();
    for (const Field &field : provider->fields()) {
        const bool fromSignIn = pending.contains(field.key)
            && (field.hidden || field.type == Field::Secret
                || values.value(field.key).toString().isEmpty());
        if (!fromSignIn && (field.hidden || !values.contains(field.key)))
            continue;
        const QVariant value = fromSignIn ? pending.value(field.key) : values.value(field.key);
        if (field.type == Field::Secret) {
            if (!value.toString().isEmpty()) {
                secrets.insert(field.key, value);
                check.insert(field.key, value);
            }
            continue;
        }
        host.settings.insert(field.key, value);
        check.insert(field.key, value);
    }

    const QString problem = provider->validate(provider->withDefaults(check));
    if (!problem.isEmpty()) {
        m_lastError = problem;
        return {};
    }

    for (auto it = secrets.begin(); it != secrets.end(); ++it) {
        if (!m_secrets->write(secretKey(hostId, it.key()), it.value().toString())) {
            m_lastError = QStringLiteral("Could not store %1.").arg(it.key());
            return {};
        }
    }

    bool replaced = false;
    for (Host &h : m_hosts) {
        if (h.id == hostId) {
            h = host;
            replaced = true;
        }
    }
    if (!replaced) {
        // New hosts go after the others set up in the app, before .sxcu files.
        qsizetype at = m_hosts.size();
        while (at > 0 && m_hosts.at(at - 1).id.startsWith(QStringLiteral("file:")))
            --at;
        m_hosts.insert(at, host);
    }
    discardAuthorization();
    persist();
    emit changed();
    return hostId;
}

void Hosts::remove(const QString &id) {
    const Host *host = find(id);
    if (!host || host->readOnly)
        return;
    if (const Provider *provider = findProvider(host->provider)) {
        for (const Field &field : provider->fields()) {
            if (field.type == Field::Secret)
                m_secrets->remove(secretKey(id, field.key));
        }
    }
    m_hosts.removeIf([&id](const Host &h) { return h.id == id; });
    persist();
    emit changed();
}

void Hosts::storeChanges(const QString &id, const QVariantMap &changes) {
    Host *host = nullptr;
    for (Host &h : m_hosts) {
        if (h.id == id)
            host = &h;
    }
    if (!host || host->readOnly)
        return;
    const Provider *provider = findProvider(host->provider);
    if (!provider)
        return;
    const QList<Field> fields = provider->fields();
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        const Field *field = findField(fields, it.key());
        if (!field)
            continue;
        if (field->type == Field::Secret) {
            if (it.value().toString().isEmpty())
                m_secrets->remove(secretKey(id, it.key()));
            else
                m_secrets->write(secretKey(id, it.key()), it.value().toString());
        } else {
            host->settings.insert(it.key(), it.value());
        }
    }
    persist();
    emit changed();
}

void Hosts::persist() {
    QJsonArray saved;
    for (const Host &host : m_hosts) {
        if (host.readOnly)
            continue;
        saved.append(QJsonObject{
            {QStringLiteral("id"), host.id},
            {QStringLiteral("provider"), host.provider},
            {QStringLiteral("name"), host.name},
            {QStringLiteral("settings"), QJsonObject::fromVariantMap(host.settings)},
        });
    }

    QDir().mkpath(m_configDir);
    QSaveFile file(QDir(m_configDir).filePath(QStringLiteral("hosts.json")));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(QJsonObject{{QStringLiteral("hosts"), saved}}).toJson());
    file.commit();
}

Services Hosts::servicesFor(const QString &id) {
    Services services;
    services.network = m_network;
    services.saveSettings = [this, id](const QVariantMap &changes) { storeChanges(id, changes); };
    services.createJob = [this](const QString &hostId, QObject *parent, QString *error) -> Job * {
        // Auto hosts only chain to real destinations, so they can't loop.
        const Host *target = find(hostId);
        Job *job = nullptr;
        if (target && target->provider == autoProviderId())
            m_lastError = QStringLiteral("An Auto host can't use another Auto host.");
        else
            job = createJob(hostId, parent);
        if (!job && error)
            *error = m_lastError;
        return job;
    };
    services.openUrl = m_openUrl;
    return services;
}

Job *Hosts::createJob(const QString &id, QObject *parent) {
    m_lastError.clear();
    const Host *host = find(id);
    const Provider *provider = host ? findProvider(host->provider) : nullptr;
    if (!provider) {
        m_lastError = QStringLiteral("That host no longer exists.");
        return nullptr;
    }
    const QVariantMap settings = resolvedSettings(*host);
    const QString problem = provider->validate(settings);
    if (!problem.isEmpty()) {
        m_lastError = QStringLiteral("%1 isn't set up: %2").arg(host->name, problem);
        return nullptr;
    }
    return provider->createJob(settings, servicesFor(id), parent);
}

void Hosts::authorize(const QString &id, const QString &providerId, const QVariantMap &values) {
    if (m_authorization)
        return;
    const Provider *provider = findProvider(providerId);
    const Host *host = id.isEmpty() ? nullptr : find(id);
    QVariantMap settings = host && host->provider == providerId ? resolvedSettings(*host)
                                                                 : QVariantMap();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!it.value().toString().isEmpty() || it.value().typeId() == QMetaType::Bool)
            settings.insert(it.key(), it.value());
    }
    Authorization *authorization =
        provider ? provider->authorize(provider->withDefaults(settings), servicesFor(id), this)
                 : nullptr;
    if (!authorization) {
        emit authorizeFinished(QStringLiteral("This host doesn't sign in."), {}, {});
        return;
    }

    m_authorization = authorization;
    const auto done = [this, authorization] {
        authorization->deleteLater();
        m_authorization = nullptr;
        emit authorizingChanged();
    };
    connect(authorization, &Authorization::status, this, &Hosts::authorizeStatus);
    connect(authorization, &Authorization::finished, this,
            [this, provider, done](const QVariantMap &changes, const QString &summary) {
                m_pendingProvider = provider->id();
                m_pending = changes;
                // The form shows what it can; secrets only as "stored".
                QVariantMap formValues;
                QStringList secretsSet;
                for (const Field &field : provider->fields()) {
                    if (!changes.contains(field.key) || field.hidden)
                        continue;
                    if (field.type == Field::Secret)
                        secretsSet << field.key;
                    else
                        formValues.insert(field.key, changes.value(field.key));
                }
                formValues.insert(QStringLiteral("secretsSet"), secretsSet);
                done();
                emit authorizeFinished(QString(), summary, formValues);
            });
    connect(authorization, &Authorization::failed, this, [this, done](const QString &error) {
        done();
        emit authorizeFinished(error, QString(), {});
    });
    emit authorizingChanged();
    authorization->start();
}

void Hosts::discardAuthorization() {
    m_pendingProvider.clear();
    m_pending.clear();
}

void Hosts::cancelAuthorize() {
    if (m_authorization)
        m_authorization->cancel();
}

}

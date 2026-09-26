#pragma once

#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

#include "provider.h"
#include "secretstore.h"

namespace upload {

// A configured destination: a provider plus its settings.
struct Host {
    QString id;
    QString provider;
    QString name;
    // Everything but Secret fields, which live in the SecretStore.
    QVariantMap settings;
    // Built-ins and .sxcu files dropped in the uploaders folder can be used
    // but not edited here.
    bool readOnly = false;
};

// Every destination omacut can upload to: the built-ins, the .sxcu files in
// <configDir>/uploaders, and the hosts set up in the app, which are kept in
// <configDir>/hosts.json. The QML settings screen drives this directly.
class Hosts : public QObject {
    Q_OBJECT
    // [{id, name, provider, providerName, readOnly}] in display order.
    Q_PROPERTY(QVariantList list READ list NOTIFY changed)
    // [{id, name, description, authorizeLabel}] for "Add host".
    Q_PROPERTY(QVariantList providers READ providerList CONSTANT)
    Q_PROPERTY(bool authorizing READ authorizing NOTIFY authorizingChanged)

public:
    Hosts(const QString &configDir, std::unique_ptr<SecretStore> secrets,
          QObject *parent = nullptr);
    ~Hosts() override;

    const QList<Host> &all() const { return m_hosts; }
    const Host *find(const QString &id) const;
    QVariantList list() const;
    QVariantList providerList() const;
    bool authorizing() const { return m_authorization != nullptr; }
    QString uploadersDir() const;

    // Re-reads hosts.json and the .sxcu folder.
    Q_INVOKABLE void reload();

    Q_INVOKABLE QVariantList fields(const QString &providerId) const;
    // A host's settings for editing. Secrets are never handed out; instead
    // "secretsSet" lists the Secret fields that have something stored.
    Q_INVOKABLE QVariantMap values(const QString &id) const;
    // Creates (empty id) or updates a host. A blank Secret field keeps what's
    // stored. Returns the host's id, or empty with the reason in lastError.
    Q_INVOKABLE QString save(const QString &id, const QString &providerId, const QString &name,
                             const QVariantMap &values);
    Q_INVOKABLE QString lastError() const { return m_lastError; }
    Q_INVOKABLE void remove(const QString &id);

    // Runs the provider's browser sign-in for a saved host and stores what it
    // returns; ends with authorizeFinished.
    Q_INVOKABLE void authorize(const QString &id);
    Q_INVOKABLE void cancelAuthorize();

    // A job uploading to the host, or null with the reason in lastError.
    Job *createJob(const QString &id, QObject *parent);

    void setNetwork(QNetworkAccessManager *network) { m_network = network; }

signals:
    void changed();
    void authorizingChanged();
    void authorizeStatus(const QString &message);
    // error is empty on success; summary says what was connected.
    void authorizeFinished(const QString &id, const QString &error, const QString &summary);

private:
    Services servicesFor(const QString &id);
    QVariantMap resolvedSettings(const Host &host) const;
    void storeChanges(const QString &id, const QVariantMap &changes);
    void persist();
    QString secretKey(const QString &hostId, const QString &field) const;

    QString m_configDir;
    std::unique_ptr<SecretStore> m_secrets;
    QNetworkAccessManager m_ownNetwork;
    QNetworkAccessManager *m_network = &m_ownNetwork;
    QList<Host> m_hosts;
    QString m_lastError;
    QPointer<Authorization> m_authorization;
};

}

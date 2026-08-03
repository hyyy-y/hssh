#include "ConnectionManager.h"

#include "SshSession.h"

#include <QUuid>

namespace hssh {

class ConnectionManager::Impl {
public:
    QMap<QString, SshSession *> sessions;
};

ConnectionManager::ConnectionManager(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
}

ConnectionManager::~ConnectionManager()
{
    closeAll();
}

ConnectionManager &ConnectionManager::instance()
{
    static ConnectionManager manager;
    return manager;
}

QString ConnectionManager::createSession(const SessionConfig &config)
{
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto *session = new SshSession(this);
    session->setSessionConfig(config);

    connect(session, &SshSession::connected, this, [this, id]() {
        emit sessionConnected(id);
    });
    connect(session, &SshSession::disconnected, this, [this, id]() {
        emit sessionDisconnected(id);
    });
    connect(session, &SshSession::errorOccurred, this, [this, id](const QString &message) {
        emit sessionError(id, message);
    });

    d->sessions[id] = session;
    return id;
}

void ConnectionManager::closeSession(const QString &sessionId)
{
    auto it = d->sessions.find(sessionId);
    if (it == d->sessions.end()) {
        return;
    }

    SshSession *session = it.value();
    session->disconnect();
    session->deleteLater();
    d->sessions.erase(it);
}

void ConnectionManager::closeAll()
{
    for (auto it = d->sessions.begin(); it != d->sessions.end(); ++it) {
        SshSession *session = it.value();
        session->disconnect();
        session->deleteLater();
    }
    d->sessions.clear();
}

SshSession *ConnectionManager::session(const QString &sessionId) const
{
    return d->sessions.value(sessionId, nullptr);
}

QList<QString> ConnectionManager::sessionIds() const
{
    return d->sessions.keys();
}

SessionConfig ConnectionManager::config(const QString &sessionId) const
{
    SshSession *s = session(sessionId);
    if (!s) {
        return {};
    }
    return s->sessionConfig();
}

} // namespace hssh

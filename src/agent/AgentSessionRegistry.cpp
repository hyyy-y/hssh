#include "AgentSessionRegistry.h"

#include "agent/AgentSession.h"
#include "core/SessionRepository.h"
#include "core/SftpSession.h"

#include <QMap>
#include <QUuid>

namespace hssh {

class AgentSessionRegistry::Impl {
public:
    Impl()
        // Separate connection name: the GUI's repository already owns
        // "hssh_default"; reusing the name would rip it away.
        : repository(nullptr, QStringLiteral("hssh_agent"))
    {
    }

    QMap<QString, AgentSession *> sessions;
    QMap<QString, SftpSession *> sftpSessions; // lazily created, one per agent session
    QHash<QString, QString> pendingTransfers;  // session id -> request id
    SessionRepository repository;              // stored-session lookup (credentials stay in-process)
    QString lastError;
};

AgentSessionRegistry::AgentSessionRegistry(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
}

AgentSessionRegistry::~AgentSessionRegistry()
{
    closeAll();
}

QString AgentSessionRegistry::createSession(const SessionConfig &config)
{
    if (!config.isValid()) {
        d->lastError = tr("Invalid session configuration");
        return {};
    }

    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto *session = new AgentSession(id, config, this);

    connect(session, &AgentSession::connected, this, [this, id]() {
        emit connected(id);
    });
    connect(session, &AgentSession::connectFailed, this, [this, id](const QString &error) {
        emit connectFailed(id, error);
        closeSession(id);
    });
    connect(session, &AgentSession::execFinished, this, [this, id](const QString &requestId,
                                                                   const QString &output,
                                                                   int exitCode,
                                                                   const QString &error) {
        emit execFinished(id, requestId, output, exitCode, error);
    });
    connect(session, &AgentSession::closed, this, [this, id]() {
        emit sessionClosed(id);
    });

    d->sessions[id] = session;
    session->connectAsync();
    return id;
}

QString AgentSessionRegistry::createSessionFromStored(const QString &nameOrId)
{
    if (!d->repository.isInitialized() && !d->repository.initialize()) {
        d->lastError = tr("Session database unavailable: %1").arg(d->repository.lastError());
        return {};
    }

    const QList<SessionConfig> sessions = d->repository.loadAllSessions();
    // Locked credentials (master mode, app locked) come back as empty configs.
    bool anyLocked = false;
    for (const SessionConfig &config : sessions) {
        if (config.id().isEmpty()) {
            anyLocked = true;
            continue;
        }
        if (config.name() == nameOrId || config.displayName() == nameOrId
            || config.host() == nameOrId || config.id() == nameOrId) {
            return createSession(config);
        }
    }

    if (anyLocked) {
        d->lastError = tr("Stored credentials are locked; unlock the application first");
        return {};
    }
    d->lastError = tr("Stored session not found: %1").arg(nameOrId);
    return {};
}

bool AgentSessionRegistry::exec(const QString &sessionId, const QString &requestId, const QString &command)
{
    AgentSession *session = d->sessions.value(sessionId, nullptr);
    if (!session) {
        d->lastError = tr("Unknown session: %1").arg(sessionId);
        return false;
    }
    session->execAsync(requestId, command);
    return true;
}

bool AgentSessionRegistry::closeSession(const QString &sessionId)
{
    AgentSession *session = d->sessions.value(sessionId, nullptr);
    if (!session) {
        d->lastError = tr("Unknown session: %1").arg(sessionId);
        return false;
    }
    session->close();
    session->deleteLater();
    d->sessions.remove(sessionId);

    if (SftpSession *sftp = d->sftpSessions.take(sessionId)) {
        sftp->stop();
        sftp->deleteLater();
    }
    d->pendingTransfers.remove(sessionId);
    return true;
}

void AgentSessionRegistry::closeAll()
{
    for (SftpSession *sftp : d->sftpSessions) {
        sftp->stop();
        sftp->deleteLater();
    }
    d->sftpSessions.clear();
    d->pendingTransfers.clear();

    for (AgentSession *session : d->sessions) {
        session->close();
        session->deleteLater();
    }
    d->sessions.clear();
}

SftpSession *AgentSessionRegistry::ensureSftpSession(const QString &sessionId)
{
    if (!d->sessions.contains(sessionId)) {
        d->lastError = tr("Unknown session: %1").arg(sessionId);
        return nullptr;
    }
    if (auto it = d->sftpSessions.find(sessionId); it != d->sftpSessions.end()) {
        return it.value();
    }
    // SftpSession opens its own connection, so this only works for sessions
    // that allow password/key authentication; agent auth is retried with the
    // same config, which may fail on the second connection.
    auto *sftp = new SftpSession(d->sessions[sessionId]->config(), d->sessions[sessionId]);
    connect(sftp, &SftpSession::transferFinished, this,
            [this, sessionId](const QString &path, bool ok, const QString &message) {
                const QString requestId = d->pendingTransfers.take(sessionId);
                if (requestId.isEmpty()) {
                    return;
                }
                emit transferFinished(sessionId, requestId, path, ok, message);
            });
    connect(sftp, &SftpSession::errorOccurred, this,
            [this, sessionId](const QString &message) {
                const QString requestId = d->pendingTransfers.take(sessionId);
                if (requestId.isEmpty()) {
                    return;
                }
                emit transferFinished(sessionId, requestId, QString(), false, message);
            });
    sftp->start();
    d->sftpSessions[sessionId] = sftp;
    return sftp;
}

bool AgentSessionRegistry::upload(const QString &sessionId, const QString &requestId,
                                  const QString &localPath, const QString &remotePath)
{
    SftpSession *sftp = ensureSftpSession(sessionId);
    if (!sftp) {
        return false;
    }
    d->pendingTransfers[sessionId] = requestId;
    sftp->upload(localPath, remotePath);
    return true;
}

bool AgentSessionRegistry::download(const QString &sessionId, const QString &requestId,
                                    const QString &remotePath, const QString &localPath)
{
    SftpSession *sftp = ensureSftpSession(sessionId);
    if (!sftp) {
        return false;
    }
    d->pendingTransfers[sessionId] = requestId;
    sftp->download(remotePath, localPath);
    return true;
}

QVariantList AgentSessionRegistry::listSessions() const
{
    QVariantList result;
    for (auto it = d->sessions.cbegin(); it != d->sessions.cend(); ++it) {
        const AgentSession *session = it.value();
        QVariantMap entry;
        entry[QStringLiteral("id")] = it.key();
        entry[QStringLiteral("name")] = session->config().displayName();
        entry[QStringLiteral("host")] = session->config().host();
        entry[QStringLiteral("port")] = session->config().port();
        entry[QStringLiteral("username")] = session->config().username();
        entry[QStringLiteral("connected")] = session->isConnected();
        result.append(entry);
    }
    return result;
}

bool AgentSessionRegistry::hasSession(const QString &sessionId) const
{
    return d->sessions.contains(sessionId);
}

QString AgentSessionRegistry::lastError() const
{
    return d->lastError;
}

} // namespace hssh

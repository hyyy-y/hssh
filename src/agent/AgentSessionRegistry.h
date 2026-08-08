#ifndef HSSH_AGENT_AGENTSESSIONREGISTRY_H
#define HSSH_AGENT_AGENTSESSIONREGISTRY_H

#include "core/SessionConfig.h"

#include <QObject>
#include <QVariantList>
#include <QString>

namespace hssh {

class SftpSession;

// Owns the agent's SSH sessions (shared by the HTTP and MCP servers).
// Thread-safe API surface: everything is driven from the caller's thread and
// results arrive via signals.
class AgentSessionRegistry : public QObject {
    Q_OBJECT

public:
    explicit AgentSessionRegistry(QObject *parent = nullptr);
    ~AgentSessionRegistry() override;

    // Creates and starts connecting; the id is returned immediately and the
    // outcome arrives via connected()/connectFailed(). Empty id on error.
    QString createSession(const SessionConfig &config);
    // Creates a session from a stored session in the session database,
    // matched by name/host/id. Credentials never leave this process.
    // Empty id when not found or credentials are locked.
    QString createSessionFromStored(const QString &nameOrId);
    // Requests an exec; result via execFinished() with the same requestId.
    // Returns false when the session id is unknown.
    bool exec(const QString &sessionId, const QString &requestId, const QString &command);
    // File transfer over the session's SFTP channel (a second connection
    // owned by the registry). Result via transferFinished().
    bool upload(const QString &sessionId, const QString &requestId,
                const QString &localPath, const QString &remotePath);
    bool download(const QString &sessionId, const QString &requestId,
                  const QString &remotePath, const QString &localPath);
    bool closeSession(const QString &sessionId);
    void closeAll();

    [[nodiscard]] QVariantList listSessions() const; // [{id,name,host,port,connected}]
    [[nodiscard]] bool hasSession(const QString &sessionId) const;
    [[nodiscard]] QString lastError() const;

signals:
    void connected(const QString &sessionId);
    void connectFailed(const QString &sessionId, const QString &error);
    void execFinished(const QString &sessionId, const QString &requestId,
                      const QString &output, int exitCode, const QString &error);
    void transferFinished(const QString &sessionId, const QString &requestId,
                          const QString &path, bool ok, const QString &message);
    void sessionClosed(const QString &sessionId);

private:
    // Lazily creates the SFTP connection for an agent session (nullptr +
    // lastError when the session id is unknown).
    SftpSession *ensureSftpSession(const QString &sessionId);

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace hssh

#endif // HSSH_AGENT_AGENTSESSIONREGISTRY_H

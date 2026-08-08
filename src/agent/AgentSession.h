#ifndef HSSH_AGENT_AGENTSESSION_H
#define HSSH_AGENT_AGENTSESSION_H

#include "core/SessionConfig.h"

#include <QObject>
#include <QString>
#include <memory>

namespace hssh {

// One SSH connection owned by the agent (HTTP/MCP). Connect and exec run on
// dedicated worker threads so blocking network I/O never stalls the caller's
// event loop. Execs are serialized per session.
class AgentSession : public QObject {
    Q_OBJECT

public:
    AgentSession(const QString &id, const SessionConfig &config, QObject *parent = nullptr);
    ~AgentSession() override;

    [[nodiscard]] QString id() const;
    [[nodiscard]] SessionConfig config() const;
    [[nodiscard]] bool isConnected() const;

    void connectAsync();
    void execAsync(const QString &requestId, const QString &command);
    // Cancels a pending exec (unblocks the worker via a stop flag) and
    // releases the connection. Emits closed().
    void close();

signals:
    void connected(const QString &id);
    void connectFailed(const QString &id, const QString &error);
    void execFinished(const QString &requestId, const QString &output,
                      int exitCode, const QString &error);
    void closed(const QString &id);

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace hssh

#endif // HSSH_AGENT_AGENTSESSION_H

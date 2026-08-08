#ifndef HSSH_AGENT_AGENTMCPSERVER_H
#define HSSH_AGENT_AGENTMCPSERVER_H

#include <QHash>
#include <QObject>
#include <QString>

namespace hssh {

class AgentSessionRegistry;

// Model Context Protocol server over stdio (newline-delimited JSON-RPC 2.0).
// Start the application with --agent-mcp to run headless, or configure it in
// an MCP client as: { "command": "hssh", "args": ["--agent-mcp"] }.
// Tools: list_sessions, ssh_connect, ssh_exec, ssh_disconnect.
class AgentMcpServer : public QObject {
    Q_OBJECT

public:
    explicit AgentMcpServer(QObject *parent = nullptr);
    ~AgentMcpServer() override;

    [[nodiscard]] AgentSessionRegistry *registry() const;

private:
    void handleMessage(const QByteArray &line);
    void respond(const QJsonObject &request, const QJsonValue &result);
    void respondError(const QJsonObject &request, int code, const QString &message);
    void handleToolCall(const QString &name, const QJsonObject &arguments, const QJsonObject &request);
    void sendMessage(const QJsonObject &message);

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace hssh

#endif // HSSH_AGENT_AGENTMCPSERVER_H

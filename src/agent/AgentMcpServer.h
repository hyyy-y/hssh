#ifndef HSSH_AGENT_AGENTMCPSERVER_H
#define HSSH_AGENT_AGENTMCPSERVER_H

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <functional>

namespace hssh {

// Model Context Protocol server over stdio (newline-delimited JSON-RPC 2.0).
// Start the application with --agent-mcp: { "command": "hssh", "args":
// ["--agent-mcp"] }.
//
// The server is a thin BRIDGE to the GUI's local REST agent: it holds no SSH
// connections of its own. Every tool lands on a real, visible GUI terminal
// tab (the user watches the AI type). When no agent answers on
// 127.0.0.1:8222, the bridge auto-launches the GUI with --agent and waits
// for it.
// Tools: list_sessions, ssh_connect, ssh_exec, ssh_sudo, ssh_upload,
// ssh_download, ssh_send, ssh_read, ssh_disconnect, get_public_key.
class AgentMcpServer : public QObject {
    Q_OBJECT

public:
    explicit AgentMcpServer(QObject *parent = nullptr);
    ~AgentMcpServer() override;

private:
    void handleMessage(const QByteArray &line);
    // Synchronous handlers use respond(request, ...), which echoes the
    // request id verbatim. Deferred (async) tool flows must use the
    // respondId variants with the ORIGINAL id value: JSON-RPC ids may be
    // numbers, and stringifying them breaks response matching in clients.
    void respond(const QJsonObject &request, const QJsonValue &result);
    void respondError(const QJsonObject &request, int code, const QString &message);
    void respondId(const QJsonValue &id, const QJsonValue &result);
    // data (optional) rides in the JSON-RPC error object: exec failures use
    // it to carry the partial output/exitCode the command produced before
    // the error, so clients don't have to re-run blind.
    void respondErrorId(const QJsonValue &id, int code, const QString &message,
                        const QJsonValue &data = QJsonValue());
    void handleToolCall(const QString &name, const QJsonObject &arguments, const QJsonObject &request);
    void sendMessage(const QJsonObject &message);

    // REST bridge to the GUI agent (127.0.0.1:8222).
    void restCall(const QByteArray &verb, const QString &path, const QJsonObject &body,
                  int timeoutMs, const std::function<void(int, const QJsonObject &)> &cb);
    // Ensures the GUI agent answers; auto-launches `hssh --agent` when not.
    void ensureGui(const std::function<void(bool, const QString &)> &cb);
    // Resolves a tool's "session" argument to an open tab ref (auto-opening
    // saved sessions unless autoOpen=false); answers the tool error itself
    // on failure and skips the callback.
    void withTab(const QJsonValue &rpcId, const QJsonObject &arguments, const QString &tool,
                 const QString &auditDetail, bool autoOpen,
                 const std::function<void(const QString &)> &cb);
    void waitTabConnected(const QJsonValue &rpcId, const QString &ref, int timeoutMs,
                          const std::function<void(const QString &)> &cb);
    // Maps a REST error body to a JSON-RPC tool error (exec contract fields
    // ride in error.data).
    void respondRestError(const QJsonValue &rpcId, int status, const QJsonObject &body);
    void handleBridgedTool(const QString &name, const QJsonObject &arguments,
                           const QJsonValue &rpcId, const QString &detail);

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace hssh

#endif // HSSH_AGENT_AGENTMCPSERVER_H

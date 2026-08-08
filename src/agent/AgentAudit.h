#ifndef HSSH_AGENT_AGENTAUDIT_H
#define HSSH_AGENT_AGENTAUDIT_H

#include <QString>

namespace hssh {

// Append-only audit log for every operation requested through the agent
// interfaces (REST / MCP / CLI). One line per event, UTC-ish local
// timestamp, result always recorded. Secrets are never logged: password
// ciphers are recorded only as "<cipher N bytes>".
//
// File: <AppData>/logs/agent_audit.log, rotated to .1 at ~5 MB.
class AgentAudit {
public:
    enum class Source {
        Rest,
        Mcp,
        Cli
    };

    // action: connect, exec, upload, download, disconnect, sudo, tab_open,
    //         tab_send, tab_close, secure_input, key_request, ...
    // detail: parameters with secrets stripped (host, user, command, paths).
    // result: "ok", "error: ...", "rejected", "pending".
    static void log(Source source, const QString &action,
                    const QString &detail, const QString &result);

    static QString logFilePath();

private:
    static QString sourceName(Source source);
};

} // namespace hssh

#endif // HSSH_AGENT_AGENTAUDIT_H

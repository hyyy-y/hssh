#ifndef HSSH_AGENT_AGENTHTTPSERVER_H
#define HSSH_AGENT_AGENTHTTPSERVER_H

#include "core/SessionConfig.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <memory>

class QTcpServer;
class QTcpSocket;

namespace hssh {

struct HttpRequest;

// Read-only view over the GUI's open terminal tabs. MainWindow implements
// this and hands it to the agent server so external tools (curl, Claude,
// kimi, scripts) can list, read and send input to the open SSH/terminal
// windows.
class AgentTabsInterface {
public:
    virtual ~AgentTabsInterface() = default;
    // [{index,title,type("ssh"|"local"),connected}]
    virtual QVariantList listTabs() const = 0;
    // Sends text + Enter to the tab's shell; false when the index is invalid
    // or the tab is not a terminal session.
    virtual bool sendToTab(int index, const QString &text) = 0;
    // Sends raw input verbatim (no auto-Enter): control characters like
    // Ctrl+C ("\u0003"), half-typed commands, etc.
    virtual bool sendInputToTab(int index, const QString &data) = 0;
    // Last maxLines rows of the terminal buffer; false for invalid index.
    virtual bool readTab(int index, int maxLines, QString *text) const = 0;
    // Windowed read: fromLine rows into the content, maxLines caps it (0=all).
    // Used by agents draining continuous output with a cursor.
    virtual bool readTabRange(int index, int fromLine, int maxLines, QString *text) const = 0;
    // Opens a new local terminal tab and returns its index (-1 on failure).
    virtual int openLocalTab(const QString &shellType) = 0;
    // Opens an SSH tab from a saved session, matched by session name, host
    // or id. Returns the tab index (-1 when not found).
    virtual int openSessionTab(const QString &name) = 0;
    // Opens an SSH tab from an ad-hoc (unsaved) config, credentials
    // included. Returns the tab index (-1 on invalid config).
    virtual int openSshTab(const SessionConfig &config) = 0;
    // Saved sessions from the repository (no secrets):
    // [{name,displayName,host,port,username,locked}]
    virtual QVariantList listSavedSessions() const = 0;
    // Closes the tab; false when the index is invalid or not a terminal.
    virtual bool closeTab(int index) = 0;
    // Triggers reconnect on a disconnected SSH tab (equivalent to the
    // Enter-to-reconnect gesture). False when the index is invalid, the tab
    // is not SSH, or the tab is still connected (reconnect on a live tab
    // would kill its foreground program).
    virtual bool reconnectTab(int index) = 0;

    // Types a decrypted secret into the tab's shell (adds Enter). Used by
    // the cipher channel: the plaintext never leaves the process.
    virtual bool sendSecretToTab(int index, const QString &text) = 0;
    // Full config of an SSH tab (credentials included; invalid config for
    // local tabs / bad index). Powers /tabs/<ref>/upload|download over a
    // dedicated SFTP connection (never shares the terminal's ssh_session:
    // libssh sessions are not thread-safe).
    virtual SessionConfig sessionConfigForTab(int index) const = 0;
    // Asks the user to approve a sudo command for the tab. Approvals are
    // remembered for the tab's lifetime; "Always Allow" in the dialog grants
    // sudo for the machine permanently (AgentSudoAuth list). On false,
    // reason receives a stable code: "unknown_tab" (bad/drifted index or not
    // a terminal), "user_rejected", "timeout" (no answer within 30 s — the
    // dialog may be hidden behind other windows).
    virtual bool confirmSudo(int index, const QString &command, QString *reason) = 0;
    // Full sudo flow: sends the command, answers the password prompt with
    // `secret` (or the tab's stored credential when useStoredCredential),
    // waits for a completion sentinel up to timeoutMs. output receives the
    // tail of the terminal buffer; timedOut is set when the sentinel never
    // appeared (command not confirmed executed); exitCode is the real sudo
    // exit status parsed from the sentinel (-1 when unknown). False on hard
    // failure (bad tab, missing credential).
    virtual bool sudoExec(int index, const QString &command, const QString &secret,
                          bool useStoredCredential, int timeoutMs,
                          QString *output, bool *timedOut, int *exitCode,
                          QString *errorMessage) = 0;
};

// Local REST API (default http://127.0.0.1:8222). Everything is VISIBLE:
// there is no headless session pool — all execution happens in real GUI
// terminal tabs the user can watch. Routes:
//   GET    /api/v1/status                     (liveness: name, version, pid, tabs)
//   GET    /api/v1/health                     (liveness: pid, pending ops, tabs)
//   GET    /api/v1/keys                       (agent RSA public key for ciphers)
//   GET    /api/v1/tabs                       (open terminal tabs incl. "ref")
//   POST   /api/v1/tabs                       {session:name} opens a saved-session tab;
//                                             {shellType} opens a local terminal;
//                                             {host,port?,username?,authMethod?,
//                                              passwordCipher?,privateKeyPath?...}
//                                             opens an ad-hoc SSH tab
//   GET    /api/v1/saved-sessions             (saved session list, no secrets)
//   GET    /api/v1/tabs/<ref>/text?lines=N&from=M   (terminal buffer dump)
//   POST   /api/v1/tabs/<ref>/send            {command} or raw {data} (Ctrl+C etc.)
//   POST   /api/v1/tabs/<ref>/exec            {command, timeout?} — visible exec:
//                                             types the command into the tab and
//                                             captures output between unique begin/
//                                             end markers; answers {output, exitCode,
//                                             timedOut}. One exec per tab (409 when
//                                             busy). The tab must sit at a shell
//                                             prompt — input goes to whatever program
//                                             is in the foreground.
//   POST   /api/v1/tabs/<ref>/sudo            {command, useStoredCredential?, timeout?}
//                                             403 carries a "reason" code (user_rejected /
//                                             timeout / unknown_tab) — timeout means the
//                                             dialog went unanswered, NOT a denial
//   POST   /api/v1/tabs/<ref>/upload          {localPath, remotePath, verify?, method?} (SSH tabs)
//   POST   /api/v1/tabs/<ref>/download        {remotePath, localPath, verify?, method?} (SSH tabs)
//                                             method: "sftp" (default, resumable) / "scp"
//                                             (remote scp binary) / "shell" (base64 over
//                                             exec channel — no sftp-server needed) /
//                                             "auto" (sftp -> scp -> shell while 0 bytes
//                                             moved). Both directions verify content
//                                             (md5 + change detection) by default and
//                                             retry once over a fresh connection.
//   DELETE /api/v1/tabs/<ref>                 (closes the tab)
// Tab <ref>: legacy positional index, or drift-safe "<name>[:<ordinal>]"
// (name = sessionName/host/title; ordinal counts same-name tabs, 1-based).
// Request bodies must be UTF-8 JSON; a body that fails to parse gets a 400
// with explicit guidance (PowerShell 5.1 needs an explicit charset header).
// /api/v2/* mirrors the list endpoints with arrays wrapped in objects
// (tabs -> {"tabs":...}; saved-sessions -> {"savedSessions":...}).
// When a token is configured (Config key "agent/token"), every request must
// carry "Authorization: Bearer <token>" except /api/v1/status.
class AgentHttpServer : public QObject {
    Q_OBJECT

public:
    explicit AgentHttpServer(QObject *parent = nullptr);
    ~AgentHttpServer() override;

    bool start(int port = 8222);
    void stop();
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] int port() const;
    [[nodiscard]] QString url() const;
    [[nodiscard]] QString errorString() const;

    void setTabsInterface(AgentTabsInterface *tabs);

signals:
    // Emitted when the listening socket hits an accept error at runtime
    // (previously silent — the agent looked alive but answered nothing).
    void acceptErrorOccurred(const QString &message);

private:
    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    // Mutable copy: /api/v2/ paths are rewritten in-place to /api/v1/.
    void handleRequest(QTcpSocket *socket, HttpRequest request);
    void respond(QTcpSocket *socket, int status, const QByteArray &body);
    void respondError(QTcpSocket *socket, int status, const QString &message);
    void onTabTransferFinished(const QString &requestId, const QString &path,
                               bool ok, const QString &message);
    // Creates and starts the transfer worker for the given method
    // ("sftp"/"scp"/"shell"); bookkeeping must be in place already.
    void startTabTransferWorker(const QString &requestId, const QString &method);
    // Visible tab exec: types the marker-bracketed command and starts the
    // poll timer (called directly, or after a reset Ctrl+C delay).
    void beginTabExec(const QString &requestId);
    // Visible tab exec: polls the tab buffer for the end marker.
    void pollTabExec(const QString &requestId);
    void finishTabExec(const QString &requestId); // cleanup without responding
    // "user@host:port" for a tab index (live peer preferred over config).
    QString tabTarget(int index) const;

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace hssh

#endif // HSSH_AGENT_AGENTHTTPSERVER_H

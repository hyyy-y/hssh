#ifndef HSSH_AGENT_AGENTHTTPSERVER_H
#define HSSH_AGENT_AGENTHTTPSERVER_H

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <memory>

class QTcpServer;
class QTcpSocket;

namespace hssh {

struct HttpRequest;

class AgentSessionRegistry;

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
    // Closes the tab; false when the index is invalid or not a terminal.
    virtual bool closeTab(int index) = 0;

    // Types a decrypted secret into the tab's shell (adds Enter). Used by
    // the cipher channel: the plaintext never leaves the process.
    virtual bool sendSecretToTab(int index, const QString &text) = 0;
    // Asks the user to approve a sudo command for the tab. Approved tabs are
    // remembered for the tab's lifetime (no repeat prompts).
    virtual bool confirmSudo(int index, const QString &command) = 0;
    // Full sudo flow: sends the command, answers the password prompt with
    // `secret` (or the tab's stored credential when useStoredCredential),
    // waits for completion up to timeoutMs. output receives the tail of the
    // terminal buffer; timedOut is set on timeout. False on hard failure
    // (bad tab, missing credential).
    virtual bool sudoExec(int index, const QString &command, const QString &secret,
                          bool useStoredCredential, int timeoutMs,
                          QString *output, bool *timedOut, QString *errorMessage) = 0;
};

// Local REST API (default http://127.0.0.1:8222). Routes:
//   GET    /api/v1/status
//   GET    /api/v1/sessions
//   POST   /api/v1/sessions                 {name?, host, port?, username?,
//                                            authMethod?, password?, privateKeyPath?}
//   POST   /api/v1/sessions/<id>/exec       {command}
//   POST   /api/v1/sessions/<id>/upload     {localPath, remotePath}
//   POST   /api/v1/sessions/<id>/download   {remotePath, localPath}
//   DELETE /api/v1/sessions/<id>
//   GET    /api/v1/tabs                     (GUI terminal tabs, GUI mode only)
//   POST   /api/v1/tabs                     {shellType?} opens a local terminal tab
//   GET    /api/v1/tabs/<i>/text?lines=N    (terminal buffer dump)
//   POST   /api/v1/tabs/<i>/send            {command}
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

    [[nodiscard]] AgentSessionRegistry *registry() const;
    void setTabsInterface(AgentTabsInterface *tabs);

private:
    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    void handleRequest(QTcpSocket *socket, const HttpRequest &request);
    void respond(QTcpSocket *socket, int status, const QByteArray &body);
    void respondError(QTcpSocket *socket, int status, const QString &message);
    void onExecFinished(const QString &sessionId, const QString &requestId,
                        const QString &output, int exitCode, const QString &error);
    void onTransferFinished(const QString &sessionId, const QString &requestId,
                            const QString &path, bool ok, const QString &message);

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace hssh

#endif // HSSH_AGENT_AGENTHTTPSERVER_H

#ifndef HSSH_APP_SESSIONTAB_H
#define HSSH_APP_SESSIONTAB_H

#include "core/SessionConfig.h"

#ifdef HSSH_HAS_LIBSSH
#include "core/KeyStore.h"
#endif

#include <QFont>
#include <QHash>
#include <QStringList>
#include <QWidget>

namespace hssh {

class SshSession;
class SessionConfig;
class TerminalSession;

class SessionTab : public QWidget {
    Q_OBJECT

public:
    explicit SessionTab(const SessionConfig &config, QWidget *parent = nullptr);
    ~SessionTab() override;

    [[nodiscard]] static SessionTab *createLocal(const QString &shellType, QWidget *parent = nullptr);

    [[nodiscard]] SessionConfig config() const;
    // Non-null when this tab runs an SSH shell backed by libssh.
    [[nodiscard]] SshSession *sshSession() const;
    // Plain-text dump of the terminal buffer (last maxLines rows).
    [[nodiscard]] QString readTerminalText(int maxLines) const;
    [[nodiscard]] QString readTerminalTextRange(int fromLine, int maxLines) const;
    // PH1-05: runtime terminal font change.
    void setTerminalFont(const QFont &font);

    // PH2-10: ZMODEM send (local -> remote rz).
    bool zmodemSendFile(const QString &localPath);

signals:
    void sizeChanged(int columns, int rows);
    // Raw keyboard input typed into this tab's terminal (sync-input source).
    void inputTyped(const QByteArray &data);

public slots:
    void connectSession();
    void disconnectSession();
    // Close the current shell process and start a fresh one with the same
    // config (Enter-to-reconnect equivalent; used by the agent API).
    void reconnectSession();
    void runCommand(const QString &command);

private:
    void setupUi();
    // PH2-12: interactive host-key confirmation (GUI thread only).
#ifdef HSSH_HAS_LIBSSH
    KeyStore::HostKeyDecision promptHostKey(const KeyStore::HostKeyInfo &info, bool changed);
#endif
    // PH2-13: remembered 2FA answers for this tab's lifetime (never stored
    // on disk). Keyed by server name + prompt texts.
    QHash<QString, QStringList> m_kbdintCache;

    SessionConfig m_config;
    TerminalSession *m_terminalSession = nullptr;

public:
    // PH2-02: outline dock access (may be null during teardown).
    [[nodiscard]] TerminalSession *terminalSession() const { return m_terminalSession; }
};

} // namespace hssh

#endif // HSSH_APP_SESSIONTAB_H

#ifndef HSSH_APP_SESSIONTAB_H
#define HSSH_APP_SESSIONTAB_H

#include "core/SessionConfig.h"

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

signals:
    void sizeChanged(int columns, int rows);

public slots:
    void connectSession();
    void disconnectSession();
    // Close the current shell process and start a fresh one with the same
    // config (Enter-to-reconnect equivalent; used by the agent API).
    void reconnectSession();
    void runCommand(const QString &command);

private:
    void setupUi();

    SessionConfig m_config;
    TerminalSession *m_terminalSession = nullptr;
};

} // namespace hssh

#endif // HSSH_APP_SESSIONTAB_H

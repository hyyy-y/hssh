#ifndef HSSH_TERMINAL_TERMINALSESSION_H
#define HSSH_TERMINAL_TERMINALSESSION_H

#include "ShellProcess.h"

#include <QFile>
#include <QWidget>

namespace hssh {

class SessionConfig;
class SshShellProcess;
class TerminalWidget;

class TerminalSession : public QWidget {
    Q_OBJECT

public:
    explicit TerminalSession(ShellProcess *process, QWidget *parent = nullptr);
    ~TerminalSession() override;

    [[nodiscard]] ShellProcess *process() const;
    void start();
    void stop();

    // Plain-text dump of the terminal buffer (last maxLines rows).
    [[nodiscard]] QString bufferText(int maxLines) const;
    [[nodiscard]] QString bufferTextRange(int fromLine, int maxLines) const;

    // Unified input entry (keyboard and API paths share this so the
    // link-dead guard and Enter-to-reconnect work for both).
    void sendInput(const QByteArray &data);

    // Close the current process and start a fresh one (Enter-to-reconnect).
    void reconnect();

signals:
    void sizeChanged(int columns, int rows);
    // Emitted when the link drops and the user can press Enter to reconnect.
    void linkDown(bool autoReconnect);

private slots:
    void onInputReceived(const QByteArray &data);
    void onDataReceived(const QByteArray &data);
    void onProcessFinished(int exitCode);
    void onProcessError(const QString &message);
    void onLinkDown(bool autoReconnect);

private:
    ShellProcess *m_process = nullptr;
    TerminalWidget *m_terminal = nullptr;
    // Automatic session log (Config "session/logging", enabled by default);
    // written to <AppData>/logs/session_<timestamp>.log.
    QFile m_logFile;
    // Set when the link has dropped; input is then intercepted for the
    // Enter-to-reconnect gesture instead of being written to the process.
    bool m_linkDead = false;
};

} // namespace hssh

#endif // HSSH_TERMINAL_TERMINALSESSION_H

#ifndef HSSH_TERMINAL_SSHSHELLPROCESS_H
#define HSSH_TERMINAL_SSHSHELLPROCESS_H

#include "ShellProcess.h"

#include "core/SessionConfig.h"

#include <QProcess>

namespace hssh {

class SshSession;

class SshShellProcess : public ShellProcess {
    Q_OBJECT

public:
    explicit SshShellProcess(const SessionConfig &config, QObject *parent = nullptr);
    ~SshShellProcess() override;

    bool start() override;
    void write(const QByteArray &data) override;
    void resize(int columns, int rows) override;
    void close() override;
    [[nodiscard]] bool isRunning() const override;

    // The backing SSH session (null until start() and after close()).
    [[nodiscard]] SshSession *session() const { return m_session; }

signals:
    // The transport dropped (keep-alive failure or read error).
    // autoReconnect indicates the session is already reconnecting itself.
    void linkDown(bool autoReconnect);

private:
    SessionConfig m_config;
    SshSession *m_session = nullptr;
    QString m_sessionId;

#ifdef HSSH_HAS_LIBSSH
    // libssh shell channel handle stored inside SshSession
#else
    QProcess *m_process = nullptr;
#endif
};

} // namespace hssh

#endif // HSSH_TERMINAL_SSHSHELLPROCESS_H

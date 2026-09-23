#ifndef HSSH_TERMINAL_SSHSHELLPROCESS_H
#define HSSH_TERMINAL_SSHSHELLPROCESS_H

#include "ShellProcess.h"

#include "core/KeyStore.h"
#include "core/SessionConfig.h"
#include "core/SshSession.h"

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

    // PH2-12/13 gates. The session object only exists after start() hands
    // one out of the ConnectionManager — store them here and start() applies
    // them to the fresh session. (Calling session()->setXxx before start()
    // dereferenced null: every SSH tab open crashed — caught by live testing
    // 2026-09-23, latent since B3-2.)
    void setHostKeyVerifier(const KeyStore::HostKeyVerifier &verifier) { m_hostKeyVerifier = verifier; }
    void setKbdintPrompter(const KbdintPrompter &prompter) { m_kbdintPrompter = prompter; }

signals:
    // The transport dropped (keep-alive failure or read error).
    // autoReconnect indicates the session is already reconnecting itself.
    void linkDown(bool autoReconnect);

private:
    SessionConfig m_config;
    SshSession *m_session = nullptr;
    QString m_sessionId;
    KeyStore::HostKeyVerifier m_hostKeyVerifier;
    KbdintPrompter m_kbdintPrompter;

#ifdef HSSH_HAS_LIBSSH
    // libssh shell channel handle stored inside SshSession
#else
    QProcess *m_process = nullptr;
#endif
};

} // namespace hssh

#endif // HSSH_TERMINAL_SSHSHELLPROCESS_H

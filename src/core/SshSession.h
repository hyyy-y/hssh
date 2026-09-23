#ifndef HSSH_CORE_SSHSESSION_H
#define HSSH_CORE_SSHSESSION_H

#include "core/SessionConfig.h"
#include "core/transport/ITransport.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <memory>

#ifdef HSSH_HAS_LIBSSH
#include "core/KeyStore.h"
#include "core/SshConnect.h"
#include <libssh/libssh.h>
#endif

class QMutex;

namespace hssh {

class PortForwardManager;

class SshSession : public QObject, public ITransport {
    Q_OBJECT

public:
    enum class State {
        Disconnected,
        Connecting,
        Authenticating,
        Connected,
        Error
    };
    Q_ENUM(State)

    explicit SshSession(QObject *parent = nullptr);
    ~SshSession() override;

    void setSessionConfig(const SessionConfig &config);
    [[nodiscard]] SessionConfig sessionConfig() const;

#ifdef HSSH_HAS_LIBSSH
    // PH2-12: consulted when the server host key is unknown/changed during
    // connect. Without one, the security/hostKeyPolicy config applies
    // (TOFU default: accept new keys, reject changed keys).
    void setHostKeyVerifier(KeyStore::HostKeyVerifier verifier);
    // PH2-13: consulted for interactive keyboard-interactive (2FA) rounds.
    // Without one, rounds beyond a plain password round fail with a clear
    // error (headless/agent paths cannot answer 2FA prompts).
    void setKbdintPrompter(KbdintPrompter prompter);
#endif

    [[nodiscard]] State state() const;
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] bool isConnected() const override;

    void connectToHost() override;
    void disconnect() override;

    // Execute a one-shot command. Emits execFinished on completion.
    void exec(const QString &command);

    // Interactive shell (placeholder for Phase 4 terminal integration).
    void writeShell(const QByteArray &data);
    void setShellSize(int columns, int rows);

    // ITransport interface (aliases to the shell API above).
    void write(const QByteArray &data) override { writeShell(data); }
    void resize(int columns, int rows) override { setShellSize(columns, rows); }

#ifdef HSSH_HAS_LIBSSH
    // Raw libssh handle plus the mutex that serializes every libssh call on
    // this session (reader thread, keep-alive probe, port forwarding and the
    // GUI thread). Returns nullptr when not connected.
    // Callers must lock the returned mutex around every libssh call.
    [[nodiscard]] ssh_session sessionHandle() const;
    [[nodiscard]] QMutex *sessionMutex() const;
#endif

    // Owned by the session; exists even while disconnected. Forwards stop
    // automatically when the session disconnects or is destroyed.
    [[nodiscard]] PortForwardManager *portForwardManager() const;

signals:
    void stateChanged(State state);
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);
    // The transport dropped (keep-alive failure or read error). Reconnect is
    // handled internally when autoReconnect() is enabled.
    void connectionLost();

    // Emitted for both exec and shell output.
    void dataReceived(const QByteArray &data);
    void execFinished(int exitCode);

private:
    void setError(const QString &message);

#ifdef HSSH_HAS_LIBSSH
    void connectToHostLibSsh();
    void onConnectWorkerDone(int rc, const QString &message);
    void execLibSsh(const QString &command);
    void writeShellLibSsh(const QByteArray &data);
    void setShellSizeLibSsh(int columns, int rows);
    void openShellChannelLibSsh();
    void startKeepAliveLibSsh();
    void stopKeepAliveLibSsh();
    void onKeepAliveFailed();
    void onShellReaderFailed();
    void onConnectionLost();
    void scheduleReconnect();
    void clearReconnectTimer();
#endif

    class Impl;
    std::unique_ptr<Impl> d;
    PortForwardManager *m_portForwards = nullptr;
};

} // namespace hssh

#endif // HSSH_CORE_SSHSESSION_H

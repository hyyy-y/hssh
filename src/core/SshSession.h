#ifndef HSSH_CORE_SSHSESSION_H
#define HSSH_CORE_SSHSESSION_H

#include "core/SessionConfig.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <memory>

#ifdef HSSH_HAS_LIBSSH
#include <libssh/libssh.h>
#endif

class QMutex;

namespace hssh {

class PortForwardManager;

class SshSession : public QObject {
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

    [[nodiscard]] State state() const;
    [[nodiscard]] QString errorString() const;
    [[nodiscard]] bool isConnected() const;

    void connectToHost();
    void disconnect();

    // Execute a one-shot command. Emits execFinished on completion.
    void exec(const QString &command);

    // Interactive shell (placeholder for Phase 4 terminal integration).
    void writeShell(const QByteArray &data);
    void setShellSize(int columns, int rows);

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

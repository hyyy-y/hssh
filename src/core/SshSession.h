#ifndef HSSH_CORE_SSHSESSION_H
#define HSSH_CORE_SSHSESSION_H

#include "core/SessionConfig.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <memory>

namespace hssh {

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

signals:
    void stateChanged(State state);
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);

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
#endif

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace hssh

#endif // HSSH_CORE_SSHSESSION_H

#ifndef HSSH_CORE_REMOTECOMMANDCHANNEL_H
#define HSSH_CORE_REMOTECOMMANDCHANNEL_H

#include "SessionConfig.h"
#include "KeyStore.h"

#include <QThread>

#include <atomic>

using ssh_session = struct ssh_session_struct *;

namespace hssh {

// B6 foundation: ad-hoc shell commands over a DEDICATED SSH connection on a
// worker thread — never through the visible terminal tabs (monitoring must
// not pollute the user's shell) and never sharing the terminal's ssh_session
// (libssh sessions are not thread-safe).
//
// Two modes:
//   runCommand  — one-shot: collects stdout, drains stderr, reports the real
//                 exit status (stderr tail rides in `error` on failure).
//   runStream   — long-running (ping, docker logs -f): output chunks are
//                 emitted as they arrive; cancel() closes the channel.
// The connection persists across commands and is re-established when dead.
class RemoteCommandChannel : public QObject {
    Q_OBJECT

public:
    explicit RemoteCommandChannel(const SessionConfig &config, QObject *parent = nullptr);
    ~RemoteCommandChannel() override;

    void start();
    void stop();

    // Host-key gate for this connection (PH2-12 semantics; see
    // KeyStore::verifyAndStoreHostKey). Set BEFORE start(). Without one the
    // default applies: new keys accepted (TOFU), changed keys rejected.
    void setHostKeyVerifier(const KeyStore::HostKeyVerifier &verifier);

    // One-shot. timeoutMs <= 0 means no limit. On timeout the partial output
    // is still delivered (as a final commandOutput chunk) with error="timeout".
    void runCommand(const QString &id, const QString &command, int timeoutMs = 15000);
    // Streaming variant: no timeout, chunks via commandOutput.
    void runStream(const QString &id, const QString &command);
    // Cancels the run with this id (channel close; remote usually gets SIGHUP).
    void cancel(const QString &id);

signals:
    // The connection is up; peerAddress is the REAL socket peer (numeric IP)
    // — the anti-wrong-machine check for cloned-image fleets where
    // known_hosts cannot tell boards apart.
    void connected(const QString &peerAddress);
    // `error` is empty on success; on nonzero exit it carries "exit N: <stderr tail>".
    void commandFinished(const QString &id, int exitCode, const QString &error);
    // stdout chunk (both modes). One-shot delivers a single chunk.
    void commandOutput(const QString &id, const QByteArray &chunk);
    void errorOccurred(const QString &message);

private:
    void doConnect();
    [[nodiscard]] bool ensureConnected(QString *error);
    void doRun(const QString &id, const QString &command, int timeoutMs, bool streaming);

    SessionConfig m_config;
    QThread m_thread;
    ssh_session m_ssh = nullptr;
    KeyStore::HostKeyVerifier m_hostKeyVerifier;
    std::atomic<bool> m_cancelCurrent{false};
    std::atomic<bool> m_stopRequested{false};
};

} // namespace hssh

#endif // HSSH_CORE_REMOTECOMMANDCHANNEL_H

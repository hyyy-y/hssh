#ifndef HSSH_CORE_CHANNELCOPYSESSION_H
#define HSSH_CORE_CHANNELCOPYSESSION_H

#include "SessionConfig.h"
#include "TransferSession.h"
#include "KeyStore.h"

#include <QString>
#include <QThread>

#include <atomic>

using ssh_session = struct ssh_session_struct *;

namespace hssh {

// Single-file transfer over plain SSH channels (no sftp-server required).
// Two wire formats:
//   Base64 — `base64` / `base64 -d` through an exec channel. Works on any
//            POSIX-ish shell (busybox, dropbear without sftp-server); ~33%
//            bandwidth overhead.
//   Scp    — classic scp protocol via libssh (remote `scp` binary; dropbear
//            ships it even when sftp-server is disabled). Single file only,
//            no resume.
// Both verify content (remote md5sum vs local md5) by default and retry once
// on failure.
class ChannelCopySession : public TransferSession {
    Q_OBJECT

public:
    enum class Mode { Base64, Scp };

    explicit ChannelCopySession(const SessionConfig &config, Mode mode,
                                QObject *parent = nullptr);
    ~ChannelCopySession() override;

    void start() override;
    void stop() override;
    // Host-key gate (PH2-12 semantics); set BEFORE start().
    void setHostKeyVerifier(const KeyStore::HostKeyVerifier &verifier);

    void upload(const QString &localPath, const QString &remotePath, bool verify = true) override;
    void download(const QString &remotePath, const QString &localPath, bool verify = true) override;
    void cancelTransfer() override;

private:
    void doConnect();
    void doUpload(const QString &localPath, const QString &remotePath, bool verify);
    void doDownload(const QString &remotePath, const QString &localPath, bool verify);

    // One attempt; `retryable` marks failures worth one fresh-channel retry.
    bool uploadBase64(const QString &localPath, const QString &remotePath, bool verify,
                      QString *error, QString *note, bool *retryable);
    bool downloadBase64(const QString &remotePath, const QString &localPath, bool verify,
                        QString *error, QString *note, bool *retryable);
    bool uploadScp(const QString &localPath, const QString &remotePath, bool verify,
                   QString *error, QString *note, bool *retryable);
    bool downloadScp(const QString &remotePath, const QString &localPath, bool verify,
                     QString *error, QString *note, bool *retryable);

    bool reconnectIfDead(QString *error);

    SessionConfig m_config;
    Mode m_mode;
    QThread m_thread;
    ssh_session m_ssh = nullptr;
    KeyStore::HostKeyVerifier m_hostKeyVerifier;
    std::atomic<bool> m_cancelTransfer{false};
};

} // namespace hssh

#endif // HSSH_CORE_CHANNELCOPYSESSION_H

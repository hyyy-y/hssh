#include "ChannelCopySession.h"

#include "SshConnect.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <vector>

#include <libssh/libssh.h>

// libssh marks the scp API SSH_DEPRECATED, but it is present and functional
// in 0.10.6 and is exactly what dropbear-without-sftp targets support.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace hssh {

namespace {

constexpr int transferChunkSize = 32 * 1024;
// Base64 upload reads local files in multiples of 3 bytes so every encoded
// chunk is self-contained (no partial quantum across writes).
constexpr int base64ReadChunk = 30 * 1024;
constexpr qint64 progressReportInterval = 256 * 1024;

QByteArray shellQuote(const QString &path)
{
    QByteArray quoted = path.toUtf8();
    quoted.replace('\'', "'\\''");
    return "'" + quoted + "'";
}

QByteArray localMd5Hex(const QString &localPath)
{
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash md5(QCryptographicHash::Md5);
    char buffer[transferChunkSize];
    qint64 n = 0;
    while ((n = file.read(buffer, sizeof(buffer))) > 0) {
        md5.addData(QByteArrayView(buffer, static_cast<qsizetype>(n)));
    }
    if (n < 0) {
        return {};
    }
    return md5.result().toHex();
}

// Runs "md5sum -- <path>" over an exec channel; lowercase hex digest or
// empty on any failure (no shell, md5sum missing, unreadable file).
QByteArray remoteMd5Hex(ssh_session ssh, const QString &remotePath)
{
    if (!ssh || !ssh_is_connected(ssh)) {
        return {};
    }
    ssh_channel channel = ssh_channel_new(ssh);
    if (!channel) {
        return {};
    }

    QByteArray digest;
    for (;;) {
        if (ssh_channel_open_session(channel) != SSH_OK) {
            break;
        }
        const QByteArray command = "md5sum -- " + shellQuote(remotePath);
        if (ssh_channel_request_exec(channel, command.constData()) != SSH_OK) {
            break;
        }
        QByteArray output;
        char buffer[512];
        int n = 0;
        while ((n = ssh_channel_read(channel, buffer, sizeof(buffer), 0)) > 0) {
            output.append(buffer, n);
        }
        // Always drain stderr too: bytes left on stream 1 linger in the
        // libssh buffer and can stall EOF/exit-status delivery.
        while (ssh_channel_read(channel, buffer, sizeof(buffer), 1) > 0) {
        }
        ssh_channel_send_eof(channel);
        if (ssh_channel_get_exit_status(channel) != 0) {
            break;
        }
        const QByteArray token = output.trimmed().split(' ').first().toLower();
        bool hex = token.size() == 32;
        for (const char c : token) {
            hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
        if (hex) {
            digest = token;
        }
        break;
    }

    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return digest;
}

// Incremental base64 decoder for streaming stdout: whitespace is skipped,
// full quanta are decoded as they complete, the tail is carried over.
class Base64Decoder {
public:
    // Appends decoded bytes to `out`. Returns false on garbage input.
    bool feed(const char *data, qsizetype size, QByteArray *out)
    {
        for (qsizetype i = 0; i < size; ++i) {
            const char c = data[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                continue;
            }
            m_pending.append(c);
        }
        const qsizetype usable = (m_pending.size() / 4) * 4;
        if (usable == 0) {
            return true;
        }
        const QByteArray chunk = m_pending.left(usable);
        m_pending.remove(0, usable);
        const QByteArray decoded = QByteArray::fromBase64(
            chunk, QByteArray::AbortOnBase64DecodingErrors);
        if (decoded.isEmpty() && !chunk.isEmpty()) {
            return false;
        }
        out->append(decoded);
        return true;
    }
    // Call at EOF: the final partial quantum (padded) must decode cleanly.
    bool finish(QByteArray *out)
    {
        if (m_pending.isEmpty()) {
            return true;
        }
        const QByteArray decoded = QByteArray::fromBase64(
            m_pending, QByteArray::AbortOnBase64DecodingErrors);
        if (decoded.isEmpty()) {
            return false;
        }
        out->append(decoded);
        m_pending.clear();
        return true;
    }

private:
    QByteArray m_pending;
};

} // namespace

ChannelCopySession::ChannelCopySession(const SessionConfig &config, Mode mode, QObject *parent)
    : TransferSession(parent)
    , m_config(config)
    , m_mode(mode)
{
    moveToThread(&m_thread);
}

ChannelCopySession::~ChannelCopySession()
{
    stop();
}

void ChannelCopySession::start()
{
    m_thread.start();
    QMetaObject::invokeMethod(this, [this]() {
        doConnect();
    }, Qt::QueuedConnection);
}

void ChannelCopySession::stop()
{
    if (!m_thread.isRunning()) {
        return;
    }
    m_cancelTransfer = true;
    m_thread.quit();
    if (!m_thread.wait(5000)) {
        m_thread.terminate();
        m_thread.wait();
    }
    sshDisconnectAndFree(m_ssh);
    m_ssh = nullptr;
}

void ChannelCopySession::cancelTransfer()
{
    m_cancelTransfer = true;
}

void ChannelCopySession::upload(const QString &localPath, const QString &remotePath, bool verify)
{
    QMetaObject::invokeMethod(this, [this, localPath, remotePath, verify]() {
        doUpload(localPath, remotePath, verify);
    }, Qt::QueuedConnection);
}

void ChannelCopySession::download(const QString &remotePath, const QString &localPath, bool verify)
{
    QMetaObject::invokeMethod(this, [this, remotePath, localPath, verify]() {
        doDownload(remotePath, localPath, verify);
    }, Qt::QueuedConnection);
}

void ChannelCopySession::doConnect()
{
    QString error;
    // Bulk transfer: post-connect timeout must stay 0 (a timed-out blocking
    // write is a silent short write — the 2026-08-19 corruption lesson).
    m_ssh = sshConnectAndAuthenticate(m_config, &error);
    if (!m_ssh) {
        emit errorOccurred(error);
    }
}

bool ChannelCopySession::reconnectIfDead(QString *error)
{
    if (m_ssh && ssh_is_connected(m_ssh)) {
        return true;
    }
    sshDisconnectAndFree(m_ssh);
    m_ssh = sshConnectAndAuthenticate(m_config, error);
    return m_ssh != nullptr;
}

void ChannelCopySession::doUpload(const QString &localPath, const QString &remotePath, bool verify)
{
    for (int attempt = 1;; ++attempt) {
        QString connError;
        if (!reconnectIfDead(&connError)) {
            emit transferFinished(localPath, false,
                                  tr("Connection lost and reconnect failed: %1").arg(connError));
            return;
        }
        QString error;
        QString note;
        bool retryable = false;
        const bool ok = m_mode == Mode::Base64
            ? uploadBase64(localPath, remotePath, verify, &error, &note, &retryable)
            : uploadScp(localPath, remotePath, verify, &error, &note, &retryable);
        if (ok) {
            emit transferFinished(localPath, true, note);
            return;
        }
        if (retryable && attempt == 1 && !m_cancelTransfer) {
            continue; // fresh channel/scp session on the second attempt
        }
        emit transferFinished(localPath, false, error);
        return;
    }
}

void ChannelCopySession::doDownload(const QString &remotePath, const QString &localPath, bool verify)
{
    for (int attempt = 1;; ++attempt) {
        QString connError;
        if (!reconnectIfDead(&connError)) {
            emit transferFinished(remotePath, false,
                                  tr("Connection lost and reconnect failed: %1").arg(connError));
            return;
        }
        QString error;
        QString note;
        bool retryable = false;
        const bool ok = m_mode == Mode::Base64
            ? downloadBase64(remotePath, localPath, verify, &error, &note, &retryable)
            : downloadScp(remotePath, localPath, verify, &error, &note, &retryable);
        if (ok) {
            emit transferFinished(remotePath, true, note);
            return;
        }
        if (retryable && attempt == 1 && !m_cancelTransfer) {
            // Neither format supports resume: the retry starts from scratch,
            // which also discards a corrupt prefix.
            QFile::remove(localPath + QStringLiteral(".part"));
            continue;
        }
        emit transferFinished(remotePath, false, error);
        return;
    }
}

// ---------------------------------------------------------------------------
// Base64 over an exec channel
// ---------------------------------------------------------------------------

bool ChannelCopySession::uploadBase64(const QString &localPath, const QString &remotePath,
                                      bool verify, QString *outError, QString *outNote,
                                      bool *outRetryable)
{
    const auto fail = [outError](const QString &message) {
        if (outError) {
            *outError = message;
        }
        return false;
    };
    *outRetryable = false;

    QFile local(localPath);
    if (!local.open(QIODevice::ReadOnly)) {
        return fail(tr("Cannot read %1").arg(localPath));
    }
    const qint64 total = local.size();

    ssh_channel channel = ssh_channel_new(m_ssh);
    if (!channel) {
        return fail(QString::fromUtf8(ssh_get_error(m_ssh)));
    }

    bool ok = false;
    QString error;
    for (;;) {
        if (ssh_channel_open_session(channel) != SSH_OK) {
            error = QString::fromUtf8(ssh_get_error(m_ssh));
            *outRetryable = true; // channel-level failure: a fresh channel may work
            break;
        }
        const QByteArray command = "base64 -d > " + shellQuote(remotePath);
        if (ssh_channel_request_exec(channel, command.constData()) != SSH_OK) {
            error = QString::fromUtf8(ssh_get_error(m_ssh));
            *outRetryable = true;
            break;
        }

        emit transferStep(localPath, 1, 1, remotePath.section(QLatin1Char('/'), -1));
        m_cancelTransfer = false;
        ok = true;
        qint64 done = 0;
        qint64 lastReport = 0;
        while (!m_cancelTransfer) {
            const QByteArray chunk = local.read(base64ReadChunk);
            if (chunk.isEmpty()) {
                if (local.error() != QFileDevice::NoError) {
                    ok = false;
                    error = tr("Failed to read %1: %2").arg(localPath, local.errorString());
                }
                break; // EOF
            }
            QByteArray encoded = chunk.toBase64();
            encoded.append('\n'); // line-wrap: some base64 -d variants want it
            qsizetype written = 0;
            while (written < encoded.size()) {
                const ssize_t n = ssh_channel_write(channel, encoded.constData() + written,
                                                    static_cast<uint32_t>(encoded.size() - written));
                if (n < 0) {
                    ok = false;
                    error = QString::fromUtf8(ssh_get_error(m_ssh));
                    *outRetryable = true;
                    break;
                }
                if (n == 0) {
                    // Timeout=0 makes this a blocking call; 0 means a wedged
                    // channel. Fail loudly rather than spinning.
                    ok = false;
                    error = tr("Channel write stalled during upload");
                    *outRetryable = true;
                    break;
                }
                written += n;
            }
            if (!ok) {
                break;
            }
            done += chunk.size();
            if (done - lastReport >= progressReportInterval) {
                lastReport = done;
                emit transferProgress(localPath, done, total);
            }
        }
        ssh_channel_send_eof(channel);
        // Drain both streams so the exit status is delivered and nothing
        // lingers on the channel.
        char drain[1024];
        while (ssh_channel_read(channel, drain, sizeof(drain), 0) > 0) {
        }
        QByteArray stderrText;
        int sn = 0;
        while ((sn = ssh_channel_read(channel, drain, sizeof(drain), 1)) > 0) {
            stderrText.append(drain, sn);
        }
        const int exitStatus = ssh_channel_get_exit_status(channel);
        if (!ok) {
            break; // error already set
        }
        if (m_cancelTransfer) {
            error = tr("Cancelled");
            ok = false;
            break;
        }
        if (exitStatus != 0) {
            // e.g. base64 missing (127) or unwritable target (1)
            error = tr("Remote 'base64 -d' failed (exit %1): %2")
                        .arg(exitStatus)
                        .arg(QString::fromUtf8(stderrText.trimmed()));
            break; // not retryable: same command would fail again
        }

        if (verify) {
            const QByteArray localDigest = localMd5Hex(localPath);
            const QByteArray remoteDigest = remoteMd5Hex(m_ssh, remotePath);
            if (!remoteDigest.isEmpty() && remoteDigest != localDigest) {
                error = tr("Upload verification failed: remote content differs "
                           "(local md5 %1, remote md5 %2)")
                            .arg(QString::fromLatin1(localDigest),
                                 QString::fromLatin1(remoteDigest));
                *outRetryable = true;
                ok = false;
                break;
            }
            if (outNote) {
                *outNote = remoteDigest.isEmpty()
                    ? tr("uploaded via base64, md5 %1; verification unavailable (no remote md5sum)")
                          .arg(QString::fromLatin1(localDigest))
                    : tr("uploaded via base64, md5 verified: %1")
                          .arg(QString::fromLatin1(localDigest));
            }
        } else if (outNote) {
            *outNote = tr("uploaded via base64 (verification off)");
        }
        emit transferProgress(localPath, done, total);
        break;
    }

    ssh_channel_close(channel);
    ssh_channel_free(channel);
    local.close();
    if (!ok) {
        return fail(error);
    }
    return true;
}

bool ChannelCopySession::downloadBase64(const QString &remotePath, const QString &localPath,
                                        bool verify, QString *outError, QString *outNote,
                                        bool *outRetryable)
{
    const auto fail = [outError](const QString &message) {
        if (outError) {
            *outError = message;
        }
        return false;
    };
    *outRetryable = false;

    // Atomic landing: bytes stream into "<local>.part" and are renamed to
    // the final name only after success (+ verification).
    const QString partPath = localPath + QStringLiteral(".part");
    QFile local(partPath);
    QDir().mkpath(QFileInfo(localPath).absolutePath());
    if (!local.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(tr("Cannot write to %1").arg(partPath));
    }

    ssh_channel channel = ssh_channel_new(m_ssh);
    if (!channel) {
        return fail(QString::fromUtf8(ssh_get_error(m_ssh)));
    }

    bool ok = false;
    QString error;
    qint64 done = 0;
    for (;;) {
        if (ssh_channel_open_session(channel) != SSH_OK) {
            error = QString::fromUtf8(ssh_get_error(m_ssh));
            *outRetryable = true;
            break;
        }
        const QByteArray command = "base64 -- " + shellQuote(remotePath);
        if (ssh_channel_request_exec(channel, command.constData()) != SSH_OK) {
            error = QString::fromUtf8(ssh_get_error(m_ssh));
            *outRetryable = true;
            break;
        }

        emit transferStep(remotePath, 1, 1, remotePath.section(QLatin1Char('/'), -1));
        m_cancelTransfer = false;
        ok = true;
        qint64 lastReport = 0;
        Base64Decoder decoder;
        std::vector<char> buffer(transferChunkSize);
        int n = 0;
        while (!m_cancelTransfer
               && (n = ssh_channel_read(channel, buffer.data(),
                                        static_cast<uint32_t>(buffer.size()), 0)) > 0) {
            QByteArray decoded;
            if (!decoder.feed(buffer.data(), n, &decoded)) {
                ok = false;
                error = tr("Garbage in base64 stream from remote");
                break;
            }
            if (!decoded.isEmpty()) {
                if (local.write(decoded) != decoded.size()) {
                    ok = false;
                    error = tr("Failed to write to %1").arg(localPath);
                    break;
                }
                done += decoded.size();
                if (done - lastReport >= progressReportInterval) {
                    lastReport = done;
                    emit transferProgress(remotePath, done, 0); // total unknown until EOF
                }
            }
        }
        if (ok && n < 0) {
            ok = false;
            error = QString::fromUtf8(ssh_get_error(m_ssh));
            *outRetryable = true;
        }
        // Drain stderr (error text from the remote base64).
        QByteArray stderrText;
        int sn = 0;
        while ((sn = ssh_channel_read(channel, buffer.data(),
                                      static_cast<uint32_t>(buffer.size()), 1)) > 0) {
            stderrText.append(buffer.data(), sn);
        }
        ssh_channel_send_eof(channel);
        const int exitStatus = ssh_channel_get_exit_status(channel);
        if (!ok) {
            break;
        }
        if (m_cancelTransfer) {
            error = tr("Cancelled");
            ok = false;
            break;
        }
        QByteArray tail;
        if (!decoder.finish(&tail)) {
            error = tr("Truncated base64 stream from remote");
            ok = false;
            *outRetryable = true;
            break;
        }
        if (!tail.isEmpty() && local.write(tail) != tail.size()) {
            error = tr("Failed to write to %1").arg(localPath);
            ok = false;
            break;
        }
        done += tail.size();
        if (exitStatus != 0) {
            // e.g. base64 missing (127) or unreadable file (1)
            error = tr("Remote 'base64' failed (exit %1): %2")
                        .arg(exitStatus)
                        .arg(QString::fromUtf8(stderrText.trimmed()));
            ok = false;
            break;
        }

        if (verify) {
            const QByteArray localDigest = localMd5Hex(partPath);
            const QByteArray remoteDigest = remoteMd5Hex(m_ssh, remotePath);
            if (!remoteDigest.isEmpty() && remoteDigest != localDigest) {
                error = tr("Download verification failed: local content differs "
                           "(remote md5 %1, local md5 %2) — the remote file may "
                           "be rewritten concurrently")
                            .arg(QString::fromLatin1(remoteDigest),
                                 QString::fromLatin1(localDigest));
                *outRetryable = true;
                ok = false;
                break;
            }
            if (outNote) {
                *outNote = remoteDigest.isEmpty()
                    ? tr("downloaded via base64, md5 %1; verification unavailable (no remote md5sum)")
                          .arg(QString::fromLatin1(localDigest))
                    : tr("downloaded via base64, md5 verified: %1")
                          .arg(QString::fromLatin1(localDigest));
            }
        } else if (outNote) {
            *outNote = tr("downloaded via base64 (verification off)");
        }
        // Atomic landing: only now does the file appear under its real name.
        // Close the .part first — Windows refuses to rename an open file
        // (2026-09-20 verification round).
        local.close();
        QFile::remove(localPath);
        if (!QFile::rename(partPath, localPath)) {
            ok = false;
            error = tr("Downloaded to %1 but failed to finalize %2").arg(partPath, localPath);
            break;
        }
        emit transferProgress(remotePath, done, done);
        break;
    }

    ssh_channel_close(channel);
    ssh_channel_free(channel);
    local.close();
    if (!ok) {
        return fail(error);
    }
    return true;
}

// ---------------------------------------------------------------------------
// scp via libssh (remote `scp` binary; single file, no resume)
// ---------------------------------------------------------------------------

bool ChannelCopySession::uploadScp(const QString &localPath, const QString &remotePath,
                                   bool verify, QString *outError, QString *outNote,
                                   bool *outRetryable)
{
    const auto fail = [outError](const QString &message) {
        if (outError) {
            *outError = message;
        }
        return false;
    };
    *outRetryable = false;

    QFile local(localPath);
    if (!local.open(QIODevice::ReadOnly)) {
        return fail(tr("Cannot read %1").arg(localPath));
    }
    const qint64 total = local.size();

    // scp wants a target DIRECTORY at session level; the file name is pushed
    // separately. A trailing slash means "into that directory".
    QString dir = remotePath;
    QString base;
    if (remotePath.endsWith(QLatin1Char('/'))) {
        dir.chop(1);
        base = QFileInfo(localPath).fileName();
    } else {
        const int slash = remotePath.lastIndexOf(QLatin1Char('/'));
        if (slash > 0) {
            dir = remotePath.left(slash);
            base = remotePath.mid(slash + 1);
        } else {
            dir = QStringLiteral(".");
            base = remotePath;
        }
    }
    if (base.isEmpty()) {
        base = QFileInfo(localPath).fileName();
    }

    ssh_scp scp = ssh_scp_new(m_ssh, SSH_SCP_WRITE, dir.toUtf8().constData());
    if (!scp) {
        return fail(QString::fromUtf8(ssh_get_error(m_ssh)));
    }

    bool ok = false;
    QString error;
    for (;;) {
        if (ssh_scp_init(scp) != SSH_OK) {
            error = QString::fromUtf8(ssh_get_error(m_ssh));
            *outRetryable = true;
            break;
        }
        if (ssh_scp_push_file64(scp, base.toUtf8().constData(),
                                static_cast<uint64_t>(total), 0644) != SSH_OK) {
            error = QString::fromUtf8(ssh_get_error(m_ssh));
            *outRetryable = true;
            break;
        }

        emit transferStep(localPath, 1, 1, base);
        m_cancelTransfer = false;
        ok = true;
        qint64 done = 0;
        qint64 lastReport = 0;
        while (!m_cancelTransfer) {
            const QByteArray chunk = local.read(transferChunkSize);
            if (chunk.isEmpty()) {
                if (local.error() != QFileDevice::NoError) {
                    ok = false;
                    error = tr("Failed to read %1: %2").arg(localPath, local.errorString());
                }
                break;
            }
            if (ssh_scp_write(scp, chunk.constData(),
                              static_cast<size_t>(chunk.size())) != SSH_OK) {
                ok = false;
                error = QString::fromUtf8(ssh_get_error(m_ssh));
                *outRetryable = true;
                break;
            }
            done += chunk.size();
            if (done - lastReport >= progressReportInterval) {
                lastReport = done;
                emit transferProgress(localPath, done, total);
            }
        }
        if (ok && m_cancelTransfer) {
            ok = false;
            error = tr("Cancelled");
        }
        if (!ok) {
            break;
        }

        if (verify) {
            const QByteArray localDigest = localMd5Hex(localPath);
            const QString remoteFile = dir == QLatin1String(".")
                ? base : dir + QLatin1Char('/') + base;
            const QByteArray remoteDigest = remoteMd5Hex(m_ssh, remoteFile);
            if (!remoteDigest.isEmpty() && remoteDigest != localDigest) {
                error = tr("Upload verification failed: remote content differs "
                           "(local md5 %1, remote md5 %2)")
                            .arg(QString::fromLatin1(localDigest),
                                 QString::fromLatin1(remoteDigest));
                *outRetryable = true;
                ok = false;
                break;
            }
            if (outNote) {
                *outNote = remoteDigest.isEmpty()
                    ? tr("uploaded via scp, md5 %1; verification unavailable (no remote md5sum)")
                          .arg(QString::fromLatin1(localDigest))
                    : tr("uploaded via scp, md5 verified: %1")
                          .arg(QString::fromLatin1(localDigest));
            }
        } else if (outNote) {
            *outNote = tr("uploaded via scp (verification off)");
        }
        emit transferProgress(localPath, total, total);
        break;
    }

    ssh_scp_close(scp);
    ssh_scp_free(scp);
    local.close();
    if (!ok) {
        return fail(error);
    }
    return true;
}

bool ChannelCopySession::downloadScp(const QString &remotePath, const QString &localPath,
                                     bool verify, QString *outError, QString *outNote,
                                     bool *outRetryable)
{
    const auto fail = [outError](const QString &message) {
        if (outError) {
            *outError = message;
        }
        return false;
    };
    *outRetryable = false;

    ssh_scp scp = ssh_scp_new(m_ssh, SSH_SCP_READ, remotePath.toUtf8().constData());
    if (!scp) {
        return fail(QString::fromUtf8(ssh_get_error(m_ssh)));
    }

    // Atomic landing: bytes stream into "<local>.part" and are renamed to
    // the final name only after success (+ verification).
    const QString partPath = localPath + QStringLiteral(".part");
    QFile local(partPath);
    QDir().mkpath(QFileInfo(localPath).absolutePath());

    bool ok = false;
    QString error;
    qint64 done = 0;
    for (;;) {
        if (ssh_scp_init(scp) != SSH_OK) {
            error = QString::fromUtf8(ssh_get_error(m_ssh));
            *outRetryable = true;
            break;
        }

        m_cancelTransfer = false;
        ok = true;
        bool fileDone = false;
        while (!m_cancelTransfer && !fileDone) {
            const int request = ssh_scp_pull_request(scp);
            switch (request) {
            case SSH_SCP_REQUEST_NEWFILE: {
                const quint64 size = ssh_scp_request_get_size64(scp);
                const QString name = QString::fromUtf8(ssh_scp_request_get_filename(scp));
                emit transferStep(remotePath, 1, 1, name);
                // Report the size immediately (status endpoint bytesTotal).
                emit transferProgress(remotePath, 0, static_cast<qint64>(size));
                if (ssh_scp_accept_request(scp) != SSH_OK) {
                    ok = false;
                    error = QString::fromUtf8(ssh_get_error(m_ssh));
                    break;
                }
                if (!local.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    ok = false;
                    error = tr("Cannot write to %1").arg(partPath);
                    ssh_scp_deny_request(scp, "local open failed");
                    break;
                }
                std::vector<char> buffer(transferChunkSize);
                quint64 remaining = size;
                qint64 lastReport = 0;
                while (ok && remaining > 0 && !m_cancelTransfer) {
                    const int n = ssh_scp_read(scp, buffer.data(),
                                               static_cast<size_t>(
                                                   qMin<quint64>(remaining, buffer.size())));
                    if (n < 0) {
                        ok = false;
                        error = QString::fromUtf8(ssh_get_error(m_ssh));
                        *outRetryable = true;
                        break;
                    }
                    if (n == 0) {
                        continue; // no data yet on a blocking session should not happen
                    }
                    if (local.write(buffer.data(), n) != n) {
                        ok = false;
                        error = tr("Failed to write to %1").arg(localPath);
                        break;
                    }
                    remaining -= static_cast<quint64>(n);
                    done += n;
                    if (done - lastReport >= progressReportInterval) {
                        lastReport = done;
                        emit transferProgress(remotePath, done, static_cast<qint64>(size));
                    }
                }
                local.close();
                fileDone = true;
                break;
            }
            case SSH_SCP_REQUEST_NEWDIR:
                // scp -r source: not supported by design (single file only).
                ssh_scp_deny_request(scp, "recursive download not supported");
                ok = false;
                error = tr("Remote path is a directory; scp transfer supports single files only");
                break;
            case SSH_SCP_REQUEST_WARNING:
                error = QString::fromUtf8(ssh_scp_request_get_warning(scp));
                break;
            case SSH_SCP_REQUEST_EOF:
                fileDone = true;
                if (ok && done == 0) {
                    // EOF before any NEWFILE: the remote scp found nothing
                    // (its stderr arrives as a warning, e.g. "No such file").
                    ok = false;
                    if (error.isEmpty()) {
                        error = tr("Remote file not found: %1").arg(remotePath);
                    }
                }
                break;
            default:
                ok = false;
                error = QString::fromUtf8(ssh_get_error(m_ssh));
                *outRetryable = true;
                break;
            }
            if (!ok || fileDone) {
                break;
            }
        }
        if (!ok) {
            break;
        }
        if (m_cancelTransfer) {
            ok = false;
            error = tr("Cancelled");
            break;
        }

        if (verify) {
            const QByteArray localDigest = localMd5Hex(partPath);
            const QByteArray remoteDigest = remoteMd5Hex(m_ssh, remotePath);
            if (!remoteDigest.isEmpty() && remoteDigest != localDigest) {
                error = tr("Download verification failed: local content differs "
                           "(remote md5 %1, local md5 %2)")
                            .arg(QString::fromLatin1(remoteDigest),
                                 QString::fromLatin1(localDigest));
                *outRetryable = true;
                ok = false;
                break;
            }
            if (outNote) {
                *outNote = remoteDigest.isEmpty()
                    ? tr("downloaded via scp, md5 %1; verification unavailable (no remote md5sum)")
                          .arg(QString::fromLatin1(localDigest))
                    : tr("downloaded via scp, md5 verified: %1")
                          .arg(QString::fromLatin1(localDigest));
            }
        } else if (outNote) {
            *outNote = tr("downloaded via scp (verification off)");
        }
        // Atomic landing: only now does the file appear under its real name.
        // Close the .part first — Windows refuses to rename an open file.
        local.close();
        QFile::remove(localPath);
        if (!QFile::rename(partPath, localPath)) {
            ok = false;
            error = tr("Downloaded to %1 but failed to finalize %2").arg(partPath, localPath);
            break;
        }
        emit transferProgress(remotePath, done, done);
        break;
    }

    ssh_scp_close(scp);
    ssh_scp_free(scp);
    if (!ok) {
        return fail(error);
    }
    return true;
}

} // namespace hssh

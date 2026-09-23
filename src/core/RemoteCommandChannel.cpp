#include "RemoteCommandChannel.h"

#include "SshConnect.h"

#include <QElapsedTimer>
#include <QThread>

#include <libssh/libssh.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace hssh {

namespace {

// A channel that frees itself on scope exit — early returns below must never
// leak an open channel.
struct ChannelGuard {
    ssh_channel channel = nullptr;
    ~ChannelGuard()
    {
        if (channel) {
            ssh_channel_close(channel);
            ssh_channel_free(channel);
        }
    }
};

// Numeric peer IP of a connected session. Call from the worker thread that
// owns the session (no locking needed there).
QString peerAddressOf(ssh_session session)
{
    if (!session) {
        return {};
    }
    const socket_t fd = ssh_get_fd(session);
    if (fd == SSH_INVALID_SOCKET) {
        return {};
    }
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        return {};
    }
    char host[NI_MAXHOST] = {};
    if (getnameinfo(reinterpret_cast<sockaddr *>(&addr), len,
                    host, sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0) {
        return {};
    }
    return QString::fromLatin1(host);
}

} // namespace

RemoteCommandChannel::RemoteCommandChannel(const SessionConfig &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
{
    // Parentless on purpose: moveToThread refuses parented objects (the
    // silent-worker-on-GUI-thread trap from 2026-09-18).
    moveToThread(&m_thread);
}

RemoteCommandChannel::~RemoteCommandChannel()
{
    stop();
}

void RemoteCommandChannel::start()
{
    m_stopRequested = false;
    m_thread.start();
    QMetaObject::invokeMethod(this, [this]() {
        doConnect();
    }, Qt::QueuedConnection);
}

void RemoteCommandChannel::stop()
{
    if (!m_thread.isRunning()) {
        return;
    }
    m_stopRequested = true;
    m_cancelCurrent = true;
    m_thread.quit();
    if (!m_thread.wait(5000)) {
        m_thread.terminate();
        m_thread.wait();
    }
    sshDisconnectAndFree(m_ssh);
    m_ssh = nullptr;
}

void RemoteCommandChannel::runCommand(const QString &id, const QString &command, int timeoutMs)
{
    QMetaObject::invokeMethod(this, [this, id, command, timeoutMs]() {
        doRun(id, command, timeoutMs, false);
    }, Qt::QueuedConnection);
}

void RemoteCommandChannel::runStream(const QString &id, const QString &command)
{
    QMetaObject::invokeMethod(this, [this, id, command]() {
        doRun(id, command, 0, true);
    }, Qt::QueuedConnection);
}

void RemoteCommandChannel::cancel(const QString &id)
{
    QMetaObject::invokeMethod(this, [this, id]() {
        // Only the CURRENT run can be cancelled; a stale id (already
        // finished) is a no-op.
        m_cancelCurrent = true;
        Q_UNUSED(id);
    }, Qt::QueuedConnection);
}

void RemoteCommandChannel::setHostKeyVerifier(const KeyStore::HostKeyVerifier &verifier)
{
    m_hostKeyVerifier = verifier;
}

void RemoteCommandChannel::doConnect()
{
    QString error;
    // Post-connect timeout stays 0 (the 2026-08-19 short-write lesson).
    m_ssh = sshConnectAndAuthenticate(m_config, &error, 0, m_hostKeyVerifier);
    if (!m_ssh) {
        emit errorOccurred(error);
        return;
    }
    emit connected(peerAddressOf(m_ssh));
}

bool RemoteCommandChannel::ensureConnected(QString *error)
{
    if (m_ssh && ssh_is_connected(m_ssh)) {
        return true;
    }
    sshDisconnectAndFree(m_ssh);
    m_ssh = sshConnectAndAuthenticate(m_config, error, 0, m_hostKeyVerifier);
    return m_ssh != nullptr;
}

void RemoteCommandChannel::doRun(const QString &id, const QString &command,
                                 int timeoutMs, bool streaming)
{
    QString connError;
    if (!ensureConnected(&connError)) {
        emit commandFinished(id, -1,
                             QStringLiteral("connection failed: %1").arg(connError));
        return;
    }

    ChannelGuard guard;
    guard.channel = ssh_channel_new(m_ssh);
    if (!guard.channel || ssh_channel_open_session(guard.channel) != SSH_OK) {
        emit commandFinished(id, -1, QStringLiteral("channel open failed"));
        return;
    }
    const QByteArray utf8 = command.toUtf8();
    if (ssh_channel_request_exec(guard.channel, utf8.constData()) != SSH_OK) {
        emit commandFinished(id, -1, QStringLiteral("exec request failed"));
        return;
    }

    m_cancelCurrent = false;
    QElapsedTimer elapsed;
    elapsed.start();
    QByteArray output;
    QByteArray stderrTail; // last 512 bytes of stderr for error context
    char buffer[8192];
    bool timedOut = false;

    for (;;) {
        int n = ssh_channel_read_nonblocking(guard.channel, buffer, sizeof(buffer), 0);
        while (n > 0) {
            output.append(buffer, n);
            n = ssh_channel_read_nonblocking(guard.channel, buffer, sizeof(buffer), 0);
        }
        if (n < 0) {
            break; // channel error: treat as end of stream
        }
        // Always drain stderr too — undrained packets wedge the channel
        // (the 2026-08 headless-exec hang lesson).
        int e = ssh_channel_read_nonblocking(guard.channel, buffer, sizeof(buffer), 1);
        while (e > 0) {
            stderrTail.append(buffer, e);
            if (stderrTail.size() > 512) {
                stderrTail.remove(0, stderrTail.size() - 512);
            }
            e = ssh_channel_read_nonblocking(guard.channel, buffer, sizeof(buffer), 1);
        }

        if (ssh_channel_is_eof(guard.channel) || ssh_channel_is_closed(guard.channel)) {
            break;
        }
        if (m_cancelCurrent || m_stopRequested) {
            break;
        }
        if (timeoutMs > 0 && elapsed.elapsed() > timeoutMs) {
            timedOut = true;
            break;
        }
        QThread::msleep(50);
    }

    const int exitCode = (timedOut || m_cancelCurrent)
        ? -1
        : ssh_channel_get_exit_status(guard.channel);

    if (!output.isEmpty()) {
        emit commandOutput(id, output);
    }

    QString error;
    if (m_cancelCurrent || m_stopRequested) {
        error = QStringLiteral("cancelled");
    } else if (timedOut) {
        error = QStringLiteral("timeout");
    } else if (exitCode != 0) {
        const QString tail = QString::fromUtf8(stderrTail).trimmed();
        error = QStringLiteral("exit %1: %2").arg(exitCode).arg(tail);
    }
    emit commandFinished(id, exitCode, error);
}

} // namespace hssh

#include "RemoteCommandChannel.moc"

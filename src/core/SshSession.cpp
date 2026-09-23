#include "SshSession.h"

#include "core/PortForward.h"
#include "core/SshConnect.h"
#include "utils/Config.h"
#include "utils/Crypto.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#ifdef HSSH_HAS_LIBSSH
#include <libssh/libssh.h>
#include <libssh/callbacks.h>
#endif

namespace hssh {

namespace {

#ifdef HSSH_HAS_LIBSSH

// Polls the shell channel for output. Every libssh call is guarded by the
// session mutex so it can run concurrently with keep-alive probes and port
// forwarding activity on the same session.
class SshShellReader : public QObject {
    Q_OBJECT

public:
    SshShellReader(ssh_channel channel, QMutex *sessionMutex,
                   QByteArray *writeQueue, QObject *parent = nullptr)
        : QObject(parent)
        , m_channel(channel)
        , m_mutex(sessionMutex)
        , m_writeQueue(writeQueue)
    {
    }

public slots:
    void start()
    {
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &SshShellReader::poll);
        m_timer->start(10);
    }

    void stop()
    {
        if (m_timer) {
            m_timer->stop();
        }
    }

    private slots:
    void poll()
    {
        if (!m_channel || !m_mutex) {
            return;
        }

        QMutexLocker locker(m_mutex);
        char buffer[4096];
        int n = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 0);
        if (n > 0) {
            emit dataReceived(QByteArray(buffer, n));
        }

        n = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 1);
        if (n > 0) {
            emit dataReceived(QByteArray(buffer, n));
        }

        if (n < 0) {
            // Transport error: treat as a dropped connection, not a shell exit.
            emit readError();
            stop();
            return;
        }

        // Drain pending shell input. The remote-window gate makes the
        // (blocking) ssh_channel_write call non-waiting: we only write as
        // many bytes as the peer's window currently accepts, so a congested
        // channel just retries on the next 10 ms tick instead of parking
        // this thread (or worse, the GUI thread) inside libssh.
        if (m_writeQueue && !m_writeQueue->isEmpty()) {
            const uint32_t window = ssh_channel_window_size(m_channel);
            if (window > 0) {
                const int chunk = qMin<qsizetype>(m_writeQueue->size(),
                                                   qMin<qint64>(window, 32 * 1024));
                const int written = ssh_channel_write(
                    m_channel, m_writeQueue->constData(), static_cast<uint32_t>(chunk));
                if (written > 0) {
                    m_writeQueue->remove(0, written);
                }
                // written <= 0: channel error — the read path above reports it.
            }
        }

        if (ssh_channel_is_eof(m_channel)) {
            const int exitCode = ssh_channel_get_exit_status(m_channel);
            emit finished(exitCode);
            stop();
        }
    }

    signals:
    void dataReceived(const QByteArray &data);
    void finished(int exitCode);
    void readError();

    private:
    ssh_channel m_channel = nullptr;
    QMutex *m_mutex = nullptr;
    QByteArray *m_writeQueue = nullptr;
    QTimer *m_timer = nullptr;
};

// Periodically writes an SSH_MSG_IGNORE message to keep NATs/firewalls from
// dropping the connection and to detect a dead transport. Runs on its own
// thread: ssh_send_ignore may block until the socket write timeout.
class SshKeepAliveProbe : public QObject {
    Q_OBJECT

public:
    SshKeepAliveProbe(QMutex *sessionMutex, int intervalMs, QObject *parent = nullptr)
        : QObject(parent)
        , m_mutex(sessionMutex)
        , m_intervalMs(intervalMs)
    {
    }

    void setSession(ssh_session session)
    {
        m_session = session;
    }

public slots:
    void start()
    {
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &SshKeepAliveProbe::probe);
        m_timer->start(m_intervalMs);
    }

    void stop()
    {
        if (m_timer) {
            m_timer->stop();
        }
        m_stopped = true;
    }

signals:
    void probeFailed();

private slots:
    void probe()
    {
        if (m_stopped || !m_session || !m_mutex) {
            return;
        }
        QMutexLocker locker(m_mutex);
        if (!ssh_is_connected(m_session)) {
            emit probeFailed();
            return;
        }
        const int rc = ssh_send_ignore(m_session, "hssh keepalive");
        if (rc != SSH_OK) {
            emit probeFailed();
        }
    }

private:
    ssh_session m_session = nullptr;
    QMutex *m_mutex = nullptr;
    int m_intervalMs = 30000;
    bool m_stopped = false;
    QTimer *m_timer = nullptr;
};

class LibSshImpl {
public:
    ssh_session session = nullptr;
    ssh_channel shellChannel = nullptr;
    SshShellReader *reader = nullptr;
    QThread readerThread;
    SshKeepAliveProbe *probe = nullptr;
    QThread probeThread;
    QMutex sessionMutex;
    // Pending shell input, drained by the reader thread (see writeShell):
    // writes happen on the reader thread with a remote-window check so a
    // congested channel (e.g. a parallel bulk transfer) can never block the
    // GUI thread on ssh_channel_write.
    QByteArray shellWriteQueue;
    SshSession::State state = SshSession::State::Disconnected;
    QString errorString;
    SessionConfig config;
    // Last requested shell size. setShellSize before the channel is up must
    // not be dropped: it is applied when the pty is requested and re-applied
    // right after the shell opens (covers resizes that arrived while
    // connecting).
    int pendingShellCols = -1;
    int pendingShellRows = -1;

    ~LibSshImpl()
    {
        cleanup();
    }

    // PH1-10: agent forwarding. The remote opens "auth-agent@openssh.com"
    // channels back to us; each gets a relay thread shuttling bytes between
    // the channel and the local agent (Windows OpenSSH named pipe, or
    // SSH_AUTH_SOCK on POSIX).
    struct AgentRelay {
        ssh_channel channel = nullptr;
#ifdef Q_OS_WIN
        void *pipe = nullptr; // HANDLE
#else
        int pipe = -1;
#endif
        std::thread thread;
    };

    ssh_channel startAgentRelay();
    ssh_callbacks_struct agentCallbacks{};

    void cleanup()
    {
        // Stop the workers before releasing libssh resources: the reader
        // thread may be inside a read and the probe inside ssh_send_ignore.
        if (probe) {
            probe->stop();
            probeThread.quit();
            probeThread.wait();
            probe->deleteLater();
            probe = nullptr;
        }
        if (reader) {
            reader->stop();
            readerThread.quit();
            readerThread.wait();
            reader->deleteLater();
            reader = nullptr;
        }
        if (shellChannel) {
            QMutexLocker locker(&sessionMutex);
            ssh_channel_close(shellChannel);
            ssh_channel_free(shellChannel);
            shellChannel = nullptr;
        }
        if (session) {
            QMutexLocker locker(&sessionMutex);
            ssh_disconnect(session);
            ssh_free(session);
            session = nullptr;
        }
    }

    void setState(SshSession::State newState)
    {
        if (state != newState) {
            state = newState;
        }
    }
};

#endif // HSSH_HAS_LIBSSH

#ifdef HSSH_HAS_LIBSSH

// Runs the blocking ssh_connect + authentication off the GUI thread so an
// unreachable host (connect timeout) does not freeze the window.
class SshConnectWorker : public QObject {
    Q_OBJECT

public:
    explicit SshConnectWorker(const SessionConfig &config,
                              KeyStore::HostKeyVerifier hostKeyVerifier,
                              KbdintPrompter kbdintPrompter)
        : m_config(config)
        , m_hostKeyVerifier(std::move(hostKeyVerifier))
        , m_kbdintPrompter(std::move(kbdintPrompter))
    {
    }

    ~SshConnectWorker() override
    {
        // Safety net for the case where the owning SshSession was destroyed
        // while the connect was still in flight and never adopted the session.
        if (session) {
            ssh_disconnect(session);
            ssh_free(session);
        }
    }

    // Owned by this worker until the GUI thread adopts it from the done slot.
    ssh_session session = nullptr;

    public slots:
    void run()
    {
        QString error;
        ssh_session s = sshConnectAndAuthenticate(m_config, &error, 0, m_hostKeyVerifier,
                                                  m_kbdintPrompter);
        if (!s) {
            emit done(SSH_ERROR, error);
            return;
        }
        session = s;
        emit done(SSH_AUTH_SUCCESS, QString());
    }

signals:
    void done(int rc, const QString &message);

private:
    SessionConfig m_config;
    KeyStore::HostKeyVerifier m_hostKeyVerifier;
    KbdintPrompter m_kbdintPrompter;
};

#endif // HSSH_HAS_LIBSSH

class QProcessImpl : public QObject {
    Q_OBJECT

public:
    explicit QProcessImpl(QObject *parent = nullptr)
        : QObject(parent)
        , process(new QProcess(this))
    {
        connect(process, &QProcess::readyReadStandardOutput, this, [this]() {
            emit dataReceived(process->readAllStandardOutput());
        });
        connect(process, &QProcess::readyReadStandardError, this, [this]() {
            emit dataReceived(process->readAllStandardError());
        });
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this, [this](int exitCode, QProcess::ExitStatus) {
                    emit finished(exitCode);
                });
        connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            emit errorOccurred(QStringLiteral("Process error: %1").arg(static_cast<int>(error)));
        });
    }

    void startExec(const SessionConfig &config, const QString &command)
    {
        const QString sshPath = findSshExecutable();
        QStringList args;
        args << QStringLiteral("-p") << QString::number(config.port());
        args << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=accept-new");
        args << QStringLiteral("-o") << QStringLiteral("BatchMode=no");
        if (!config.username().isEmpty()) {
            args << QStringLiteral("-l") << config.username();
        }
        if (!config.privateKeyPath().isEmpty()) {
            args << QStringLiteral("-i") << config.privateKeyPath();
        }
        args << config.host();
        args << command;

        process->start(sshPath, args);
    }

    void startShell(const SessionConfig &config)
    {
        const QString sshPath = findSshExecutable();
        QStringList args;
        args << QStringLiteral("-tt");
        args << QStringLiteral("-p") << QString::number(config.port());
        args << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=accept-new");
        if (!config.username().isEmpty()) {
            args << QStringLiteral("-l") << config.username();
        }
        if (!config.privateKeyPath().isEmpty()) {
            args << QStringLiteral("-i") << config.privateKeyPath();
        }
        args << config.host();

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
        process->setProcessEnvironment(env);

        process->start(sshPath, args);
    }

    void write(const QByteArray &data)
    {
        process->write(data);
    }

    void close()
    {
        process->close();
    }

    [[nodiscard]] bool isRunning() const
    {
        return process->state() != QProcess::NotRunning;
    }

signals:
    void dataReceived(const QByteArray &data);
    void finished(int exitCode);
    void errorOccurred(const QString &message);

private:
    static QString findSshExecutable()
    {
#ifdef Q_OS_WIN
        const QStringList candidates = {
            QStringLiteral("ssh.exe"),
            QStringLiteral("C:/Windows/System32/OpenSSH/ssh.exe"),
            QStringLiteral("C:/Program Files/OpenSSH/ssh.exe"),
        };
#else
        const QStringList candidates = {
            QStringLiteral("ssh"),
            QStringLiteral("/usr/bin/ssh"),
        };
#endif
        for (const QString &candidate : candidates) {
            if (QFile::exists(candidate) || !QStandardPaths::findExecutable(candidate).isEmpty()) {
                return candidate;
            }
        }
        return QStringLiteral("ssh");
    }

    QProcess *process = nullptr;
};

} // namespace

class SshSession::Impl {
public:
    SessionConfig config;
    State state = State::Disconnected;
    QString errorString;

#ifdef HSSH_HAS_LIBSSH
    std::unique_ptr<LibSshImpl> ssh;
    QThread *connectThread = nullptr;
    SshConnectWorker *connectWorker = nullptr;
    int reconnectAttempts = 0;
    QTimer *reconnectTimer = nullptr;
    bool userDisconnect = false;
    // PH2-12: optional interactive host-key verifier (GUI installs one).
    KeyStore::HostKeyVerifier hostKeyVerifier;
    // PH2-13: optional interactive 2FA (kbdint) prompter (GUI installs one).
    KbdintPrompter kbdintPrompter;
#else
    std::unique_ptr<QProcessImpl> process;
#endif
};

// ---------------------------------------------------------------------------
// PH1-10: ssh-agent forwarding relay.
// ---------------------------------------------------------------------------

#ifdef Q_OS_WIN
namespace {
// Connects to the Windows OpenSSH agent named pipe (the "ssh-agent"
// service must be running). Returns nullptr on failure.
void *openLocalAgentPipe()
{
    HANDLE h = CreateFileW(L"\\\\.\\pipe\\openssh-ssh-agent",
                           GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    return (h == INVALID_HANDLE_VALUE) ? nullptr : h;
}
} // namespace
#else
namespace {
int openLocalAgentPipe()
{
    const char *sock = getenv("SSH_AUTH_SOCK");
    if (!sock || !*sock) {
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}
} // namespace
#endif

// One relay thread per forwarded agent channel: shuttles bytes between the
// SSH channel (guarded by the session mutex — the shell reader shares the
// session) and the local agent pipe.
void agentRelayRun(LibSshImpl::AgentRelay *relay, QMutex *sessionMutex)
{
    char buffer[32 * 1024];
    bool alive = true;
    while (alive) {
        // Local agent pipe -> channel.
#ifdef Q_OS_WIN
        DWORD available = 0;
        HANDLE pipe = static_cast<HANDLE>(relay->pipe);
        if (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD n = 0;
            const DWORD want = available < sizeof(buffer) ? available
                                                          : static_cast<DWORD>(sizeof(buffer));
            if (ReadFile(pipe, buffer, want, &n, nullptr) && n > 0) {
                DWORD off = 0;
                while (off < n) {
                    QMutexLocker locker(sessionMutex);
                    const int w = ssh_channel_write(relay->channel, buffer + off, n - off);
                    if (w <= 0) {
                        alive = false;
                        break;
                    }
                    off += w;
                }
            } else {
                alive = false;
            }
        }
#else
        pollfd pfd{relay->pipe, POLLIN, 0};
        if (::poll(&pfd, 1, 5) > 0 && (pfd.revents & POLLIN)) {
            const ssize_t n = ::read(relay->pipe, buffer, sizeof(buffer));
            if (n > 0) {
                ssize_t off = 0;
                while (off < n) {
                    QMutexLocker locker(sessionMutex);
                    const int w = ssh_channel_write(relay->channel, buffer + off,
                                                    static_cast<uint32_t>(n - off));
                    if (w <= 0) {
                        alive = false;
                        break;
                    }
                    off += w;
                }
            } else {
                alive = false;
            }
        }
#endif
        if (!alive) {
            break;
        }
        // Channel -> local agent pipe (nonblocking drain under the lock).
        {
            QMutexLocker locker(sessionMutex);
            const int n = ssh_channel_read_nonblocking(relay->channel, buffer,
                                                       sizeof(buffer), 0);
            if (n < 0) {
                alive = false;
            } else if (n > 0) {
#ifdef Q_OS_WIN
                DWORD written = 0;
                if (!WriteFile(static_cast<HANDLE>(relay->pipe), buffer, n, &written, nullptr)
                    || written != static_cast<DWORD>(n)) {
                    alive = false;
                }
#else
                if (::write(relay->pipe, buffer, static_cast<size_t>(n)) != n) {
                    alive = false;
                }
#endif
            } else if (ssh_channel_is_eof(relay->channel)
                       || ssh_channel_is_closed(relay->channel)) {
                alive = false;
            }
        }
#ifdef Q_OS_WIN
        Sleep(5);
#else
        usleep(5000);
#endif
    }
#ifdef Q_OS_WIN
    CloseHandle(static_cast<HANDLE>(relay->pipe));
#else
    ::close(relay->pipe);
#endif
    {
        QMutexLocker locker(sessionMutex);
        ssh_channel_close(relay->channel);
        ssh_channel_free(relay->channel);
    }
    delete relay;
}

// Session callback: the remote opens an agent channel back to us.
ssh_channel agentChannelOpenCallback(ssh_session, void *user)
{
    auto *impl = static_cast<LibSshImpl *>(user);
    if (!impl) {
        return nullptr;
    }
    return impl->startAgentRelay();
}

ssh_channel LibSshImpl::startAgentRelay()
{
    void *pipeWin = nullptr;
    int pipeUnix = -1;
#ifdef Q_OS_WIN
    pipeWin = openLocalAgentPipe();
    if (!pipeWin) {
        return nullptr; // no local agent (service not running): refuse
    }
#else
    pipeUnix = openLocalAgentPipe();
    if (pipeUnix < 0) {
        return nullptr;
    }
#endif
    ssh_channel channel = ssh_channel_new(session);
    if (!channel) {
#ifdef Q_OS_WIN
        CloseHandle(static_cast<HANDLE>(pipeWin));
#else
        ::close(pipeUnix);
#endif
        return nullptr;
    }
    auto *relay = new AgentRelay();
    relay->channel = channel;
#ifdef Q_OS_WIN
    relay->pipe = pipeWin;
#else
    relay->pipe = pipeUnix;
#endif
    std::thread(agentRelayRun, relay, &sessionMutex).detach();
    return channel;
}

SshSession::SshSession(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
    , m_portForwards(new PortForwardManager(this, this))
{
#ifdef HSSH_HAS_LIBSSH
    d->ssh = std::make_unique<LibSshImpl>();
#else
    d->process = std::make_unique<QProcessImpl>(this);
    connect(d->process.get(), &QProcessImpl::dataReceived, this, [this](const QByteArray &data) {
        emit dataReceived(data);
    });
    connect(d->process.get(), &QProcessImpl::finished, this, [this](int exitCode) {
        emit execFinished(exitCode);
    });
    connect(d->process.get(), &QProcessImpl::errorOccurred, this, [this](const QString &message) {
        setError(message);
    });
#endif
}

SshSession::~SshSession()
{
#ifdef HSSH_HAS_LIBSSH
    clearReconnectTimer();
    if (d->connectThread) {
        // The worker may still be blocked in ssh_connect; stop result
        // delivery to this dying object and let the thread finish (and
        // clean itself up) in the background.
        if (d->connectWorker) {
            QObject::disconnect(d->connectWorker, &SshConnectWorker::done,
                                this, &SshSession::onConnectWorkerDone);
        }
        QObject::disconnect(d->connectThread, &QThread::finished, this, nullptr);
        d->connectThread->quit();
        if (!d->connectThread->wait(500)) {
            connect(d->connectThread, &QThread::finished,
                    d->connectThread, &QObject::deleteLater);
            d->connectThread->setParent(nullptr);
        }
        d->connectThread = nullptr;
        d->connectWorker = nullptr;
    }
#endif
}

void SshSession::setHostKeyVerifier(KeyStore::HostKeyVerifier verifier)
{
#ifdef HSSH_HAS_LIBSSH
    d->hostKeyVerifier = std::move(verifier);
#else
    Q_UNUSED(verifier)
#endif
}

void SshSession::setKbdintPrompter(KbdintPrompter prompter)
{
#ifdef HSSH_HAS_LIBSSH
    d->kbdintPrompter = std::move(prompter);
#else
    Q_UNUSED(prompter)
#endif
}

void SshSession::setSessionConfig(const SessionConfig &config)
{
    if (d->state != State::Disconnected) {
        disconnect();
    }
    d->config = config;
#ifdef HSSH_HAS_LIBSSH
    d->ssh->config = config;
#endif
}

SessionConfig SshSession::sessionConfig() const
{
    return d->config;
}

SshSession::State SshSession::state() const
{
    return d->state;
}

QString SshSession::errorString() const
{
    return d->errorString;
}

bool SshSession::isConnected() const
{
    return d->state == State::Connected;
}

void SshSession::setError(const QString &message)
{
    d->errorString = message;
    d->state = State::Error;
    emit stateChanged(State::Error);
    emit errorOccurred(message);
}

void SshSession::connectToHost()
{
    if (!d->config.isValid()) {
        setError(tr("Invalid session configuration"));
        return;
    }

#ifdef HSSH_HAS_LIBSSH
    connectToHostLibSsh();
#else
    // Fallback: open an interactive shell process. Exec will create separate processes.
    d->state = State::Connecting;
    emit stateChanged(State::Connecting);
    d->process->startShell(d->config);
    d->state = State::Connected;
    emit stateChanged(State::Connected);
    emit connected();
#endif
}

void SshSession::disconnect()
{
#ifdef HSSH_HAS_LIBSSH
    d->userDisconnect = true;
    clearReconnectTimer();
    m_portForwards->clear();
    if (d->ssh) {
        d->ssh->cleanup();
        d->ssh->setState(State::Disconnected);
    }
#else
    if (d->process) {
        d->process->close();
    }
#endif
    d->state = State::Disconnected;
    emit stateChanged(State::Disconnected);
    emit disconnected();
}

void SshSession::exec(const QString &command)
{
    if (!d->config.isValid()) {
        setError(tr("Invalid session configuration"));
        return;
    }

#ifdef HSSH_HAS_LIBSSH
    execLibSsh(command);
#else
    d->process->startExec(d->config, command);
#endif
}

void SshSession::writeShell(const QByteArray &data)
{
#ifdef HSSH_HAS_LIBSSH
    writeShellLibSsh(data);
#else
    if (d->process) {
        d->process->write(data);
    }
#endif
}

void SshSession::setShellSize(int columns, int rows)
{
#ifdef HSSH_HAS_LIBSSH
    setShellSizeLibSsh(columns, rows);
#else
    Q_UNUSED(columns)
    Q_UNUSED(rows)
#endif
}

#ifdef HSSH_HAS_LIBSSH

ssh_session SshSession::sessionHandle() const
{
    return d->ssh ? d->ssh->session : nullptr;
}

QMutex *SshSession::sessionMutex() const
{
    return d->ssh ? &d->ssh->sessionMutex : nullptr;
}

PortForwardManager *SshSession::portForwardManager() const
{
    return m_portForwards;
}

void SshSession::connectToHostLibSsh()
{
    d->userDisconnect = false;
    clearReconnectTimer();
    d->ssh->cleanup();

    d->state = State::Connecting;
    emit stateChanged(State::Connecting);

    // Connect + authenticate on a worker thread; both are blocking network
    // operations and would freeze the GUI for the duration of the timeout.
    // PH2-12: resolve the host-key policy on THIS (GUI) thread — reading
    // QSettings from a worker is not safe — then hand the decision lambda
    // to the worker.
    KeyStore::HostKeyVerifier verifier = d->hostKeyVerifier;
    if (!verifier) {
        const QString policy = Config::instance().stringValue(
            QStringLiteral("security/hostKeyPolicy"), QStringLiteral("accept-new"));
        verifier = [policy](const KeyStore::HostKeyInfo &, bool changed) {
            if (policy == QLatin1String("accept-all")) {
                return KeyStore::HostKeyDecision::Accept;
            }
            // accept-new / ask-without-UI: trust first use, reject changes.
            return changed ? KeyStore::HostKeyDecision::Reject
                           : KeyStore::HostKeyDecision::Accept;
        };
    }
    auto *worker = new SshConnectWorker(d->config, verifier, d->kbdintPrompter);
    auto *thread = new QThread(this);
    d->connectThread = thread;
    d->connectWorker = worker;
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &SshConnectWorker::run);
    connect(worker, &SshConnectWorker::done, this, &SshSession::onConnectWorkerDone);
    connect(worker, &SshConnectWorker::done, thread, &QThread::quit);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (d->connectThread == thread) {
            d->connectThread = nullptr;
            d->connectWorker = nullptr;
        }
        thread->deleteLater();
    });
    thread->start();
}

void SshSession::onConnectWorkerDone(int rc, const QString &message)
{
    auto *worker = qobject_cast<SshConnectWorker *>(sender());
    ssh_session session = worker ? worker->session : nullptr;
    if (worker) {
        worker->session = nullptr;
    }

    const auto discard = [session]() {
        if (session) {
            ssh_disconnect(session);
            ssh_free(session);
        }
    };

    // A newer connect attempt (or a disconnect) superseded this one.
    if (!worker || worker != d->connectWorker || d->state != State::Connecting) {
        discard();
        return;
    }
    d->connectWorker = nullptr;

    if (rc != SSH_AUTH_SUCCESS || !session) {
        discard();
        setError(message.isEmpty() ? tr("Authentication failed") : message);
        return;
    }

    d->ssh->session = session;
    // PH1-10: accept agent channels the remote opens back (forwarding only
    // does anything once the shell also sends the request below).
    {
        QMutexLocker locker(&d->ssh->sessionMutex);
        ssh_callbacks_init(&d->ssh->agentCallbacks);
        d->ssh->agentCallbacks.userdata = d->ssh.get();
        d->ssh->agentCallbacks.channel_open_request_auth_agent_function =
            agentChannelOpenCallback;
        ssh_set_callbacks(session, &d->ssh->agentCallbacks);
    }
    openShellChannelLibSsh();
}

void SshSession::openShellChannelLibSsh()
{
    const auto failAndCleanup = [this](const QString &message) {
        setError(message);
        d->ssh->cleanup();
        d->state = State::Disconnected;
        emit stateChanged(State::Disconnected);
    };

    d->ssh->shellChannel = ssh_channel_new(d->ssh->session);
    if (!d->ssh->shellChannel) {
        failAndCleanup(tr("Failed to create SSH channel"));
        return;
    }

    int     rc = ssh_channel_open_session(d->ssh->shellChannel);
    if (rc != SSH_OK) {
        failAndCleanup(QString::fromUtf8(ssh_get_error(d->ssh->session)));
        return;
    }

    // Use the size the widget last asked for instead of the 80x24 default;
    // pre-connection setShellSize calls are cached in pendingShellCols/Rows.
    const int ptyCols = d->ssh->pendingShellCols > 0 ? d->ssh->pendingShellCols : 80;
    const int ptyRows = d->ssh->pendingShellRows > 0 ? d->ssh->pendingShellRows : 24;
    rc = ssh_channel_request_pty_size(d->ssh->shellChannel, "xterm-256color", ptyCols, ptyRows);
    if (rc != SSH_OK) {
        failAndCleanup(QString::fromUtf8(ssh_get_error(d->ssh->session)));
        return;
    }

    rc = ssh_channel_request_shell(d->ssh->shellChannel);
    if (rc != SSH_OK) {
        failAndCleanup(QString::fromUtf8(ssh_get_error(d->ssh->session)));
        return;
    }

    // If a resize arrived while connecting and differs from what we just
    // requested, push the newest size now so the remote shell never lingers
    // at a stale geometry (wrapped readline redraws otherwise glue lines).
    if (d->ssh->pendingShellCols > 0 && d->ssh->pendingShellRows > 0
        && (d->ssh->pendingShellCols != ptyCols || d->ssh->pendingShellRows != ptyRows)) {
        ssh_channel_change_pty_size(d->ssh->shellChannel,
                                    d->ssh->pendingShellCols, d->ssh->pendingShellRows);
    }

    d->state = State::Connected;
    d->ssh->setState(State::Connected);
    emit stateChanged(State::Connected);
    emit connected();

    // PH1-10: ask the remote to forward our agent (best effort — a missing
    // local agent or an sshd without support is not an error).
    if (d->ssh->config.forwardAgent()) {
        QMutexLocker locker(&d->ssh->sessionMutex);
        ssh_channel_request_auth_agent(d->ssh->shellChannel);
    }

    d->reconnectAttempts = 0;
    startKeepAliveLibSsh();

    d->ssh->reader = new SshShellReader(d->ssh->shellChannel, &d->ssh->sessionMutex,
                                        &d->ssh->shellWriteQueue);
    d->ssh->reader->moveToThread(&d->ssh->readerThread);
    connect(d->ssh->reader, &SshShellReader::dataReceived, this, [this](const QByteArray &data) {
        emit dataReceived(data);
    });
    connect(d->ssh->reader, &SshShellReader::finished, this, [this](int exitCode) {
        emit execFinished(exitCode);
    });
    connect(d->ssh->reader, &SshShellReader::readError, this, &SshSession::onShellReaderFailed);
    connect(&d->ssh->readerThread, &QThread::started, d->ssh->reader, &SshShellReader::start);
    d->ssh->readerThread.start();
}

void SshSession::startKeepAliveLibSsh()
{
    const int intervalSeconds = d->config.keepAliveSeconds();
    if (intervalSeconds <= 0) {
        return;
    }

    d->ssh->probe = new SshKeepAliveProbe(&d->ssh->sessionMutex, intervalSeconds * 1000);
    d->ssh->probe->setSession(d->ssh->session);
    d->ssh->probe->moveToThread(&d->ssh->probeThread);
    connect(d->ssh->probe, &SshKeepAliveProbe::probeFailed, this, &SshSession::onKeepAliveFailed);
    connect(&d->ssh->probeThread, &QThread::started, d->ssh->probe, &SshKeepAliveProbe::start);
    d->ssh->probeThread.start();
}

void SshSession::stopKeepAliveLibSsh()
{
    if (d->ssh->probe) {
        d->ssh->probe->stop();
        d->ssh->probeThread.quit();
        d->ssh->probeThread.wait();
        d->ssh->probe->deleteLater();
        d->ssh->probe = nullptr;
    }
}

void SshSession::onKeepAliveFailed()
{
    onConnectionLost();
}

void SshSession::onShellReaderFailed()
{
    onConnectionLost();
}

void SshSession::onConnectionLost()
{
    if (d->state != State::Connected) {
        return;
    }

    qWarning() << "SSH connection lost:" << d->config.displayName();
    stopKeepAliveLibSsh();
    m_portForwards->clear();
    d->ssh->cleanup();
    d->state = State::Disconnected;
    emit stateChanged(State::Disconnected);
    emit connectionLost();

    if (d->config.autoReconnect() && !d->userDisconnect) {
        scheduleReconnect();
    }
}

void SshSession::scheduleReconnect()
{
    constexpr int kMaxAttempts = 3;
    if (d->reconnectAttempts >= kMaxAttempts) {
        setError(tr("Connection lost; reconnect attempts exhausted"));
        return;
    }

    ++d->reconnectAttempts;
    qWarning() << "Scheduling SSH reconnect, attempt" << d->reconnectAttempts;
    clearReconnectTimer();
    d->reconnectTimer = new QTimer(this);
    d->reconnectTimer->setSingleShot(true);
    d->reconnectTimer->setInterval(3000);
    connect(d->reconnectTimer, &QTimer::timeout, this, [this]() {
        d->reconnectTimer = nullptr;
        connectToHost();
    });
    d->reconnectTimer->start();
}

void SshSession::clearReconnectTimer()
{
    if (d->reconnectTimer) {
        d->reconnectTimer->stop();
        d->reconnectTimer->deleteLater();
        d->reconnectTimer = nullptr;
    }
}

void SshSession::execLibSsh(const QString &command)
{
    if (d->state != State::Connected || !d->ssh->session) {
        setError(tr("Not connected"));
        return;
    }

    // The whole command runs under the session mutex: the reader thread and
    // keep-alive probe must not touch the session while the channel is open.
    QMutexLocker locker(&d->ssh->sessionMutex);

    ssh_channel channel = ssh_channel_new(d->ssh->session);
    if (!channel) {
        setError(tr("Failed to create SSH channel"));
        return;
    }

    int rc = ssh_channel_open_session(channel);
    if (rc != SSH_OK) {
        ssh_channel_free(channel);
        setError(QString::fromUtf8(ssh_get_error(d->ssh->session)));
        return;
    }

    rc = ssh_channel_request_exec(channel, command.toUtf8().constData());
    if (rc != SSH_OK) {
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        setError(QString::fromUtf8(ssh_get_error(d->ssh->session)));
        return;
    }

    QByteArray output;
    char buffer[256];
    int nbytes = 0;
    while ((nbytes = ssh_channel_read(channel, buffer, sizeof(buffer), 0)) > 0) {
        output.append(buffer, nbytes);
    }

    while ((nbytes = ssh_channel_read(channel, buffer, sizeof(buffer), 1)) > 0) {
        output.append(buffer, nbytes);
    }

    ssh_channel_send_eof(channel);
    int exitCode = ssh_channel_get_exit_status(channel);
    ssh_channel_close(channel);
    ssh_channel_free(channel);

    if (!output.isEmpty()) {
        emit dataReceived(output);
    }
    emit execFinished(exitCode);
}

void SshSession::writeShellLibSsh(const QByteArray &data)
{
    if (d->state != State::Connected || !d->ssh->shellChannel) {
        return;
    }
    // Queue only — NEVER call ssh_channel_write from the caller's thread:
    // with a congested channel window the blocking write would freeze the
    // GUI thread for as long as the peer's window stays full (observed as a
    // 30 s+ agent freeze during a parallel bulk download, 2026-09-18).
    // The reader thread drains the queue with a remote-window gate.
    QMutexLocker locker(&d->ssh->sessionMutex);
    d->ssh->shellWriteQueue.append(data);
}

void SshSession::setShellSizeLibSsh(int columns, int rows)
{
    if (columns <= 0 || rows <= 0) {
        return;
    }
    // Always cache the newest request: pre-connection calls used to be
    // silently dropped, leaving the remote shell at 80x24 forever.
    d->ssh->pendingShellCols = columns;
    d->ssh->pendingShellRows = rows;

    if (d->state != State::Connected || !d->ssh->shellChannel) {
        return;
    }
    QMutexLocker locker(&d->ssh->sessionMutex);
    ssh_channel_change_pty_size(d->ssh->shellChannel, columns, rows);
}

#endif // HSSH_HAS_LIBSSH

} // namespace hssh

#include "SshSession.moc"

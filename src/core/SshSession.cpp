#include "SshSession.h"

#include "core/SshConnect.h"
#include "utils/Crypto.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#ifdef HSSH_HAS_LIBSSH
#include <libssh/libssh.h>
#endif

namespace hssh {

namespace {

#ifdef HSSH_HAS_LIBSSH

class SshShellReader : public QObject {
    Q_OBJECT

public:
    explicit SshShellReader(ssh_channel channel, QObject *parent = nullptr)
        : QObject(parent)
        , m_channel(channel)
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
        if (!m_channel)
            return;

        char buffer[4096];
        int n = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 0);
        if (n > 0) {
            emit dataReceived(QByteArray(buffer, n));
        }

        n = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 1);
        if (n > 0) {
            emit dataReceived(QByteArray(buffer, n));
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

private:
    ssh_channel m_channel = nullptr;
    QTimer *m_timer = nullptr;
};

class LibSshImpl {
public:
    ssh_session session = nullptr;
    ssh_channel shellChannel = nullptr;
    SshShellReader *reader = nullptr;
    QThread readerThread;
    SshSession::State state = SshSession::State::Disconnected;
    QString errorString;
    SessionConfig config;

    ~LibSshImpl()
    {
        cleanup();
    }

    void cleanup()
    {
        if (reader) {
            reader->stop();
            readerThread.quit();
            readerThread.wait();
            reader->deleteLater();
            reader = nullptr;
        }
        if (shellChannel) {
            ssh_channel_close(shellChannel);
            ssh_channel_free(shellChannel);
            shellChannel = nullptr;
        }
        if (session) {
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
    explicit SshConnectWorker(const SessionConfig &config)
        : m_config(config)
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
        ssh_session s = sshConnectAndAuthenticate(m_config, &error);
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
#else
    std::unique_ptr<QProcessImpl> process;
#endif
};

SshSession::SshSession(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
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

void SshSession::connectToHostLibSsh()
{
    d->ssh->cleanup();

    d->state = State::Connecting;
    emit stateChanged(State::Connecting);

    // Connect + authenticate on a worker thread; both are blocking network
    // operations and would freeze the GUI for the duration of the timeout.
    auto *worker = new SshConnectWorker(d->config);
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
    openShellChannelLibSsh();
}

void SshSession::openShellChannelLibSsh()
{
    d->ssh->shellChannel = ssh_channel_new(d->ssh->session);
    if (!d->ssh->shellChannel) {
        setError(tr("Failed to create SSH channel"));
        return;
    }

    int rc = ssh_channel_open_session(d->ssh->shellChannel);
    if (rc != SSH_OK) {
        setError(QString::fromUtf8(ssh_get_error(d->ssh->session)));
        return;
    }

    rc = ssh_channel_request_pty_size(d->ssh->shellChannel, "xterm-256color", 80, 24);
    if (rc != SSH_OK) {
        setError(QString::fromUtf8(ssh_get_error(d->ssh->session)));
        return;
    }

    rc = ssh_channel_request_shell(d->ssh->shellChannel);
    if (rc != SSH_OK) {
        setError(QString::fromUtf8(ssh_get_error(d->ssh->session)));
        return;
    }

    d->state = State::Connected;
    d->ssh->setState(State::Connected);
    emit stateChanged(State::Connected);
    emit connected();

    d->ssh->reader = new SshShellReader(d->ssh->shellChannel);
    d->ssh->reader->moveToThread(&d->ssh->readerThread);
    connect(d->ssh->reader, &SshShellReader::dataReceived, this, [this](const QByteArray &data) {
        emit dataReceived(data);
    });
    connect(d->ssh->reader, &SshShellReader::finished, this, [this](int exitCode) {
        emit execFinished(exitCode);
    });
    connect(&d->ssh->readerThread, &QThread::started, d->ssh->reader, &SshShellReader::start);
    d->ssh->readerThread.start();
}

void SshSession::execLibSsh(const QString &command)
{
    if (d->state != State::Connected) {
        setError(tr("Not connected"));
        return;
    }

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
    ssh_channel_write(d->ssh->shellChannel, data.constData(), static_cast<uint32_t>(data.size()));
}

void SshSession::setShellSizeLibSsh(int columns, int rows)
{
    if (d->state != State::Connected || !d->ssh->shellChannel) {
        return;
    }
    ssh_channel_change_pty_size(d->ssh->shellChannel, columns, rows);
}

#endif // HSSH_HAS_LIBSSH

} // namespace hssh

#include "SshSession.moc"

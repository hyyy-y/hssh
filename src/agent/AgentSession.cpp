#include "AgentSession.h"

#include "core/SshConnect.h"

#include <QThread>

#include <atomic>

#ifdef HSSH_HAS_LIBSSH
#include <libssh/libssh.h>
#endif

namespace hssh {

namespace {

#ifdef HSSH_HAS_LIBSSH

class AgentConnectWorker : public QObject {
    Q_OBJECT

public:
    explicit AgentConnectWorker(const SessionConfig &config)
        : m_config(config)
    {
    }

    ~AgentConnectWorker() override
    {
        if (session) {
            ssh_disconnect(static_cast<ssh_session>(session));
            ssh_free(static_cast<ssh_session>(session));
        }
    }

    // Raw ssh_session passed as void* through the queued signal; owned by
    // this worker until the GUI thread adopts it.
    void *session = nullptr;

public slots:
    void run()
    {
        QString error;
        ssh_session s = sshConnectAndAuthenticate(m_config, &error);
        if (!s) {
            emit done(nullptr, error);
            return;
        }
        session = s;
        emit done(s, QString());
    }

signals:
    void done(void *session, const QString &error);

private:
    SessionConfig m_config;
};

class AgentExecWorker : public QObject {
    Q_OBJECT

public:
    AgentExecWorker(void *session, const QString &requestId, const QString &command)
        : m_session(static_cast<ssh_session>(session))
        , m_requestId(requestId)
        , m_command(command)
    {
    }

    void requestStop()
    {
        m_stop = true;
    }

public slots:
    void run()
    {
        QByteArray output;
        QString error;
        int exitCode = -1;

        ssh_channel channel = ssh_channel_new(m_session);
        if (!channel) {
            error = QStringLiteral("Failed to create SSH channel");
            emit done(m_requestId, output, exitCode, error);
            return;
        }

        int rc = ssh_channel_open_session(channel);
        if (rc == SSH_OK) {
            rc = ssh_channel_request_exec(channel, m_command.toUtf8().constData());
        }
        if (rc != SSH_OK) {
            error = QString::fromUtf8(ssh_get_error(m_session));
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            emit done(m_requestId, output, exitCode, error);
            return;
        }

        char buffer[4096];
        while (!m_stop) {
            const int n = ssh_channel_read_nonblocking(channel, buffer, sizeof(buffer), 0);
            if (n > 0) {
                output.append(buffer, n);
                continue;
            }
            if (n == SSH_ERROR) {
                error = QString::fromUtf8(ssh_get_error(m_session));
                break;
            }
            if (ssh_channel_is_eof(channel)) {
                break;
            }
            QThread::msleep(20);
        }

        exitCode = ssh_channel_get_exit_status(channel);
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        emit done(m_requestId, output, exitCode, error);
    }

signals:
    void done(const QString &requestId, const QByteArray &output, int exitCode, const QString &error);

private:
    ssh_session m_session = nullptr;
    QString m_requestId;
    QString m_command;
    std::atomic<bool> m_stop{false};
};

#endif // HSSH_HAS_LIBSSH

} // namespace

class AgentSession::Impl {
public:
    QString id;
    SessionConfig config;
#ifdef HSSH_HAS_LIBSSH
    ssh_session session = nullptr;
    QThread *connectThread = nullptr;
    QThread *execThread = nullptr;
    AgentConnectWorker *connectWorker = nullptr;
    AgentExecWorker *execWorker = nullptr;
    bool execBusy = false;
#endif
    bool connected = false;
    bool closing = false;
};

AgentSession::AgentSession(const QString &id, const SessionConfig &config, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    d->id = id;
    d->config = config;
}

AgentSession::~AgentSession()
{
    close();
}

QString AgentSession::id() const
{
    return d->id;
}

SessionConfig AgentSession::config() const
{
    return d->config;
}

bool AgentSession::isConnected() const
{
    return d->connected;
}

void AgentSession::connectAsync()
{
#ifdef HSSH_HAS_LIBSSH
    if (d->closing || d->connectWorker || (d->connectThread && d->connectThread->isRunning())) {
        return;
    }

    auto *worker = new AgentConnectWorker(d->config);
    auto *thread = new QThread(this);
    d->connectWorker = worker;
    d->connectThread = thread;
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &AgentConnectWorker::run);
    connect(worker, &AgentConnectWorker::done, this, [this](void *rawSession, const QString &error) {
        if (!d->connectWorker) {
            // close() already discarded the worker.
            return;
        }
        d->connectWorker = nullptr;
        if (!rawSession) {
            emit connectFailed(d->id, error);
            return;
        }
        // Adopt the session: the worker's destructor frees any session it
        // still owns, so it must be detached before deleteLater runs.
        if (auto *w = qobject_cast<AgentConnectWorker *>(sender())) {
            w->session = nullptr;
        }
        d->session = static_cast<ssh_session>(rawSession);
        d->connected = true;
        emit connected(d->id);
    });
    connect(worker, &AgentConnectWorker::done, thread, &QThread::quit);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    thread->start();
#else
    emit connectFailed(d->id, QStringLiteral("libssh backend not available"));
#endif
}

void AgentSession::execAsync(const QString &requestId, const QString &command)
{
#ifdef HSSH_HAS_LIBSSH
    if (!d->connected || !d->session) {
        emit execFinished(requestId, QString(), -1, QStringLiteral("Not connected"));
        return;
    }
    if (d->execBusy || d->execWorker) {
        emit execFinished(requestId, QString(), -1, QStringLiteral("Another exec is running on this session"));
        return;
    }
    d->execBusy = true;

    auto *worker = new AgentExecWorker(d->session, requestId, command);
    auto *thread = new QThread(this);
    d->execWorker = worker;
    d->execThread = thread;
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &AgentExecWorker::run);
    connect(worker, &AgentExecWorker::done, this,
            [this](const QString &doneRequestId, const QByteArray &output, int exitCode, const QString &error) {
                d->execBusy = false;
                if (!d->closing) {
                    emit execFinished(doneRequestId, QString::fromUtf8(output), exitCode, error);
                }
            });
    connect(worker, &AgentExecWorker::done, thread, &QThread::quit);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, worker, thread]() {
        if (d->execWorker == worker) {
            d->execWorker = nullptr;
        }
        if (d->execThread == thread) {
            d->execThread = nullptr;
        }
    });
    thread->start();
#else
    emit execFinished(requestId, QString(), -1, QStringLiteral("libssh backend not available"));
#endif
}

void AgentSession::close()
{
    if (d->closing) {
        return;
    }
    d->closing = true;

#ifdef HSSH_HAS_LIBSSH
    // Stop a running exec first. If the worker is stuck inside libssh (dead
    // network), detach the thread and intentionally leak the session instead
    // of terminating the thread mid-call and freeing the session (that
    // crashes).
    if (d->execWorker) {
        d->execWorker->requestStop();
    }
    if (d->execThread && d->execThread->isRunning()) {
        if (!d->execThread->wait(10000)) {
            qWarning() << "AgentSession: exec worker stuck, detaching thread and leaking session";
            QThread *thread = d->execThread;
            d->execThread = nullptr;
            d->execWorker = nullptr;
            d->session = nullptr; // worker may still be using it; leaked on purpose
            // The thread object must outlive this AgentSession: unparent it
            // so ~QObject does not destroy a still-running QThread (UB).
            thread->setParent(nullptr);
            connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        }
    }
    if (d->execThread) {
        d->execThread->quit();
        d->execThread->wait();
        d->execThread->deleteLater();
        d->execThread = nullptr;
    }
    d->execWorker = nullptr;

    if (d->connectThread && d->connectThread->isRunning()) {
        if (!d->connectThread->wait(12000)) {
            qWarning() << "AgentSession: connect worker stuck, detaching thread";
            QThread *thread = d->connectThread;
            d->connectThread = nullptr;
            d->connectWorker = nullptr;
            thread->setParent(nullptr);
            connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        }
    }
    if (d->connectThread) {
        d->connectThread->quit();
        d->connectThread->wait();
        d->connectThread->deleteLater();
        d->connectThread = nullptr;
    }
    d->connectWorker = nullptr;

    if (d->session) {
        ssh_disconnect(d->session);
        ssh_free(d->session);
        d->session = nullptr;
    }
#endif
    d->connected = false;
    emit closed(d->id);
}

} // namespace hssh

#include "AgentSession.moc"

#include "PortForward.h"

#include "core/SshSession.h"

#include <QCoreApplication>
#include <QMutex>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>

#ifdef HSSH_HAS_LIBSSH
#include <libssh/libssh.h>
#endif

namespace hssh {

QString ForwardSpec::description() const
{
    const QString bind = QStringLiteral("%1:%2").arg(bindAddress).arg(bindPort);
    switch (type) {
    case Type::Local:
        return QCoreApplication::translate("hssh::ForwardSpec", "Local  %1 -> %2:%3")
            .arg(bind, targetHost)
            .arg(targetPort);
    case Type::Remote:
        return QCoreApplication::translate("hssh::ForwardSpec", "Remote %1 -> %2:%3")
            .arg(bind, targetHost)
            .arg(targetPort);
    case Type::Dynamic:
        return QCoreApplication::translate("hssh::ForwardSpec", "SOCKS5 %1 (dynamic)").arg(bind);
    }
    return bind;
}

namespace {

#ifdef HSSH_HAS_LIBSSH

// Pumps data between a QTcpSocket and an SSH channel. Used for local
// forwards and (with a SOCKS5 handshake first) dynamic forwards.
class ForwardConnection : public QObject {
    Q_OBJECT

public:
    ForwardConnection(QTcpSocket *socket, ssh_session session, QMutex *mutex,
                      bool dynamic, QObject *parent = nullptr)
        : QObject(parent)
        , m_socket(socket)
        , m_session(session)
        , m_mutex(mutex)
        , m_dynamic(dynamic)
    {
        m_socket->setParent(this);
        connect(m_socket, &QTcpSocket::readyRead, this, &ForwardConnection::onSocketReadyRead);
        connect(m_socket, &QTcpSocket::disconnected, this, &ForwardConnection::cleanup);
        connect(m_socket, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            cleanup();
        });
    }

public slots:
    void start()
    {
        if (m_dynamic) {
            // Wait for the SOCKS5 greeting before opening the channel.
            return;
        }
        openChannel(m_targetHost, m_targetPort);
    }

    void poll()
    {
        if (m_cleanedUp || !m_channel || !m_socket || !m_mutex) {
            return;
        }
        QMutexLocker locker(m_mutex);
        char buffer[16384];
        const int n = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 0);
        if (n > 0) {
            m_socket->write(QByteArray(buffer, n));
        } else if (n == SSH_ERROR) {
            cleanup();
        }
    }

    void setTarget(const QString &host, quint16 port)
    {
        m_targetHost = host;
        m_targetPort = port;
    }

public slots:
    void cleanup()
    {
        if (m_cleanedUp) {
            return;
        }
        m_cleanedUp = true;
        if (m_socket) {
            m_socket->disconnectFromHost();
        }
        if (m_channel && m_mutex) {
            QMutexLocker locker(m_mutex);
            ssh_channel_close(m_channel);
            ssh_channel_free(m_channel);
            m_channel = nullptr;
        }
        emit finished();
    }

signals:
    void finished();

private slots:
    void onSocketReadyRead()
    {
        if (m_cleanedUp) {
            return;
        }
        if (m_dynamic && !m_handshaken) {
            handleSocksHandshake();
            return;
        }
        if (!m_channel || !m_mutex) {
            return;
        }
        const QByteArray data = m_socket->readAll();
        if (data.isEmpty()) {
            return;
        }
        QMutexLocker locker(m_mutex);
        const int n = ssh_channel_write(m_channel, data.constData(), static_cast<uint32_t>(data.size()));
        if (n == SSH_ERROR) {
            cleanup();
        }
    }

private:
    bool openChannel(const QString &host, quint16 port)
    {
        if (m_cleanedUp || m_channel || !m_session || !m_mutex) {
            return false;
        }
        QMutexLocker locker(m_mutex);
        m_channel = ssh_channel_new(m_session);
        if (!m_channel) {
            cleanup();
            return false;
        }
        const QByteArray hostBytes = host.toUtf8();
        const int rc = ssh_channel_open_forward(m_channel, hostBytes.constData(), port,
                                                "localhost", 0);
        if (rc != SSH_OK) {
            ssh_channel_free(m_channel);
            m_channel = nullptr;
            cleanup();
            return false;
        }
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &ForwardConnection::poll);
        m_timer->start(10);
        return true;
    }

    void handleSocksHandshake()
    {
        if (!m_socksGreeted) {
            //  VER(1) NMETHODS(1) METHODS(N)
            if (m_socksBuffer.size() < 2) {
                return;
            }
            if (static_cast<quint8>(m_socksBuffer.at(0)) != 0x05) {
                m_socket->write(QByteArray::fromHex(QStringLiteral("05ff").toUtf8()));
                cleanup();
                return;
            }
            const int methods = static_cast<quint8>(m_socksBuffer.at(1));
            if (m_socksBuffer.size() < 2 + methods) {
                return;
            }
            // No authentication.
            m_socket->write(QByteArray::fromHex(QStringLiteral("0500").toUtf8()));
            m_socket->flush();
            m_socksBuffer = m_socksBuffer.mid(2 + methods);
            m_socksGreeted = true;
        }

        // Request: VER(1) CMD(1) RSV(1) ATYP(1) ADDR(1/4/16) PORT(2)
        const QByteArray &req = m_socksBuffer;
        if (req.size() < 4) {
            return;
        }
        const quint8 cmd = static_cast<quint8>(req.at(1));
        const quint8 atyp = static_cast<quint8>(req.at(3));
        int addrLen = 0;
        if (atyp == 0x01) {        // IPv4
            addrLen = 4;
        } else if (atyp == 0x03) { // Domain name
            addrLen = 1 + static_cast<quint8>(req.at(4));
        } else if (atyp == 0x04) { // IPv6
            addrLen = 16;
        } else {
            cleanup();
            return;
        }
        if (req.size() < 4 + addrLen + 2) {
            return;
        }

        if (cmd != 0x01) { // CONNECT only
            m_socket->write(QByteArray::fromHex(QStringLiteral("050700000000000000").toUtf8()));
            cleanup();
            return;
        }

        QString host;
        if (atyp == 0x03) {
            host = QString::fromUtf8(req.constData() + 5, addrLen - 1);
        } else if (atyp == 0x01) {
            const auto byte = [&req](int offset) {
                return static_cast<quint8>(req.at(offset));
            };
            host = QStringLiteral("%1.%2.%3.%4").arg(byte(4)).arg(byte(5)).arg(byte(6)).arg(byte(7));
        } else {
            QStringList groups;
            for (int i = 0; i < 16; i += 2) {
                groups.append(QStringLiteral("%1%2")
                                  .arg(static_cast<quint8>(req.at(4 + i)), 2, 16, QLatin1Char('0'))
                                  .arg(static_cast<quint8>(req.at(5 + i)), 2, 16, QLatin1Char('0')));
            }
            host = groups.join(QLatin1Char(':'));
        }
        const quint16 port = static_cast<quint16>(
            (static_cast<quint8>(req.at(4 + addrLen)) << 8)
            | static_cast<quint8>(req.at(4 + addrLen + 1)));
        if (port == 0 || host.isEmpty()) {
            cleanup();
            return;
        }

        m_handshaken = true;
        m_socksBuffer.clear();
        if (!openChannel(host, port)) {
            m_socket->write(QByteArray::fromHex(QStringLiteral("050100000000000000").toUtf8()));
            return;
        }
        m_socket->write(QByteArray::fromHex(QStringLiteral("050000000000000000").toUtf8()));
    }

    QTcpSocket *m_socket = nullptr;
    ssh_session m_session = nullptr;
    QMutex *m_mutex = nullptr;
    ssh_channel m_channel = nullptr;
    bool m_dynamic = false;
    bool m_socksGreeted = false;
    bool m_handshaken = false;
    bool m_cleanedUp = false;
    QString m_targetHost;
    quint16 m_targetPort = 0;
    QTimer *m_timer = nullptr;
    QByteArray m_socksBuffer;
};

// Owns every listener and connection for one session; lives on the forward
// thread so sockets and SSH channels are never touched from the GUI thread.
class ForwardWorker : public QObject {
    Q_OBJECT

public:
    ForwardWorker(ssh_session session, QMutex *mutex, QObject *parent = nullptr)
        : QObject(parent)
        , m_session(session)
        , m_mutex(mutex)
    {
    }

public slots:
    void addForward(int index, const ForwardSpec &spec, QString *errorMessage)
    {
        if (!m_session || !m_mutex) {
            const QString error = QStringLiteral("SSH session is not connected");
            if (errorMessage) {
                *errorMessage = error;
            }
            emit forwardResult(index, false, error);
            return;
        }

        if (spec.type == ForwardSpec::Type::Remote) {
            addRemoteForward(index, spec, errorMessage);
            return;
        }

        auto *server = new QTcpServer(this);
        if (!server->listen(QHostAddress(spec.bindAddress), spec.bindPort)) {
            const QString error = server->errorString();
            server->deleteLater();
            if (errorMessage) {
                *errorMessage = error;
            }
            emit forwardResult(index, false, error);
            return;
        }

        m_servers[index] = server;
        connect(server, &QTcpServer::newConnection, this, [this, index, spec]() {
            QTcpServer *server = m_servers.value(index);
            if (!server) {
                return;
            }
            while (QTcpSocket *socket = server->nextPendingConnection()) {
                auto *conn = new ForwardConnection(socket, m_session, m_mutex,
                                                   spec.type == ForwardSpec::Type::Dynamic,
                                                   this);
                if (spec.type == ForwardSpec::Type::Local) {
                    conn->setTarget(spec.targetHost, spec.targetPort);
                }
                m_connections.append(conn);
                connect(conn, &ForwardConnection::finished, this, [this, conn]() {
                    m_connections.removeAll(conn);
                    conn->deleteLater();
                });
                conn->start();
            }
        });
        emit forwardResult(index, true, QString());
    }

    void removeForward(int index)
    {
        if (auto *server = m_servers.take(index)) {
            server->close();
            server->deleteLater();
        }
        if (auto *poller = m_remotePollers.take(index)) {
            poller->stop();
            poller->deleteLater();
        }
        const auto it = m_listens.find(index);
        if (it != m_listens.end()) {
            QMutexLocker locker(m_mutex);
            ssh_channel_cancel_forward(m_session, it->first.constData(), it->second);
            m_listens.erase(it);
        }
    }

    void stop()
    {
        const auto indexes = m_servers.keys() + m_remotePollers.keys();
        for (int index : indexes) {
            removeForward(index);
        }
        // cleanup() emits finished() which synchronously mutates
        // m_connections, so iterate over a copy instead.
        const QList<QPointer<ForwardConnection>> connections = m_connections;
        m_connections.clear();
        for (const QPointer<ForwardConnection> &conn : connections) {
            if (conn) {
                conn->cleanup();
            }
        }
    }

signals:
    void forwardResult(int index, bool ok, const QString &message);

private:
    void addRemoteForward(int index, const ForwardSpec &spec, QString *errorMessage)
    {
        QMutexLocker locker(m_mutex);
        const QByteArray address = spec.bindAddress.toUtf8();
        const int rc = ssh_channel_listen_forward(m_session, address.constData(),
                                                  spec.bindPort, nullptr);
        if (rc != SSH_OK) {
            const QString error = QString::fromUtf8(ssh_get_error(m_session));
            if (errorMessage) {
                *errorMessage = error;
            }
            emit forwardResult(index, false, error);
            return;
        }
        m_listens[index] = {address, static_cast<int>(spec.bindPort)};
        locker.unlock();

        auto *poller = new QTimer(this);
        poller->setInterval(500);
        m_remotePollers[index] = poller;
        connect(poller, &QTimer::timeout, this, [this, spec]() {
            ssh_channel channel = nullptr;
            {
                QMutexLocker locker(m_mutex);
                if (!m_session) {
                    return;
                }
                channel = ssh_channel_accept_forward(m_session, 100, nullptr);
            }
            if (!channel) {
                return;
            }
            auto *socket = new QTcpSocket();
            socket->connectToHost(spec.targetHost, spec.targetPort);
            if (!socket->waitForConnected(5000)) {
                QMutexLocker locker(m_mutex);
                ssh_channel_close(channel);
                ssh_channel_free(channel);
                socket->deleteLater();
                return;
            }
            auto *conn = new ForwardConnection(socket, m_session, m_mutex, false, this);
            conn->setTarget(spec.targetHost, spec.targetPort);
            conn->start();
            m_connections.append(conn);
            connect(conn, &ForwardConnection::finished, this, [this, conn]() {
                m_connections.removeAll(conn);
                conn->deleteLater();
            });
        });
        poller->start();
        emit forwardResult(index, true, QString());
    }

    ssh_session m_session = nullptr;
    QMutex *m_mutex = nullptr;
    QHash<int, QTcpServer *> m_servers;
    QHash<int, QTimer *> m_remotePollers;
    QHash<int, QPair<QByteArray, int>> m_listens;
    QList<QPointer<ForwardConnection>> m_connections;
};

#endif // HSSH_HAS_LIBSSH

} // namespace

class PortForwardManager::Impl {
public:
    explicit Impl(SshSession *session)
        : session(session)
    {
    }

    SshSession *session = nullptr;
    QList<ForwardSpec> specs;
    QList<bool> active;
    QList<QString> status;
#ifdef HSSH_HAS_LIBSSH
    QThread thread;
    ForwardWorker *worker = nullptr;
    bool threadStarted = false;
#endif
};

PortForwardManager::PortForwardManager(SshSession *session, QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>(session))
{
#ifdef HSSH_HAS_LIBSSH
    d->thread.setObjectName(QStringLiteral("hssh-portforward"));
    d->worker = new ForwardWorker(session ? session->sessionHandle() : nullptr,
                                  session ? session->sessionMutex() : nullptr);
    d->worker->moveToThread(&d->thread);
    connect(d->worker, &ForwardWorker::forwardResult, this,
            [this](int index, bool ok, const QString &message) {
                if (index >= 0 && index < d->active.size()) {
                    d->active[index] = ok;
                    d->status[index] = ok ? tr("active") : (message.isEmpty() ? tr("failed") : message);
                    emit forwardsChanged();
                }
            });
#endif
}

PortForwardManager::~PortForwardManager()
{
#ifdef HSSH_HAS_LIBSSH
    if (d->threadStarted) {
        QMetaObject::invokeMethod(d->worker, &ForwardWorker::stop, Qt::BlockingQueuedConnection);
        d->thread.quit();
        d->thread.wait();
        delete d->worker;
        d->worker = nullptr;
    }
#endif
}

bool PortForwardManager::addForward(const ForwardSpec &spec, QString *errorMessage)
{
    if (spec.bindPort == 0) {
        if (errorMessage) {
            *errorMessage = tr("Bind port must not be 0");
        }
        return false;
    }
    if ((spec.type == ForwardSpec::Type::Local || spec.type == ForwardSpec::Type::Remote)
        && (spec.targetHost.isEmpty() || spec.targetPort == 0)) {
        if (errorMessage) {
            *errorMessage = tr("Target host and port are required");
        }
        return false;
    }

    const int index = d->specs.size();
    d->specs.append(spec);
    d->active.append(false);
    d->status.append(tr("starting"));

#ifdef HSSH_HAS_LIBSSH
    if (!d->threadStarted) {
        d->thread.start();
        d->threadStarted = true;
    }
    QString error;
    QMetaObject::invokeMethod(d->worker, [this, index, spec, &error]() {
        d->worker->addForward(index, spec, &error);
    }, Qt::BlockingQueuedConnection);
    if (!error.isEmpty() && errorMessage) {
        *errorMessage = error;
    }
    d->active[index] = error.isEmpty();
    d->status[index] = error.isEmpty() ? tr("active") : error;
#else
    if (errorMessage) {
        *errorMessage = tr("Port forwarding requires the libssh backend");
    }
    d->active[index] = false;
    d->status[index] = tr("unsupported");
#endif

    emit forwardsChanged();
    return d->active[index];
}

void PortForwardManager::removeForward(int index)
{
    if (index < 0 || index >= d->specs.size()) {
        return;
    }
#ifdef HSSH_HAS_LIBSSH
    if (d->threadStarted) {
        QMetaObject::invokeMethod(d->worker, [this, index]() {
            d->worker->removeForward(index);
        }, Qt::BlockingQueuedConnection);
    }
#endif
    d->specs.removeAt(index);
    d->active.removeAt(index);
    d->status.removeAt(index);
    emit forwardsChanged();
}

void PortForwardManager::clear()
{
    for (int i = d->specs.size() - 1; i >= 0; --i) {
        removeForward(i);
    }
}

QList<ForwardSpec> PortForwardManager::forwards() const
{
    return d->specs;
}

bool PortForwardManager::isActive(int index) const
{
    return index >= 0 && index < d->active.size() && d->active.at(index);
}

QString PortForwardManager::statusText(int index) const
{
    return index >= 0 && index < d->status.size() ? d->status.at(index) : QString();
}

int PortForwardManager::count() const
{
    return d->specs.size();
}

} // namespace hssh

#include "PortForward.moc"

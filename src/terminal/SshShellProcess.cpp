#include "SshShellProcess.h"

#include "core/ConnectionManager.h"
#include "core/SshSession.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

namespace hssh {

SshShellProcess::SshShellProcess(const SessionConfig &config, QObject *parent)
    : ShellProcess(parent)
    , m_config(config)
{
}

SshShellProcess::~SshShellProcess()
{
    close();
}

bool SshShellProcess::start()
{
#ifdef HSSH_HAS_LIBSSH
    m_sessionId = ConnectionManager::instance().createSession(m_config);
    m_session = ConnectionManager::instance().session(m_sessionId);
    if (!m_session) {
        emit errorOccurred(tr("Failed to create SSH session"));
        return false;
    }

    // Apply the gates stored before start() (SessionTab sets them right
    // after construction — the session object did not exist yet then).
    if (m_hostKeyVerifier) {
        m_session->setHostKeyVerifier(m_hostKeyVerifier);
    }
    if (m_kbdintPrompter) {
        m_session->setKbdintPrompter(m_kbdintPrompter);
    }

    connect(m_session, &SshSession::dataReceived, this, [this](const QByteArray &data) {
        emit dataReceived(data);
    });
    connect(m_session, &SshSession::errorOccurred, this, [this](const QString &message) {
        emit errorOccurred(message);
    });
    connect(m_session, &SshSession::connectionLost, this, [this]() {
        emit linkDown(m_config.autoReconnect());
    });
    connect(m_session, &SshSession::execFinished, this, [this](int exitCode) {
        emit finished(exitCode);
    });

    // Run the session's post-login commands once the shell channel is up.
    // A short delay lets the shell print its banner/prompt first so the
    // commands land in a usable prompt instead of being swallowed.
    const QStringList postLoginCommands = m_config.postLoginCommands();
    if (!postLoginCommands.isEmpty()) {
        connect(m_session, &SshSession::connected, this, [this, postLoginCommands]() {
            QTimer::singleShot(150, this, [this, postLoginCommands]() {
                if (!m_session) {
                    return;
                }
                for (const QString &command : postLoginCommands) {
                    m_session->writeShell(command.toUtf8() + "\n");
                }
            });
        });
    }

    // Connect signals first so that the initial banner/prompt data emitted by
    // the shell reader thread is not lost before SshShellProcess is wired up.
    // The connect runs asynchronously; success/failure arrives via signals.
    // Push the widget's current size before connecting: a reconnect
    // (close+start) creates a fresh SshSession whose pending pty size is
    // unset, so the new pty would open at the 80x24 default while the widget
    // keeps its real size — readline then wraps at 80 columns and glues
    // prompt-redraw fragments mid-line across existing screen content.
    // SshSession caches the size and applies it when the shell channel opens.
    if (m_size.width() > 0 && m_size.height() > 0) {
        m_session->setShellSize(m_size.width(), m_size.height());
    }
    m_session->connectToHost();
    return true;
#else
    // Fallback: run ssh.exe in interactive mode
    m_process = new QProcess(this);
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
        emit dataReceived(m_process->readAllStandardOutput());
    });
    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        emit dataReceived(m_process->readAllStandardError());
    });
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
                emit finished(exitCode);
            });

    QStringList args;
    args << QStringLiteral("-tt"); // Force pseudo-terminal allocation even if stdin is not a TTY
    args << QStringLiteral("-p") << QString::number(m_config.port());
    args << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=accept-new");
    args << QStringLiteral("-o") << QStringLiteral("BatchMode=no");
    if (!m_config.username().isEmpty()) {
        args << QStringLiteral("-l") << m_config.username();
    }
    if (!m_config.privateKeyPath().isEmpty()) {
        args << QStringLiteral("-i") << m_config.privateKeyPath();
    }
    args << m_config.host();

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    m_process->setProcessEnvironment(env);

    m_process->start(QStringLiteral("ssh.exe"), args);
    return m_process->state() != QProcess::NotRunning;
#endif
}

void SshShellProcess::write(const QByteArray &data)
{
#ifdef HSSH_HAS_LIBSSH
    if (m_session) {
        m_session->writeShell(data);
    }
#else
    if (m_process) {
        m_process->write(data);
    }
#endif
}

void SshShellProcess::resize(int columns, int rows)
{
    m_size = QSize(columns, rows);
#ifdef HSSH_HAS_LIBSSH
    if (m_session) {
        m_session->setShellSize(columns, rows);
    }
#else
    Q_UNUSED(columns)
    Q_UNUSED(rows)
#endif
}

void SshShellProcess::close()
{
#ifdef HSSH_HAS_LIBSSH
    if (m_session) {
        // closeSession disconnects and deletes the SshSession and removes it
        // from the manager, so it does not leak with its reader thread.
        ConnectionManager::instance().closeSession(m_sessionId);
        m_session = nullptr;
        m_sessionId.clear();
    }
#else
    if (m_process) {
        m_process->closeWriteChannel();
        m_process->kill();
        m_process->waitForFinished(1000);
    }
#endif
}

bool SshShellProcess::isRunning() const
{
#ifdef HSSH_HAS_LIBSSH
    return m_session != nullptr;
#else
    return m_process && m_process->state() != QProcess::NotRunning;
#endif
}

} // namespace hssh

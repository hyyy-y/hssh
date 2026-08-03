#include "LocalShellProcess.h"

#include <QDebug>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace hssh {

bool LocalShellProcess::start()
{
    if (m_masterFd != -1 || m_childPid != -1) {
        emit errorOccurred(tr("Local shell process is already running"));
        return false;
    }

    const ShellCommand cmd = resolveShellCommand();
    if (!cmd.valid || cmd.program.isEmpty()) {
        emit errorOccurred(tr("Shell type '%1' is not available on this system").arg(m_shellType));
        return false;
    }

    if (!initializeUnixPty()) {
        return false;
    }

    struct winsize ws;
    std::memset(&ws, 0, sizeof(ws));
    ws.ws_col = static_cast<unsigned short>(m_size.width());
    ws.ws_row = static_cast<unsigned short>(m_size.height());

    pid_t pid = forkpty(&m_masterFd, nullptr, nullptr, &ws);
    if (pid == -1) {
        const QString reason = QString::fromLocal8Bit(std::strerror(errno));
        emit errorOccurred(tr("forkpty failed: %1").arg(reason));
        shutdownUnixPty();
        return false;
    }

    if (pid == 0) {
        // Child process: set up environment and exec shell.
        const QByteArray term = "xterm-256color";
        ::setenv("TERM", term.constData(), 1);

        // Ensure the child is the session leader so job control works.
        ::setsid();

        const QByteArray program = cmd.program.toLocal8Bit();
        std::vector<QByteArray> argBytes;
        std::vector<char *> argv;
        argBytes.reserve(static_cast<size_t>(cmd.args.size()) + 2);
        argBytes.emplace_back(program);
        for (const QString &arg : cmd.args) {
            argBytes.emplace_back(arg.toLocal8Bit());
        }
        argv.reserve(argBytes.size() + 1);
        for (auto &item : argBytes) {
            argv.push_back(item.data());
        }
        argv.push_back(nullptr);

        ::execvp(program.constData(), argv.data());
        ::_exit(127);
    }

    // Parent process.
    m_childPid = pid;
    ::fcntl(m_masterFd, F_SETFD, FD_CLOEXEC);

    m_readNotifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &LocalShellProcess::onMasterFdReady);

    return true;
}

void LocalShellProcess::write(const QByteArray &data)
{
    if (m_masterFd == -1) {
        return;
    }
    const char *ptr = data.constData();
    qint64 remaining = data.size();
    while (remaining > 0) {
        const ssize_t n = ::write(m_masterFd, ptr, static_cast<size_t>(remaining));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // PTY is full; blocking fd should not hit this path often.
                // In a production app, use a write notifier for backpressure.
                break;
            }
            break;
        }
        ptr += n;
        remaining -= n;
    }
}

void LocalShellProcess::resize(int columns, int rows)
{
    m_size = QSize(columns, rows);
    if (m_masterFd == -1) {
        return;
    }
    struct winsize ws;
    std::memset(&ws, 0, sizeof(ws));
    ws.ws_col = static_cast<unsigned short>(columns);
    ws.ws_row = static_cast<unsigned short>(rows);
    ::ioctl(m_masterFd, TIOCSWINSZ, &ws);
}

void LocalShellProcess::close()
{
    if (m_masterFd == -1 && m_childPid == -1) {
        return;
    }

    if (m_childPid > 0) {
        // Send SIGHUP to simulate terminal hangup.
        ::kill(m_childPid, SIGHUP);
    }

    // Graceful shutdown with escalating signals.
    QTimer::singleShot(1000, this, [this]() {
        if (m_childPid > 0 && ::kill(m_childPid, 0) == 0) {
            ::kill(m_childPid, SIGTERM);
            QTimer::singleShot(500, this, [this]() {
                if (m_childPid > 0 && ::kill(m_childPid, 0) == 0) {
                    ::kill(m_childPid, SIGKILL);
                }
                shutdownUnixPty();
            });
        } else {
            shutdownUnixPty();
        }
    });
}

bool LocalShellProcess::isRunning() const
{
    if (m_exited || m_childPid == -1) {
        return false;
    }
    return ::kill(m_childPid, 0) == 0;
}

bool LocalShellProcess::initializeUnixPty()
{
    m_masterFd = -1;
    m_childPid = -1;
    m_exited = false;
    return true;
}

void LocalShellProcess::shutdownUnixPty()
{
    if (m_readNotifier) {
        m_readNotifier->setEnabled(false);
        m_readNotifier->deleteLater();
        m_readNotifier = nullptr;
    }

    if (m_masterFd != -1) {
        ::close(m_masterFd);
        m_masterFd = -1;
    }

    if (m_childPid > 0) {
        int status = 0;
        int exitCode = -1;
        const pid_t reaped = ::waitpid(m_childPid, &status, WNOHANG);
        if (reaped == m_childPid) {
            if (WIFEXITED(status)) {
                exitCode = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                exitCode = 128 + WTERMSIG(status);
            }
            m_childPid = -1;
            if (!m_exited) {
                m_exited = true;
                emit finished(exitCode);
            }
        } else if (reaped == 0) {
            // Child still running; do not reset m_childPid yet.
        } else {
            // Error; assume gone.
            m_childPid = -1;
            if (!m_exited) {
                m_exited = true;
                emit finished(exitCode);
            }
        }
    }
}

void LocalShellProcess::onMasterFdReady()
{
    if (m_masterFd == -1) {
        return;
    }

    std::array<char, 4096> buffer;
    bool eof = false;
    while (true) {
        const ssize_t n = ::read(m_masterFd, buffer.data(), buffer.size());
        if (n > 0) {
            emit dataReceived(QByteArray(buffer.data(), static_cast<int>(n)));
        } else if (n == 0) {
            eof = true;
            break;
        } else {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            // EIO typically means the slave side has closed.
            eof = true;
            break;
        }
    }

    if (eof) {
        shutdownUnixPty();
    }
}

void LocalShellProcess::emitFinished(int exitCode)
{
    if (!m_exited) {
        m_exited = true;
        emit finished(exitCode);
    }
}

} // namespace hssh

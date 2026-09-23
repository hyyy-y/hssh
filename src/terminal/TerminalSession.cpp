#include "TerminalSession.h"

#include "ZModemEngine.h"
#include "TerminalWidget.h"
#include "terminal/SshShellProcess.h"
#include "utils/Config.h"

#include <QDateTime>
#include <QDir>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace hssh {

namespace {
// WindTerm-style disconnect banner drawn into the terminal itself.
QByteArray disconnectBanner(bool autoReconnect)
{
    QByteArray banner = "\r\n\x1b[97;41m ✕ The remote host closed the connection \x1b[0m\r\n";
    if (autoReconnect) {
        banner += "\x1b[90m连接已断开，正在自动重连… (Auto-reconnect in progress)\x1b[0m\r\n";
    } else {
        banner += "\x1b[90m会话已断开连接，按回车重新连接。 (Press Enter to reconnect)\x1b[0m\r\n";
    }
    return banner;
}
} // namespace

TerminalSession::TerminalSession(ShellProcess *process, QWidget *parent)
    : QWidget(parent)
    , m_process(process)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_terminal = new TerminalWidget(this);
    layout->addWidget(m_terminal);

    // PH2-10: ZMODEM watcher (engine emits protocol bytes via output()).
    m_zmodem = new ZModemEngine(this);
    connect(m_zmodem, &ZModemEngine::output, this, [this](const QByteArray &data) {
        if (!data.isEmpty() && m_process) {
            m_process->write(data);
        }
    });
    connect(m_zmodem, &ZModemEngine::status, this, [this](const QString &line) {
        m_terminal->feedData(("\r\n" + line + "\r\n").toUtf8());
    });

    if (m_process) {
        m_process->setParent(this);
        connect(m_terminal, &TerminalWidget::dataToSend, this, &TerminalSession::onKeyboardInput);
        connect(m_process, &ShellProcess::dataReceived, this, &TerminalSession::onDataReceived);
        connect(m_process, &ShellProcess::finished, this, &TerminalSession::onProcessFinished);
        connect(m_process, &ShellProcess::errorOccurred, this, &TerminalSession::onProcessError);
        connect(m_terminal, &TerminalWidget::sizeChanged, m_process, &ShellProcess::resize);
        connect(m_terminal, &TerminalWidget::sizeChanged, this, &TerminalSession::sizeChanged);
        if (auto *ssh = qobject_cast<SshShellProcess *>(m_process)) {
            connect(ssh, &SshShellProcess::linkDown, this, &TerminalSession::onLinkDown);
        }
    }

    // Automatic session logging (one file per terminal tab).
    if (Config::instance().boolValue(QStringLiteral("session/logging"), true)) {
        const QString logsDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QDir::separator() + QStringLiteral("logs");
        QDir().mkpath(logsDir);
        m_logFile.setFileName(logsDir + QDir::separator()
                              + QStringLiteral("session_%1.log")
                                    .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"))));
        m_logFile.open(QIODevice::WriteOnly | QIODevice::Append);
    }
}

TerminalSession::~TerminalSession()
{
    if (m_logFile.isOpen()) {
        m_logFile.flush();
        m_logFile.close();
    }
}

ShellProcess *TerminalSession::process() const
{
    return m_process;
}

void TerminalSession::start()
{
    if (m_process) {
        // Synchronize the PTY size with the widget before starting so the
        // pseudo terminal is created with the correct dimensions.
        m_process->resize(m_terminal->columns(), m_terminal->rows());
        m_process->start();
    }
}

void TerminalSession::stop()
{
    if (m_process) {
        m_process->close();
    }
}

QString TerminalSession::bufferText(int maxLines) const
{
    return m_terminal ? m_terminal->bufferText(maxLines) : QString();
}

QString TerminalSession::bufferTextRange(int fromLine, int maxLines) const
{
    return m_terminal ? m_terminal->bufferTextRange(fromLine, maxLines) : QString();
}

void TerminalSession::sendInput(const QByteArray &data)
{
    onInputReceived(data);
}

// Keyboard path: same delivery as the API path, plus a signal for sync-input
// routing. Mirrored input re-enters the targets through sendInput (API), so
// the mirror never re-emits keyboardInput — no feedback loop.
void TerminalSession::onKeyboardInput(const QByteArray &data)
{
    emit keyboardInput(data);
    // Ctrl+C during an active ZMODEM transfer cancels the transfer (the
    // engine sends the CAN sequence); the keystroke itself is swallowed.
    if (m_zmodem && m_zmodem->isActive() && data.contains('\x03')) {
        m_zmodem->abort();
        return;
    }
    onInputReceived(data);
}

void TerminalSession::onInputReceived(const QByteArray &data)
{
    if (m_linkDead) {
        // Enter-to-reconnect: swallow all other input while the link is dead.
        if (data.contains('\r') || data.contains('\n')) {
            reconnect();
        }
        return;
    }
    if (m_process) {
        m_process->write(data);
    }
}

void TerminalSession::onDataReceived(const QByteArray &data)
{
    // PH2-10: the ZMODEM engine watches the stream; protocol bytes never
    // reach the terminal, everything else displays as before.
    if (m_zmodem) {
        const QByteArray display = m_zmodem->feed(data);
        if (!display.isEmpty()) {
            if (m_logFile.isOpen()) {
                m_logFile.write(display);
                m_logFile.flush();
            }
            m_terminal->feedData(display);
        }
        return;
    }
    if (m_logFile.isOpen()) {
        m_logFile.write(data);
        m_logFile.flush();
    }
    m_terminal->feedData(data);
}

void TerminalSession::onProcessFinished(int exitCode)
{
    m_terminal->feedData(tr("\n[Process finished with exit code %1]\n").arg(exitCode).toUtf8());
    // For SSH shells, a finished channel means the remote closed the link;
    // offer the reconnect gesture. Local shells restart on Enter as well.
    m_linkDead = true;
    m_terminal->feedData(disconnectBanner(false));
}

void TerminalSession::onProcessError(const QString &message)
{
    m_terminal->feedData(tr("\n[Error: %1]\n").arg(message).toUtf8());
    // A failed (re)connect emits errorOccurred but neither finished nor
    // linkDown, so m_linkDead stayed false and the Enter-to-reconnect gesture
    // silently died after the first attempt. Re-arm it on every failure.
    if (!m_linkDead) {
        m_linkDead = true;
        m_terminal->feedData(disconnectBanner(false));
    }
}

void TerminalSession::onLinkDown(bool autoReconnect)
{
    m_linkDead = true;
    m_terminal->feedData(disconnectBanner(autoReconnect));
    emit linkDown(autoReconnect);
}

void TerminalSession::setTerminalFont(const QFont &font)
{
    if (m_terminal) {
        m_terminal->setTerminalFont(font);
    }
}

bool TerminalSession::zmodemSendFile(const QString &localPath)
{
    return m_zmodem && m_zmodem->startSend(localPath);
}

void TerminalSession::reconnect()
{
    if (!m_process) {
        return;
    }
    m_linkDead = false;
    m_terminal->feedData("\r\n\x1b[90mReconnecting… / 正在重新连接…\x1b[0m\r\n");
    m_process->close();
    m_process->start();
}

} // namespace hssh

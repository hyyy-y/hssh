#include "SessionTab.h"

#include "core/SessionConfig.h"
#include "core/SshSession.h"
#include "terminal/LocalShellProcess.h"
#include "terminal/SshShellProcess.h"
#include "terminal/TerminalSession.h"

#include <QVBoxLayout>

namespace hssh {

SessionTab::SessionTab(const SessionConfig &config, QWidget *parent)
    : QWidget(parent)
    , m_config(config)
{
    setupUi();
    connectSession();
}

SessionTab::~SessionTab() = default;

SessionTab *SessionTab::createLocal(const QString &shellType, QWidget *parent)
{
    SessionConfig config;
    config.setSessionType(SessionType::Local);
    config.setShellType(shellType.isEmpty() ? LocalShellProcess::defaultShell() : shellType);
    config.setName(config.displayName());
    return new SessionTab(config, parent);
}

SessionConfig SessionTab::config() const
{
    return m_config;
}

SshSession *SessionTab::sshSession() const
{
    if (!m_terminalSession) {
        return nullptr;
    }
    auto *sshProcess = qobject_cast<SshShellProcess *>(m_terminalSession->process());
    return sshProcess ? sshProcess->session() : nullptr;
}

QString SessionTab::readTerminalText(int maxLines) const
{
    return m_terminalSession ? m_terminalSession->bufferText(maxLines) : QString();
}

QString SessionTab::readTerminalTextRange(int fromLine, int maxLines) const
{
    return m_terminalSession ? m_terminalSession->bufferTextRange(fromLine, maxLines) : QString();
}

void SessionTab::setupUi()
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // TerminalSession is created in connectSession().
    Q_UNUSED(layout)
}

void SessionTab::connectSession()
{
    ShellProcess *process = nullptr;

    if (m_config.sessionType() == SessionType::Local) {
        process = new LocalShellProcess(m_config.shellType(), this);
    } else if (m_config.isValid()) {
        process = new SshShellProcess(m_config, this);
    } else {
        // Fallback local shell if no valid config (should not normally happen).
        process = new LocalShellProcess(LocalShellProcess::defaultShell(), this);
    }

    m_terminalSession = new TerminalSession(process, this);
    layout()->addWidget(m_terminalSession);
    connect(m_terminalSession, &TerminalSession::sizeChanged, this, &SessionTab::sizeChanged);
    m_terminalSession->start();
}

void SessionTab::disconnectSession()
{
    if (m_terminalSession) {
        m_terminalSession->stop();
    }
}

void SessionTab::runCommand(const QString &command)
{
    if (m_terminalSession) {
        // Route through TerminalSession so the link-dead guard and
        // Enter-to-reconnect apply to API-sent input too.
        m_terminalSession->sendInput(command.toUtf8());
    }
}

} // namespace hssh

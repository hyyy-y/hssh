#include "SessionTab.h"

#include "dialogs/KbdintPromptDialog.h"
#include "core/SessionConfig.h"
#include "core/SshSession.h"
#include "terminal/LocalShellProcess.h"
#include "terminal/SerialShellProcess.h"
#include "terminal/SshShellProcess.h"
#include "terminal/TerminalSession.h"
#include "utils/Config.h"

#include <QMessageBox>
#include <QVBoxLayout>

#include <memory>

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
    } else if (m_config.sessionType() == SessionType::Serial) {
        process = new SerialShellProcess(m_config.serialPort(), m_config.serialBaudRate(), this);
    } else if (m_config.isValid()) {
        auto *ssh = new SshShellProcess(m_config, this);
#ifdef HSSH_HAS_LIBSSH
        // PH2-12: interactive host-key prompt when the policy says "ask".
        // The verifier runs on the connect worker thread; the dialog is
        // marshalled to the GUI thread via a blocking queued call.
        if (Config::instance().stringValue(QStringLiteral("security/hostKeyPolicy"),
                                           QStringLiteral("accept-new"))
            == QLatin1String("ask")) {
            ssh->setHostKeyVerifier(
                [this](const KeyStore::HostKeyInfo &info, bool changed)
                    -> KeyStore::HostKeyDecision {
                    auto result = std::make_shared<int>(
                        static_cast<int>(KeyStore::HostKeyDecision::Reject));
                    QMetaObject::invokeMethod(
                        this,
                        [this, info, changed, result]() {
                            *result = static_cast<int>(promptHostKey(info, changed));
                        },
                        Qt::BlockingQueuedConnection);
                    return static_cast<KeyStore::HostKeyDecision>(*result);
                });
        }
        // PH2-13: interactive 2FA (keyboard-interactive) rounds — same
        // worker→GUI marshalling, plus an optional per-session answer cache
        // (the dialog's "remember" checkbox).
        ssh->setKbdintPrompter(
            [this](const QString &name, const QString &instruction,
                   const QStringList &prompts, const QList<bool> &echo,
                   QStringList *answers) -> bool {
                const QString cacheKey =
                    name + QLatin1Char('|') + prompts.join(QLatin1Char('\x1f'));
                const auto cached = m_kbdintCache.constFind(cacheKey);
                if (cached != m_kbdintCache.constEnd()) {
                    *answers = cached.value();
                    return true;
                }
                auto ok = std::make_shared<bool>(false);
                auto filled = std::make_shared<QStringList>();
                QMetaObject::invokeMethod(
                    this,
                    [this, name, instruction, prompts, echo, ok, filled]() {
                        KbdintPromptDialog dialog(name, instruction, prompts, echo, this);
                        *ok = dialog.exec() == QDialog::Accepted;
                        if (*ok) {
                            *filled = dialog.answers();
                            if (dialog.rememberForSession()) {
                                m_kbdintCache.insert(
                                    name + QLatin1Char('|')
                                        + prompts.join(QLatin1Char('\x1f')),
                                    *filled);
                            }
                        }
                    },
                    Qt::BlockingQueuedConnection);
                if (!*ok) {
                    return false;
                }
                *answers = *filled;
                return true;
            });
#endif
        process = ssh;
    } else {
        // Fallback local shell if no valid config (should not normally happen).
        process = new LocalShellProcess(LocalShellProcess::defaultShell(), this);
    }

    m_terminalSession = new TerminalSession(process, this);
    layout()->addWidget(m_terminalSession);
    connect(m_terminalSession, &TerminalSession::sizeChanged, this, &SessionTab::sizeChanged);
    connect(m_terminalSession, &TerminalSession::keyboardInput, this, &SessionTab::inputTyped);
    m_terminalSession->start();
}

#ifdef HSSH_HAS_LIBSSH
KeyStore::HostKeyDecision SessionTab::promptHostKey(const KeyStore::HostKeyInfo &info,
                                                    bool changed)
{
    const QString title = changed ? tr("Host Key CHANGED") : tr("Unknown Host Key");
    const QString body = changed
        ? tr("The host key for %1 has CHANGED!\n"
             "This could indicate a man-in-the-middle attack, or the server\n"
             "was reinstalled. Verify the fingerprint out-of-band before\n"
             "continuing.\n\nKey type: %2\nSHA256 fingerprint: %3\nMD5 fingerprint: %4\n\n"
             "Accept and replace the stored key?")
              .arg(info.host, info.keyType, info.fingerprintSha256, info.fingerprintMd5)
        : tr("The authenticity of host %1 cannot be established.\n\n"
             "Key type: %2\nSHA256 fingerprint: %3\nMD5 fingerprint: %4\n\n"
             "Trust this host and store its key?")
              .arg(info.host, info.keyType, info.fingerprintSha256, info.fingerprintMd5);
    const auto answer = QMessageBox::warning(this, title, body,
                                             QMessageBox::Yes | QMessageBox::No,
                                             QMessageBox::No);
    return answer == QMessageBox::Yes ? KeyStore::HostKeyDecision::Accept
                                      : KeyStore::HostKeyDecision::Reject;
}
#endif

void SessionTab::disconnectSession()
{
    if (m_terminalSession) {
        m_terminalSession->stop();
    }
}

void SessionTab::reconnectSession()
{
    if (m_terminalSession) {
        m_terminalSession->reconnect();
    }
}

void SessionTab::setTerminalFont(const QFont &font)
{
    if (m_terminalSession) {
        m_terminalSession->setTerminalFont(font);
    }
}

bool SessionTab::zmodemSendFile(const QString &localPath)
{
    return m_terminalSession && m_terminalSession->zmodemSendFile(localPath);
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

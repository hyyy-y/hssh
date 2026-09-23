#include "NetworkToolsDialog.h"

#include "app/GuiHostKeyPrompt.h"
#include "core/RemoteCommandChannel.h"

#include <QComboBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace hssh {

namespace {

// Hostname / IPv4 / IPv6-ish / interface-name characters. Anything else in
// the target rejects the run: these strings end up in a remote shell line.
const QRegularExpression &safeTargetRx()
{
    static const QRegularExpression rx(QStringLiteral("^[A-Za-z0-9_.:\\[\\]-]{1,255}$"));
    return rx;
}

} // namespace

NetworkToolsDialog::NetworkToolsDialog(const SessionConfig &config, QWidget *parent)
    : QDialog(parent)
    , m_config(config)
{
    setWindowTitle(tr("Network Tools — %1").arg(config.displayName()));
    setObjectName(QStringLiteral("networkToolsDialog"));
    resize(760, 520);

    auto *layout = new QVBoxLayout(this);

    auto *form = new QHBoxLayout;
    m_kindBox = new QComboBox(this);
    m_kindBox->addItem(tr("ping"), QStringLiteral("ping"));
    m_kindBox->addItem(tr("traceroute"), QStringLiteral("traceroute"));
    m_kindBox->addItem(tr("listening sockets (ss)"), QStringLiteral("ss"));
    m_kindBox->addItem(tr("port probe"), QStringLiteral("port"));
    m_targetEdit = new QLineEdit(this);
    m_targetEdit->setPlaceholderText(tr("host or IP"));
    m_portEdit = new QLineEdit(this);
    m_portEdit->setPlaceholderText(tr("port"));
    m_portEdit->setMaximumWidth(90);
    m_runButton = new QPushButton(tr("Run"), this);
    m_runButton->setDefault(true);
    m_stopButton = new QPushButton(tr("Stop"), this);
    m_stopButton->setEnabled(false);
    form->addWidget(m_kindBox);
    form->addWidget(m_targetEdit, 1);
    form->addWidget(m_portEdit);
    form->addWidget(m_stopButton);
    form->addWidget(m_runButton);
    layout->addLayout(form);

    m_output = new QPlainTextEdit(this);
    m_output->setReadOnly(true);
    m_output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_output->setPlaceholderText(tr("Output appears here (streaming; Stop interrupts)"));
    layout->addWidget(m_output, 1);

    connect(m_kindBox, &QComboBox::currentIndexChanged, this, [this](int index) {
        // ss needs no target; the port box only matters for the probe.
        const QString kind = m_kindBox->itemData(index).toString();
        m_targetEdit->setEnabled(kind != QLatin1String("ss"));
        m_portEdit->setEnabled(kind == QLatin1String("port"));
    });
    m_portEdit->setEnabled(false);
    connect(m_runButton, &QPushButton::clicked, this, &NetworkToolsDialog::run);
    connect(m_stopButton, &QPushButton::clicked, this, &NetworkToolsDialog::stop);

    // Parentless worker (it moveToThreads itself).
    m_channel = new RemoteCommandChannel(m_config);
    connect(m_channel, &RemoteCommandChannel::commandOutput, this,
            [this](const QString &id, const QByteArray &chunk) {
                appendOutput(id, chunk);
            });
    // PH2-12 policy-aware host-key gate (background connections used to be
    // silent TOFU even with security/hostKeyPolicy=ask).
    m_channel->setHostKeyVerifier(
        [](const KeyStore::HostKeyInfo &info, bool changed) {
            return decideHostKey(info, changed);
        });
    // Show the REAL peer: DNS/DHCP drift lands background connections on a
    // different machine than the visible tab.
    const QString baseTitle = windowTitle();
    connect(m_channel, &RemoteCommandChannel::connected, this,
            [this, baseTitle](const QString &peer) {
                setWindowTitle(QStringLiteral("%1 — %2").arg(baseTitle, peer));
            });
    connect(m_channel, &RemoteCommandChannel::commandFinished, this,
            [this](const QString &id, int exitCode, const QString &error) {
                onFinished(id, exitCode, error);
            });
}

NetworkToolsDialog::~NetworkToolsDialog()
{
    m_channel->stop();
    delete m_channel; // stop() joined the worker: direct delete is safe
}

QString NetworkToolsDialog::buildCommand(const QString &kind, const QString &target,
                                         const QString &port, bool *streaming)
{
    const bool isStreaming = kind == QLatin1String("ping") || kind == QLatin1String("traceroute");
    if (streaming) {
        *streaming = isStreaming;
    }

    if (kind == QLatin1String("ss")) {
        // One-shot; falls back to netstat when ss is missing (busybox).
        return QStringLiteral("ss -tunlp 2>/dev/null || netstat -tunlp 2>/dev/null");
    }

    if (!safeTargetRx().match(target).hasMatch()) {
        return QString();
    }
    if (kind == QLatin1String("ping")) {
        // No -c limit: the Stop button is the natural end for an interactive
        // ping. Plain flags only — busybox ping knows -O but others may not.
        return QStringLiteral("ping %1").arg(target);
    }
    if (kind == QLatin1String("traceroute")) {
        return QStringLiteral("traceroute -n -w 2 -q 1 %1 2>/dev/null || tracepath -n %1").arg(target);
    }
    if (kind == QLatin1String("port")) {
        bool okPort = false;
        const int portNumber = port.toInt(&okPort);
        if (!okPort || portNumber < 1 || portNumber > 65535) {
            return QString();
        }
        // Quoted because the whole probe is inside a bash -c string.
        return QStringLiteral(
                   "timeout 4 bash -c 'echo > /dev/tcp/%1/%2' 2>/dev/null && echo OPEN || echo CLOSED")
            .arg(target)
            .arg(portNumber);
    }
    return QString();
}

void NetworkToolsDialog::run()
{
    if (m_running) {
        return;
    }
    const QString kind = m_kindBox->currentData().toString();
    bool streaming = false;
    const QString command = buildCommand(kind, m_targetEdit->text().trimmed(),
                                         m_portEdit->text().trimmed(), &streaming);
    if (command.isEmpty()) {
        m_output->appendPlainText(tr("Invalid target or port (allowed: letters, digits, "
                                     ". : _ [ ] -; port 1-65535)."));
        return;
    }

    m_output->clear();
    m_output->appendPlainText(tr("$ %1").arg(command));
    m_running = true;
    m_runButton->setEnabled(false);
    m_stopButton->setEnabled(streaming); // one-shots finish on their own
    if (streaming) {
        m_channel->runStream(QStringLiteral("net"), command);
    } else {
        m_channel->runCommand(QStringLiteral("net"), command, 20000);
    }
}

void NetworkToolsDialog::stop()
{
    m_channel->cancel(QStringLiteral("net"));
}

void NetworkToolsDialog::appendOutput(const QString &id, const QByteArray &chunk)
{
    if (id != QLatin1String("net")) {
        return;
    }
    m_output->appendPlainText(QString::fromUtf8(chunk).trimmed());
}

void NetworkToolsDialog::onFinished(const QString &id, int exitCode, const QString &error)
{
    if (id != QLatin1String("net")) {
        return;
    }
    m_running = false;
    m_runButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    if (!error.isEmpty()) {
        m_output->appendPlainText(tr("\n[%1]").arg(error));
    } else if (exitCode == 0) {
        m_output->appendPlainText(tr("\n[done]"));
    } else {
        m_output->appendPlainText(tr("\n[exit %1]").arg(exitCode));
    }
}

} // namespace hssh

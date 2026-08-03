#include "TerminalSession.h"

#include "TerminalWidget.h"

#include <QCoreApplication>
#include <QFile>
#include <QVBoxLayout>

namespace hssh {

TerminalSession::TerminalSession(ShellProcess *process, QWidget *parent)
    : QWidget(parent)
    , m_process(process)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_terminal = new TerminalWidget(this);
    layout->addWidget(m_terminal);

    if (m_process) {
        m_process->setParent(this);
        connect(m_terminal, &TerminalWidget::dataToSend, this, &TerminalSession::onInputReceived);
        connect(m_process, &ShellProcess::dataReceived, this, &TerminalSession::onDataReceived);
        connect(m_process, &ShellProcess::finished, this, &TerminalSession::onProcessFinished);
        connect(m_process, &ShellProcess::errorOccurred, this, &TerminalSession::onProcessError);
        connect(m_terminal, &TerminalWidget::sizeChanged, m_process, &ShellProcess::resize);
        connect(m_terminal, &TerminalWidget::sizeChanged, this, &TerminalSession::sizeChanged);
    }
}

TerminalSession::~TerminalSession() = default;

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

void TerminalSession::onInputReceived(const QByteArray &data)
{
    if (m_process) {
        m_process->write(data);
    }
}

void TerminalSession::onDataReceived(const QByteArray &data)
{
    QFile debugFile(QCoreApplication::applicationDirPath() + QStringLiteral("/terminal_debug.log"));
    if (debugFile.open(QIODevice::WriteOnly | QIODevice::Append)) {
        debugFile.write(data);
        debugFile.flush();
    }
    m_terminal->feedData(data);
}

void TerminalSession::onProcessFinished(int exitCode)
{
    m_terminal->feedData(tr("\n[Process finished with exit code %1]\n").arg(exitCode).toUtf8());
}

void TerminalSession::onProcessError(const QString &message)
{
    m_terminal->feedData(tr("\n[Error: %1]\n").arg(message).toUtf8());
}

} // namespace hssh

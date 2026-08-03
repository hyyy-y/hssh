#ifndef HSSH_TERMINAL_TERMINALSESSION_H
#define HSSH_TERMINAL_TERMINALSESSION_H

#include "ShellProcess.h"

#include <QWidget>

namespace hssh {

class SessionConfig;
class TerminalWidget;

class TerminalSession : public QWidget {
    Q_OBJECT

public:
    explicit TerminalSession(ShellProcess *process, QWidget *parent = nullptr);
    ~TerminalSession() override;

    [[nodiscard]] ShellProcess *process() const;
    void start();
    void stop();

signals:
    void sizeChanged(int columns, int rows);

private slots:
    void onInputReceived(const QByteArray &data);
    void onDataReceived(const QByteArray &data);
    void onProcessFinished(int exitCode);
    void onProcessError(const QString &message);

private:
    ShellProcess *m_process = nullptr;
    TerminalWidget *m_terminal = nullptr;
};

} // namespace hssh

#endif // HSSH_TERMINAL_TERMINALSESSION_H

#ifndef HSSH_APP_DIALOGS_NETWORKTOOLDIALOG_H
#define HSSH_APP_DIALOGS_NETWORKTOOLDIALOG_H

#include "core/SessionConfig.h"

#include <QDialog>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace hssh {

class RemoteCommandChannel;

// B6-3: ping / traceroute / listening sockets / port probe, all executed on
// the REMOTE side over a dedicated connection. Long-running commands stream
// into the output pane and can be stopped; short ones run one-shot.
class NetworkToolsDialog : public QDialog {
    Q_OBJECT

public:
    explicit NetworkToolsDialog(const SessionConfig &config, QWidget *parent = nullptr);
    ~NetworkToolsDialog() override;

    // Command builder (unit-tested). Returns an empty string when the
    // target/port contains characters outside the safe set (no shell
    // injection surface — the values go into a remote shell line).
    [[nodiscard]] static QString buildCommand(const QString &kind, const QString &target,
                                              const QString &port, bool *streaming);

private:
    void run();
    void stop();
    void appendOutput(const QString &id, const QByteArray &chunk);
    void onFinished(const QString &id, int exitCode, const QString &error);

    SessionConfig m_config;
    RemoteCommandChannel *m_channel = nullptr;
    QComboBox *m_kindBox = nullptr;
    QLineEdit *m_targetEdit = nullptr;
    QLineEdit *m_portEdit = nullptr;
    QPushButton *m_runButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPlainTextEdit *m_output = nullptr;
    bool m_running = false;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_NETWORKTOOLDIALOG_H

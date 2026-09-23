#ifndef HSSH_APP_DIALOGS_PROCESSDIALOG_H
#define HSSH_APP_DIALOGS_PROCESSDIALOG_H

#include "core/SessionConfig.h"

#include <QDialog>

class QLineEdit;
class QTableWidget;

namespace hssh {

class RemoteCommandChannel;

// B6-2: remote process list over a dedicated connection (ps -eo), sortable
// and filterable, with SIGTERM / SIGKILL actions. A kill that fails for
// lack of permission reports the remote error (root-owned processes need a
// sudo path — deliberately not auto-chained here).
class ProcessDialog : public QDialog {
    Q_OBJECT

public:
    struct PsEntry {
        qint64 pid = 0;
        qint64 ppid = 0;
        QString user;
        double cpuPercent = 0;
        double memPercent = 0;
        QString stat;
        QString args;
    };

    explicit ProcessDialog(const SessionConfig &config, QWidget *parent = nullptr);
    ~ProcessDialog() override;

    // Parse `ps -eo pid,ppid,user,%cpu,%mem,stat,args --no-headers` output.
    [[nodiscard]] static QList<PsEntry> parsePs(const QString &output);

private:
    void refresh();
    void applyFilter();
    void signalSelected(int signalNumber);
    void onCommandFinished(const QString &id, int exitCode, const QString &error);

    SessionConfig m_config;
    RemoteCommandChannel *m_channel = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QTableWidget *m_table = nullptr;
    QList<PsEntry> m_entries;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_PROCESSDIALOG_H

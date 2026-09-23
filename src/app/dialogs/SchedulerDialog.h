#ifndef HSSH_APP_DIALOGS_SCHEDULERDIALOG_H
#define HSSH_APP_DIALOGS_SCHEDULERDIALOG_H

#include "core/SessionConfig.h"

#include <QDateTime>
#include <QDialog>
#include <QFile>

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QTableWidget;
class QTimer;

namespace hssh {

class RemoteCommandChannel;
class SessionTab;

// B6-5: interval-scheduled tasks for one SSH tab. Two execution modes:
//   background — runs over a dedicated connection; the output tail and exit
//                code append to <AppData>/logs/scheduler_<date>.log
//   terminal   — injects the command into the VISIBLE terminal tab (the
//                session log already records everything there).
// Tasks live for the dialog's tab lifetime (in-memory, no persistence).
class SchedulerDialog : public QDialog {
    Q_OBJECT

public:
    struct Task {
        int id = 0;
        QString name;
        QString command;
        int intervalSeconds = 60;
        bool inTerminal = false;
        bool enabled = true;
        QDateTime lastRun;
        QDateTime nextDue;
    };

    explicit SchedulerDialog(SessionTab *tab, QWidget *parent = nullptr);
    ~SchedulerDialog() override;

  [[nodiscard]] static QString scheduleLogPath(const QDate &date);
    // Computes the next due time (pure; unit-tested).
    [[nodiscard]] static QDateTime nextDue(const QDateTime &after, int intervalSeconds);

private:
    void addTask();
    void removeSelected();
    void toggleSelected(bool enabled);
    void tick();
    void runTask(const Task &task);

    SessionTab *m_tab = nullptr;
    SessionConfig m_config;
    RemoteCommandChannel *m_channel = nullptr;
    QTimer *m_timer = nullptr;
    QTableWidget *m_table = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_commandEdit = nullptr;
    QComboBox *m_intervalBox = nullptr;
    QComboBox *m_modeBox = nullptr;
    QPlainTextEdit *m_lastResult = nullptr;
    QFile m_logFile;

    QList<Task> m_tasks;
    int m_nextTaskId = 1;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_SCHEDULERDIALOG_H

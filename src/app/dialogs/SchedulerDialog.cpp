#include "SchedulerDialog.h"

#include "app/SessionTab.h"
#include "app/GuiHostKeyPrompt.h"
#include "core/RemoteCommandChannel.h"
#include "terminal/TerminalSession.h"

#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace hssh {

namespace {

constexpr int kTickMs = 1000;

} // namespace

SchedulerDialog::SchedulerDialog(SessionTab *tab, QWidget *parent)
    : QDialog(parent)
    , m_tab(tab)
    , m_config(tab ? tab->config() : SessionConfig())
{
    setWindowTitle(tr("Scheduled Tasks — %1").arg(m_config.displayName()));
    setObjectName(QStringLiteral("schedulerDialog"));
    resize(820, 520);

    auto *layout = new QVBoxLayout(this);

    // New-task form.
    auto *form = new QFormLayout;
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(tr("uptime check"));
    m_commandEdit = new QLineEdit(this);
    m_commandEdit->setPlaceholderText(tr("uptime && free -m | head -2"));
    m_intervalBox = new QComboBox(this);
    for (int seconds : {10, 30, 60, 300, 600, 1800, 3600}) {
        m_intervalBox->addItem(tr("%1 s").arg(seconds), seconds);
    }
    m_intervalBox->setCurrentIndex(2); // 60 s
    m_modeBox = new QComboBox(this);
    m_modeBox->addItem(tr("background (logged)"), false);
    m_modeBox->addItem(tr("terminal (visible)"), true);
    form->addRow(tr("Name:"), m_nameEdit);
    form->addRow(tr("Command:"), m_commandEdit);
    form->addRow(tr("Interval:"), m_intervalBox);
    form->addRow(tr("Mode:"), m_modeBox);
    layout->addLayout(form);

    auto *addRow = new QHBoxLayout;
    auto *addButton = new QPushButton(tr("Add task"), this);
    addRow->addStretch(1);
    addRow->addWidget(addButton);
    layout->addLayout(addRow);

    m_table = new QTableWidget(0, 6, this);
    m_table->setHorizontalHeaderLabels(
        {tr("Name"), tr("Command"), tr("Interval"), tr("Mode"), tr("Next run"), tr("Last run")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table, 1);

    auto *bar = new QHBoxLayout;
    auto *removeButton = new QPushButton(tr("Remove"), this);
    auto *toggleButton = new QPushButton(tr("Enable / Disable"), this);
    bar->addWidget(removeButton);
    bar->addWidget(toggleButton);
    bar->addStretch(1);
    layout->addLayout(bar);

    m_lastResult = new QPlainTextEdit(this);
    m_lastResult->setReadOnly(true);
    m_lastResult->setMaximumHeight(110);
    m_lastResult->setPlaceholderText(tr("Last background result (also appended to the "
                                        "scheduler log)"));
    layout->addWidget(m_lastResult);

    // Scheduler log: one file per day, next to the session logs.
    m_logFile.setFileName(scheduleLogPath(QDate::currentDate()));
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                  + QDir::separator() + QStringLiteral("logs"));
    m_logFile.open(QIODevice::WriteOnly | QIODevice::Append);

    // Parentless worker (it moveToThreads itself).
    m_channel = new RemoteCommandChannel(m_config);
    connect(m_channel, &RemoteCommandChannel::commandOutput, this,
            [this](const QString &id, const QByteArray &chunk) {
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
                if (!id.startsWith(QLatin1String("task-"))) {
                    return;
                }
                // Remember the tail for the pane; the log line is written on
                // finish (with exit code) in onFinished via m_lastResult.
                m_lastResult->setPlainText(QStringLiteral("[%1]\n%2")
                                               .arg(id, QString::fromUtf8(chunk).trimmed()));
                if (m_logFile.isOpen()) {
                    m_logFile.write(QStringLiteral("[%1 %2] %3\n")
                                        .arg(QDateTime::currentDateTime().toString(
                                                 QStringLiteral("HH:mm:ss")),
                                             id,
                                             QString::fromUtf8(chunk).trimmed())
                                        .toUtf8());
                    m_logFile.flush();
                }
            });

    m_timer = new QTimer(this);
    m_timer->setInterval(kTickMs);
    connect(m_timer, &QTimer::timeout, this, &SchedulerDialog::tick);
    m_timer->start();

    connect(addButton, &QPushButton::clicked, this, &SchedulerDialog::addTask);
    connect(removeButton, &QPushButton::clicked, this, &SchedulerDialog::removeSelected);
    connect(toggleButton, &QPushButton::clicked, this,
            [this]() { toggleSelected(true); });
}

SchedulerDialog::~SchedulerDialog()
{
    m_channel->stop();
    delete m_channel; // stop() joined the worker: direct delete is safe
    if (m_logFile.isOpen()) {
        m_logFile.close();
    }
}

QString SchedulerDialog::scheduleLogPath(const QDate &date)
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QDir::separator() + QStringLiteral("logs") + QDir::separator()
        + QStringLiteral("scheduler_%1.log").arg(date.toString(QStringLiteral("yyyyMMdd")));
}

QDateTime SchedulerDialog::nextDue(const QDateTime &after, int intervalSeconds)
{
    if (intervalSeconds <= 0) {
        return after;
    }
    return after.addSecs(intervalSeconds);
}

void SchedulerDialog::addTask()
{
    static const QRegularExpression nameRx(QStringLiteral("^[^\\r\\n]{1,64}$"));
    Task task;
    task.id = m_nextTaskId++;
    task.name = m_nameEdit->text().trimmed();
    task.command = m_commandEdit->text().trimmed();
    task.intervalSeconds = m_intervalBox->currentData().toInt();
    task.inTerminal = m_modeBox->currentData().toBool();
    if (!nameRx.match(task.name).hasMatch() || task.command.isEmpty()) {
        m_lastResult->setPlainText(tr("Provide a name (single line) and a command."));
        return;
    }
    task.nextDue = nextDue(QDateTime::currentDateTime(), task.intervalSeconds);
    m_tasks.append(task);
    m_nameEdit->clear();
    m_commandEdit->clear();

    // Rebuild the table (few rows; simplicity beats in-place updates).
    const auto timeText = [](const Task &t) {
        return t.nextDue.isValid() ? t.nextDue.toString(QStringLiteral("HH:mm:ss")) : QString();
    };
    m_table->setRowCount(m_tasks.size());
    for (int i = 0; i < m_tasks.size(); ++i) {
        const Task &t = m_tasks.at(i);
        m_table->setItem(i, 0, new QTableWidgetItem(t.enabled ? t.name : tr("%1 (off)").arg(t.name)));
        m_table->setItem(i, 1, new QTableWidgetItem(t.command));
        m_table->setItem(i, 2, new QTableWidgetItem(tr("%1 s").arg(t.intervalSeconds)));
        m_table->setItem(i, 3,
                         new QTableWidgetItem(t.inTerminal ? tr("terminal") : tr("background")));
        m_table->setItem(i, 4, new QTableWidgetItem(timeText(t)));
        m_table->setItem(i, 5,
                         new QTableWidgetItem(t.lastRun.isValid()
                                                  ? t.lastRun.toString(QStringLiteral("HH:mm:ss"))
                                                  : QString()));
    }
}

void SchedulerDialog::removeSelected()
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_tasks.size()) {
        return;
    }
    m_tasks.removeAt(row);
    m_table->removeRow(row);
}

void SchedulerDialog::toggleSelected(bool)
{
    const int row = m_table->currentRow();
    if (row < 0 || row >= m_tasks.size()) {
        return;
    }
    m_tasks[row].enabled = !m_tasks[row].enabled;
    m_table->setItem(row, 0,
                     new QTableWidgetItem(m_tasks.at(row).enabled
                                             ? m_tasks.at(row).name
                                             : tr("%1 (off)").arg(m_tasks.at(row).name)));
}

void SchedulerDialog::tick()
{
    const QDateTime now = QDateTime::currentDateTime();
    for (int i = 0; i < m_tasks.size(); ++i) {
        Task &task = m_tasks[i];
        if (!task.enabled || (task.nextDue.isValid() && now < task.nextDue)) {
            continue;
        }
        runTask(task);
        task.lastRun = now;
        task.nextDue = nextDue(now, task.intervalSeconds);
        m_table->setItem(i, 4, new QTableWidgetItem(
                                   task.nextDue.toString(QStringLiteral("HH:mm:ss"))));
        m_table->setItem(i, 5, new QTableWidgetItem(
                                   task.lastRun.toString(QStringLiteral("HH:mm:ss"))));
    }
}

void SchedulerDialog::runTask(const Task &task)
{
    if (task.inTerminal) {
        // Visible injection: the tab's own session log records the output.
        if (m_tab && m_tab->terminalSession()) {
            m_tab->terminalSession()->sendInput(
                (task.command + QStringLiteral("\n")).toUtf8());
        }
        return;
    }
    m_channel->runCommand(QStringLiteral("task-%1-%2").arg(task.id).arg(task.name),
                          task.command, 60000);
}

} // namespace hssh

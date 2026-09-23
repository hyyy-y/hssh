#include "ProcessDialog.h"

#include "app/GuiHostKeyPrompt.h"
#include "core/RemoteCommandChannel.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>

namespace hssh {

namespace {

// pid ppid user %cpu %mem stat args... — args keep their spaces.
constexpr auto kPsCommand =
    "ps -eo pid,ppid,user:24,%cpu,%mem,stat,args --no-headers --sort=-%cpu 2>/dev/null | head -n 500";

constexpr int kPidRole = Qt::UserRole + 1;

} // namespace

ProcessDialog::ProcessDialog(const SessionConfig &config, QWidget *parent)
    : QDialog(parent)
    , m_config(config)
{
    setWindowTitle(tr("Processes — %1").arg(config.displayName()));
    setObjectName(QStringLiteral("processDialog"));
    resize(900, 560);

    auto *layout = new QVBoxLayout(this);

    auto *bar = new QHBoxLayout;
    auto *refreshButton = new QPushButton(tr("Refresh"), this);
    auto *endButton = new QPushButton(tr("End Process (TERM)"), this);
    auto *killButton = new QPushButton(tr("Kill (KILL)"), this);
    m_filterEdit = new QLineEdit(this);
    m_filterEdit->setPlaceholderText(tr("Filter by user, command or PID"));
    m_filterEdit->setClearButtonEnabled(true);
    bar->addWidget(m_filterEdit, 1);
    bar->addWidget(refreshButton);
    bar->addWidget(endButton);
    bar->addWidget(killButton);
    layout->addLayout(bar);

    m_table = new QTableWidget(0, 7, this);
    m_table->setHorizontalHeaderLabels(
        {tr("PID"), tr("PPID"), tr("User"), tr("CPU %"), tr("MEM %"), tr("Stat"), tr("Command")});
    m_table->verticalHeader()->setVisible(false);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSortingEnabled(true);
    m_table->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_table, 1);

    auto *statusLabel = new QLabel(this);
    layout->addWidget(statusLabel);

    // Parentless worker (it moveToThreads itself).
    m_channel = new RemoteCommandChannel(m_config);
    connect(m_channel, &RemoteCommandChannel::commandOutput, this,
            [this, statusLabel](const QString &id, const QByteArray &chunk) {
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
                if (id != QLatin1String("ps")) {
                    return;
                }
                m_entries = parsePs(QString::fromUtf8(chunk));
                applyFilter();
                statusLabel->setText(tr("%1 process(es)").arg(m_entries.size()));
            });
    connect(m_channel, &RemoteCommandChannel::commandFinished, this,
            [this, statusLabel](const QString &id, int exitCode, const QString &error) {
                if (id != QLatin1String("ps")) {
                    return;
                }
                Q_UNUSED(exitCode);
                if (!error.isEmpty() && m_entries.isEmpty()) {
                    statusLabel->setText(tr("ps failed: %1").arg(error));
                }
            });
    // Kill command results (id "kill-<pid>") land here.
    connect(m_channel, &RemoteCommandChannel::commandFinished, this,
            &ProcessDialog::onCommandFinished);

    connect(refreshButton, &QPushButton::clicked, this, &ProcessDialog::refresh);
    connect(endButton, &QPushButton::clicked, this, [this]() { signalSelected(15); });
    connect(killButton, &QPushButton::clicked, this, [this]() { signalSelected(9); });
    connect(m_filterEdit, &QLineEdit::textChanged, this, &ProcessDialog::applyFilter);

    refresh();
}

ProcessDialog::~ProcessDialog()
{
    m_channel->stop();
    delete m_channel; // stop() joined the worker: direct delete is safe
}

QList<ProcessDialog::PsEntry> ProcessDialog::parsePs(const QString &output)
{
    QList<PsEntry> entries;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QStringList f = line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (f.size() < 7) {
            continue;
        }
        PsEntry entry;
        bool okPid = false;
        bool okPpid = false;
        entry.pid = f.at(0).toLongLong(&okPid);
        entry.ppid = f.at(1).toLongLong(&okPpid);
        entry.user = f.at(2);
        entry.cpuPercent = f.at(3).toDouble();
        entry.memPercent = f.at(4).toDouble();
        entry.stat = f.at(5);
        entry.args = f.mid(6).join(QLatin1Char(' '));
        if (okPid && okPpid) {
            entries.append(entry);
        }
    }
    return entries;
}

void ProcessDialog::refresh()
{
    m_entries.clear();
    m_channel->runCommand(QStringLiteral("ps"), QString::fromLatin1(kPsCommand), 15000);
}

void ProcessDialog::applyFilter()
{
    const QString needle = m_filterEdit->text().trimmed();
    m_table->setSortingEnabled(false); // bulk refill is faster unsorted
    m_table->setRowCount(0);
    for (const PsEntry &entry : std::as_const(m_entries)) {
        if (!needle.isEmpty()
            && !entry.args.contains(needle, Qt::CaseInsensitive)
            && !entry.user.contains(needle, Qt::CaseInsensitive)
            && !QString::number(entry.pid).contains(needle)) {
            continue;
        }
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *pidItem = new QTableWidgetItem;
        pidItem->setData(Qt::DisplayRole, QVariant::fromValue<qlonglong>(entry.pid));
        pidItem->setData(kPidRole, QVariant::fromValue<qlonglong>(entry.pid));
        auto *ppidItem = new QTableWidgetItem;
        ppidItem->setData(Qt::DisplayRole, QVariant::fromValue<qlonglong>(entry.ppid));
        auto *cpuItem = new QTableWidgetItem;
        cpuItem->setData(Qt::DisplayRole, entry.cpuPercent);
        cpuItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto *memItem = new QTableWidgetItem;
        memItem->setData(Qt::DisplayRole, entry.memPercent);
        memItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

        m_table->setItem(row, 0, pidItem);
        m_table->setItem(row, 1, ppidItem);
        m_table->setItem(row, 2, new QTableWidgetItem(entry.user));
        m_table->setItem(row, 3, cpuItem);
        m_table->setItem(row, 4, memItem);
        m_table->setItem(row, 5, new QTableWidgetItem(entry.stat));
        m_table->setItem(row, 6, new QTableWidgetItem(entry.args));
    }
    m_table->setSortingEnabled(true);
}

void ProcessDialog::signalSelected(int signalNumber)
{
    const int row = m_table->currentRow();
    if (row < 0 || !m_table->item(row, 0)) {
        return;
    }
    const qint64 pid = m_table->item(row, 0)->data(kPidRole).toLongLong();
    const QString command = m_table->item(row, 6)->text();

    const QMessageBox::StandardButton answer = QMessageBox::question(
        this, signalNumber == 9 ? tr("Kill process") : tr("End process"),
        tr("Send SIG%1 to PID %2?\n\n%3")
            .arg(signalNumber == 9 ? QStringLiteral("KILL") : QStringLiteral("TERM"))
            .arg(pid)
            .arg(command.left(200)),
        QMessageBox::Yes | QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }

    // stderr stays on its own channel: the error tail rides `error`.
    m_channel->runCommand(QStringLiteral("kill-%1").arg(pid),
                          QStringLiteral("kill -%1 %2").arg(signalNumber).arg(pid), 10000);
    // The table refreshes when the kill command finishes (onCommandFinished).
}

void ProcessDialog::onCommandFinished(const QString &id, int exitCode, const QString &error)
{
    if (!id.startsWith(QLatin1String("kill-"))) {
        return; // ps results are handled in the output lambda
    }
    if (error.isEmpty()) {
        refresh();
        return;
    }
    // Common case: not the owner — needs root, which goes through the sudo
    // confirmation path on purpose.
    QMessageBox::warning(this, tr("Signal failed"),
                         tr("kill %1 failed: %2\n\n(root-owned processes need the sudo "
                            "path in the terminal tab)").arg(id.mid(5), error));
    Q_UNUSED(exitCode);
}

} // namespace hssh

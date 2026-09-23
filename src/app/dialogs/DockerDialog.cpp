#include "DockerDialog.h"

#include "app/GuiHostKeyPrompt.h"
#include "core/RemoteCommandChannel.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace hssh {

namespace {

// docker CLI IDs are hex; anything longer than a short id's worth of hex is
// rejected before it ever reaches the shell line.
const QRegularExpression &safeIdRx()
{
    static const QRegularExpression rx(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]{0,128}$"));
    return rx;
}

} // namespace

DockerDialog::DockerDialog(const SessionConfig &config, QWidget *parent)
    : QDialog(parent)
    , m_config(config)
{
    setWindowTitle(tr("Docker — %1").arg(config.displayName()));
    setObjectName(QStringLiteral("dockerDialog"));
    resize(900, 560);

    auto *layout = new QVBoxLayout(this);

    m_tabs = new QTabWidget(this);
    m_containerTable = new QTableWidget(0, 5, m_tabs);
    m_containerTable->setHorizontalHeaderLabels(
        {tr("Name"), tr("Image"), tr("State"), tr("Status"), tr("ID")});
    m_containerTable->verticalHeader()->setVisible(false);
    m_containerTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_containerTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_containerTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_containerTable->horizontalHeader()->setStretchLastSection(true);
    m_tabs->addTab(m_containerTable, tr("Containers"));

    m_imageTable = new QTableWidget(0, 4, m_tabs);
    m_imageTable->setHorizontalHeaderLabels({tr("Repository"), tr("Tag"), tr("Size"), tr("ID")});
    m_imageTable->verticalHeader()->setVisible(false);
    m_imageTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_imageTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_imageTable->horizontalHeader()->setStretchLastSection(true);
    m_tabs->addTab(m_imageTable, tr("Images"));
    layout->addWidget(m_tabs, 1);

    auto *bar = new QHBoxLayout;
    auto *refreshButton = new QPushButton(tr("Refresh"), this);
    m_startButton = new QPushButton(tr("Start"), this);
    m_stopButton = new QPushButton(tr("Stop"), this);
    m_restartButton = new QPushButton(tr("Restart"), this);
    m_rmButton = new QPushButton(tr("Remove"), this);
    m_logsButton = new QPushButton(tr("Logs..."), this);
    bar->addWidget(refreshButton);
    bar->addStretch(1);
    bar->addWidget(m_logsButton);
    bar->addWidget(m_startButton);
    bar->addWidget(m_stopButton);
    bar->addWidget(m_restartButton);
    bar->addWidget(m_rmButton);
    layout->addLayout(bar);

    const auto enableActionButtons = [this](bool enabled) {
        const QList<QPushButton *> buttons = {m_startButton, m_stopButton, m_restartButton,
                                              m_rmButton, m_logsButton};
        for (QPushButton *button : buttons) {
            button->setEnabled(enabled);
        }
    };
    enableActionButtons(false);
    connect(m_containerTable, &QTableWidget::itemSelectionChanged, this,
            [this, enableActionButtons]() {
                enableActionButtons(m_containerTable->currentRow() >= 0);
            });

    connect(refreshButton, &QPushButton::clicked, this, &DockerDialog::refresh);
    connect(m_startButton, &QPushButton::clicked, this,
            [this]() { containerAction(QStringLiteral("start")); });
    connect(m_stopButton, &QPushButton::clicked, this,
            [this]() { containerAction(QStringLiteral("stop")); });
    connect(m_restartButton, &QPushButton::clicked, this,
            [this]() { containerAction(QStringLiteral("restart")); });
    connect(m_rmButton, &QPushButton::clicked, this,
            [this]() { containerAction(QStringLiteral("rm")); });
    connect(m_logsButton, &QPushButton::clicked, this, &DockerDialog::showLogs);

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
                if (id == QLatin1String("ps")) {
                    m_containers = parseContainers(QString::fromUtf8(chunk));
                    fillContainers();
                } else if (id == QLatin1String("images")) {
                    m_images = parseImages(QString::fromUtf8(chunk));
                    fillImages();
                }
            });
    connect(m_channel, &RemoteCommandChannel::commandFinished, this,
            [this](const QString &id, int exitCode, const QString &error) {
                onFinished(id, exitCode, error);
            });

    refresh();
}

DockerDialog::~DockerDialog()
{
    m_channel->stop();
    delete m_channel; // stop() joined the worker: direct delete is safe
}

QList<DockerDialog::ContainerEntry> DockerDialog::parseContainers(const QString &output)
{
    QList<ContainerEntry> entries;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed().toUtf8());
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject obj = doc.object();
        ContainerEntry entry;
        entry.id = obj.value(QStringLiteral("ID")).toString();
        entry.name = obj.value(QStringLiteral("Names")).toString();
        entry.image = obj.value(QStringLiteral("Image")).toString();
        entry.state = obj.value(QStringLiteral("State")).toString();
        entry.status = obj.value(QStringLiteral("Status")).toString();
        if (!entry.id.isEmpty()) {
            entries.append(entry);
        }
    }
    return entries;
}

QList<DockerDialog::ImageEntry> DockerDialog::parseImages(const QString &output)
{
    QList<ImageEntry> entries;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QJsonDocument doc = QJsonDocument::fromJson(line.trimmed().toUtf8());
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject obj = doc.object();
        ImageEntry entry;
        entry.id = obj.value(QStringLiteral("ID")).toString();
        entry.repository = obj.value(QStringLiteral("Repository")).toString();
        entry.tag = obj.value(QStringLiteral("Tag")).toString();
        entry.size = obj.value(QStringLiteral("Size")).toString();
        if (!entry.id.isEmpty()) {
            entries.append(entry);
        }
    }
    return entries;
}

void DockerDialog::refresh()
{
    // stderr stays separate: a missing/locked docker reports through the
    // error channel into the warning box.
    m_channel->runCommand(QStringLiteral("ps"),
                          QStringLiteral("docker ps -a --format '{{json .}}'"), 20000);
    m_channel->runCommand(QStringLiteral("images"),
                          QStringLiteral("docker images --format '{{json .}}'"), 20000);
}

QString DockerDialog::selectedContainerId() const
{
    const int row = m_containerTable->currentRow();
    if (row < 0 || row >= m_containers.size()) {
        return QString();
    }
    return m_containers.at(row).id;
}

void DockerDialog::containerAction(const QString &verb)
{
    const QString id = selectedContainerId();
    if (id.isEmpty() || !safeIdRx().match(id).hasMatch()) {
        return;
    }
    if (verb == QLatin1String("rm")) {
        const auto answer = QMessageBox::question(
            this, tr("Remove container"),
            tr("Remove container %1? Running containers are stopped first "
               "(docker rm -f).").arg(id),
            QMessageBox::Yes | QMessageBox::No);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    // docker writes its errors to stderr — no 2>&1 here, the error tail
    // rides the error channel into the message box.
    m_channel->runCommand(QStringLiteral("act-%1-%2").arg(verb, id),
                          QStringLiteral("docker %1 %2 %3")
                              .arg(verb, id, verb == QLatin1String("rm") ? QStringLiteral("-f")
                                                                         : QString()),
                          30000);
}

void DockerDialog::showLogs()
{
    const QString id = selectedContainerId();
    if (id.isEmpty() || !safeIdRx().match(id).hasMatch()) {
        return;
    }

    QDialog logDialog(this);
    logDialog.setWindowTitle(tr("Logs — %1 (streaming, close to stop)").arg(id));
    logDialog.resize(760, 480);
    auto *logLayout = new QVBoxLayout(&logDialog);
    auto *view = new QPlainTextEdit(&logDialog);
    view->setReadOnly(true);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    logLayout->addWidget(view);

    // NB: this dialog shares the channel; a log stream and a container
    // action may interleave, ids keep them apart.
    const QString logId = QStringLiteral("logs-%1").arg(id);
    const QMetaObject::Connection outputConn = connect(
        m_channel, &RemoteCommandChannel::commandOutput, &logDialog,
        [logId, view](const QString &id, const QByteArray &chunk) {
            if (id == logId) {
                view->appendPlainText(QString::fromUtf8(chunk).trimmed());
            }
        });
    connect(&logDialog, &QDialog::finished, this, [this, logId, outputConn]() {
        disconnect(outputConn);
        m_channel->cancel(logId);
    });

    m_channel->runStream(logId, QStringLiteral("docker logs -f --tail 200 %1").arg(id));
    logDialog.exec();
}

void DockerDialog::fillContainers()
{
    m_containerTable->setRowCount(m_containers.size());
    for (int i = 0; i < m_containers.size(); ++i) {
        const auto &c = m_containers.at(i);
        m_containerTable->setItem(i, 0, new QTableWidgetItem(c.name));
        m_containerTable->setItem(i, 1, new QTableWidgetItem(c.image));
        m_containerTable->setItem(i, 2, new QTableWidgetItem(c.state));
        m_containerTable->setItem(i, 3, new QTableWidgetItem(c.status));
        m_containerTable->setItem(i, 4, new QTableWidgetItem(c.id));
    }
}

void DockerDialog::fillImages()
{
    m_imageTable->setRowCount(m_images.size());
    for (int i = 0; i < m_images.size(); ++i) {
        const auto &img = m_images.at(i);
        m_imageTable->setItem(i, 0, new QTableWidgetItem(img.repository));
        m_imageTable->setItem(i, 1, new QTableWidgetItem(img.tag));
        m_imageTable->setItem(i, 2, new QTableWidgetItem(img.size));
        m_imageTable->setItem(i, 3, new QTableWidgetItem(img.id));
    }
}

void DockerDialog::onFinished(const QString &id, int exitCode, const QString &error)
{
    if (id == QLatin1String("ps") || id == QLatin1String("images")) {
        if (!error.isEmpty() && m_containers.isEmpty() && m_images.isEmpty()) {
            QMessageBox::warning(this, tr("Docker"),
                                 tr("docker not usable: %1").arg(error));
        }
        return;
    }
    if (id.startsWith(QLatin1String("act-"))) {
        if (error.isEmpty()) {
            refresh();
        } else {
            QMessageBox::warning(this, tr("Docker action failed"),
                                 tr("%1").arg(error));
        }
        return;
    }
    Q_UNUSED(exitCode);
}

} // namespace hssh

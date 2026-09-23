#include "ServerTransferDialog.h"

#include "app/GuiHostKeyPrompt.h"
#include "app/TransferRegistry.h"
#include "core/SftpSession.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QUuid>
#include <utility>
#include <QVBoxLayout>

namespace hssh {

namespace {

[[nodiscard]] bool usableForTransfer(const SessionConfig &config)
{
    return config.sessionType() == SessionType::Ssh && !config.host().isEmpty();
}

} // namespace

ServerTransferDialog::ServerTransferDialog(const QList<SessionConfig> &sessions,
                                           const QList<QPair<QString, QString>> &openTabEndpoints,
                                           QWidget *parent)
    : ServerTransferDialog(sessions, parent)
{
    m_openTabEndpoints = openTabEndpoints;
}

ServerTransferDialog::ServerTransferDialog(const QList<SessionConfig> &sessions, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Server-to-Server Transfer"));
    setObjectName(QStringLiteral("serverTransferDialog"));

    for (const SessionConfig &config : sessions) {
        if (usableForTransfer(config)) {
            m_sessions.append(config);
        }
    }

    auto *rootLayout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    m_sourceBox = new QComboBox(this);
    m_targetBox = new QComboBox(this);
    for (const SessionConfig &config : std::as_const(m_sessions)) {
        const QString label = QStringLiteral("%1 (%2)")
                                  .arg(config.displayName(),
                                       QStringLiteral("%1@%2:%3")
                                           .arg(config.username(), config.host())
                                           .arg(config.port()));
        m_sourceBox->addItem(label);
        m_targetBox->addItem(label);
    }
    if (!m_sessions.isEmpty()) {
        m_targetBox->setCurrentIndex(m_sessions.size() > 1 ? 1 : 0);
    }

    m_sourcePath = new QLineEdit(this);
    m_sourcePath->setPlaceholderText(tr("/remote/path/to/source-file"));
    m_targetPath = new QLineEdit(this);
    m_targetPath->setPlaceholderText(tr("/remote/path/on/target-host"));

    form->addRow(tr("Source session:"), m_sourceBox);
    form->addRow(tr("Source file:"), m_sourcePath);
    form->addRow(tr("Target session:"), m_targetBox);
    form->addRow(tr("Target file:"), m_targetPath);
    rootLayout->addLayout(form);

    auto *note = new QLabel(
        tr("Single files. The copy goes through a verified local temp file "
           "(both hops are md5-checked); nothing is left behind."),
        this);
    note->setWordWrap(true);
    rootLayout->addWidget(note);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    rootLayout->addWidget(m_progress);

    m_phaseLabel = new QLabel(this);
    rootLayout->addWidget(m_phaseLabel);

    auto *buttonRow = new QHBoxLayout;
    m_startButton = new QPushButton(tr("Transfer"), this);
    m_startButton->setDefault(true);
    m_cancelButton = new QPushButton(tr("Cancel"), this);
    m_cancelButton->setEnabled(false);
    buttonRow->addStretch(1);
    buttonRow->addWidget(m_cancelButton);
    buttonRow->addWidget(m_startButton);
    rootLayout->addLayout(buttonRow);

    if (m_sessions.isEmpty()) {
        m_startButton->setEnabled(false);
        m_phaseLabel->setText(tr("No saved SSH sessions — save the source and "
                                 "target sessions first."));
    }

    connect(m_startButton, &QPushButton::clicked, this, &ServerTransferDialog::onStart);
    connect(m_cancelButton, &QPushButton::clicked, this, &ServerTransferDialog::onCancel);
}

ServerTransferDialog::~ServerTransferDialog()
{
    // A run still in flight at destruction (parent window closing): stop the
    // workers; the temp file is left for %TEMP% housekeeping — deleting it
    // mid-download would race the worker writing it.
    if (m_sourceWorker) {
        m_sourceWorker->stop();
        delete m_sourceWorker;
    }
    if (m_targetWorker) {
        m_targetWorker->stop();
        delete m_targetWorker;
    }
}

void ServerTransferDialog::reject()
{
    if (m_phase == Phase::Downloading || m_phase == Phase::Uploading) {
        onCancel(); // closing the dialog mid-run means "stop"
        return;
    }
    QDialog::reject();
}

// Empty when the selected session agrees with every open same-name tab (or
// no such tab is open); otherwise a human-readable warning. Pure.
QString ServerTransferDialog::endpointWarning(
    const SessionConfig &selected,
    const QList<QPair<QString, QString>> &openTabEndpoints)
{
    if (selected.name().isEmpty()) {
        return QString(); // ad-hoc: no saved session to go stale
    }
    const QString want = QStringLiteral("%1@%2:%3")
                             .arg(selected.username(), selected.host())
                             .arg(selected.port());
    for (const auto &tab : openTabEndpoints) {
        if (tab.first != selected.name() || tab.second.isEmpty()) {
            continue;
        }
        if (tab.second != want) {
            return QStringLiteral(
                       "Session '%1' currently points at %2, but an open tab with that "
                       "name is connected to %3 (the session was edited after the tab "
                       "opened). The transfer follows the SAVED endpoint.")
                .arg(selected.name(), want, tab.second);
        }
    }
    return QString();
}

void ServerTransferDialog::onStart()
{
    if (m_sessions.isEmpty() || m_phase == Phase::Downloading || m_phase == Phase::Uploading) {
        return;
    }
    m_sourceRemote = m_sourcePath->text().trimmed();
    m_targetRemote = m_targetPath->text().trimmed();
    if (m_sourceRemote.isEmpty() || m_targetRemote.isEmpty()) {
        m_phaseLabel->setText(tr("Enter both remote paths."));
        return;
    }
    if (m_sourceBox->currentIndex() < 0 || m_targetBox->currentIndex() < 0) {
        return;
    }

    // Wrong-machine guard: both hops must agree with any open same-name tab.
    const QString sourceWarning =
        endpointWarning(m_sessions.at(m_sourceBox->currentIndex()), m_openTabEndpoints);
    const QString targetWarning =
        endpointWarning(m_sessions.at(m_targetBox->currentIndex()), m_openTabEndpoints);
    QString combined;
    if (!sourceWarning.isEmpty()) {
        combined += sourceWarning + QStringLiteral("\n\n");
    }
    if (!targetWarning.isEmpty()) {
        combined += targetWarning + QStringLiteral("\n\n");
    }
    if (!combined.isEmpty()) {
        combined += tr("Proceed anyway?");
        if (QMessageBox::warning(this, tr("Endpoint mismatch"), combined,
                                 QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No)
            != QMessageBox::Yes) {
            return;
        }
    }

    const SessionConfig sourceConfig = m_sessions.at(m_sourceBox->currentIndex());
    const SessionConfig targetConfig = m_sessions.at(m_targetBox->currentIndex());

    // Temp file: unique per run, cleaned in finish(); ".part"-style staging
    // is SftpSession's own business during the download.
    const QString fileName = m_sourceRemote.section(QLatin1Char('/'), -1);
    const QDir tempDir = QDir::temp();
    tempDir.mkpath(QStringLiteral("hssh-s2s"));
    m_tempFile = tempDir.filePath(QStringLiteral("hssh-s2s/%1.%2")
                                      .arg(fileName.isEmpty() ? QStringLiteral("file") : fileName,
                                           QUuid::createUuid().toString(QUuid::WithoutBraces).left(8)));

    m_cancelled = false;
    m_phase = Phase::Downloading;
    m_startButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_progress->setValue(0);
    m_phaseLabel->setText(tr("Connecting to %1…").arg(sourceConfig.displayName()));

    // Parentless on purpose: SftpSession moveToThreads itself (the same rule
    // as SftpWidget / AgentHttpServer workers).
    m_sourceWorker = new SftpSession(sourceConfig);
    m_sourceWorker->setHostKeyVerifier(
        [](const KeyStore::HostKeyInfo &info, bool changed) {
            return decideHostKey(info, changed);
        });
    connect(m_sourceWorker, &TransferSession::transferProgress, this,
            &ServerTransferDialog::onSourceProgress);
    connect(m_sourceWorker, &TransferSession::transferFinished, this,
            &ServerTransferDialog::onSourceFinished);
    connect(m_sourceWorker, &TransferSession::errorOccurred, this,
            &ServerTransferDialog::onWorkerError);
    m_sourceTransferId = TransferRegistry::instance().beginTransfer(
        fileName, TransferRegistry::Direction::Download, sourceConfig.displayName());
    m_sourceWorker->start();
    m_sourceWorker->download(m_sourceRemote, m_tempFile);
}

void ServerTransferDialog::onCancel()
{
    if (m_phase != Phase::Downloading && m_phase != Phase::Uploading) {
        return;
    }
    m_cancelled = true;
    if (m_sourceWorker) {
        m_sourceWorker->cancelTransfer();
    }
    if (m_targetWorker) {
        m_targetWorker->cancelTransfer();
    }
    m_phaseLabel->setText(tr("Cancelling…"));
}

void ServerTransferDialog::cleanupRun(bool removeTemp)
{
    if (m_sourceWorker) {
        m_sourceWorker->stop();
        delete m_sourceWorker;
        m_sourceWorker = nullptr;
    }
    if (m_targetWorker) {
        m_targetWorker->stop();
        delete m_targetWorker;
        m_targetWorker = nullptr;
    }
    if (removeTemp && !m_tempFile.isEmpty()) {
        QFile::remove(m_tempFile); // also drops a leftover .part
        QFile::remove(m_tempFile + QStringLiteral(".part"));
        m_tempFile.clear();
    }
}

void ServerTransferDialog::startUploadPhase()
{
    const SessionConfig targetConfig = m_sessions.at(m_targetBox->currentIndex());
    m_phase = Phase::Uploading;
    m_progress->setValue(0);
    m_phaseLabel->setText(tr("Uploading to %1…").arg(targetConfig.displayName()));

    m_targetWorker = new SftpSession(targetConfig);
    m_targetWorker->setHostKeyVerifier(
        [](const KeyStore::HostKeyInfo &info, bool changed) {
            return decideHostKey(info, changed);
        });
    connect(m_targetWorker, &TransferSession::transferProgress, this,
            &ServerTransferDialog::onTargetProgress);
    connect(m_targetWorker, &TransferSession::transferFinished, this,
            &ServerTransferDialog::onTargetFinished);
    connect(m_targetWorker, &TransferSession::errorOccurred, this,
            &ServerTransferDialog::onWorkerError);
    m_targetTransferId = TransferRegistry::instance().beginTransfer(
        m_targetRemote.section(QLatin1Char('/'), -1), TransferRegistry::Direction::Upload,
        targetConfig.displayName());
    m_targetWorker->start();
    m_targetWorker->upload(m_tempFile, m_targetRemote);
}

void ServerTransferDialog::finish(const QString &message, bool ok)
{
    // Close the Transfers-dock entries for both hops (whichever ran).
    const QString note = ok ? QString() : message;
    if (m_sourceTransferId > 0) {
        TransferRegistry::instance().finishTransfer(m_sourceTransferId, ok, note);
        m_sourceTransferId = 0;
    }
    if (m_targetTransferId > 0) {
        TransferRegistry::instance().finishTransfer(m_targetTransferId, ok, note);
        m_targetTransferId = 0;
    }
    cleanupRun(true);
    m_phase = Phase::Done;
    m_startButton->setEnabled(true);
    m_cancelButton->setEnabled(false);
    m_phaseLabel->setText(message);
    if (ok) {
        m_progress->setValue(100);
    }
}

void ServerTransferDialog::onSourceProgress(const QString &path, qint64 done, qint64 total)
{
    Q_UNUSED(path);
    if (m_phase == Phase::Downloading && total > 0) {
        m_progress->setValue(static_cast<int>(done * 100 / total));
        m_phaseLabel->setText(tr("Downloading from source… %1 %")
                                  .arg(done * 100 / total));
    }
}

void ServerTransferDialog::onSourceFinished(const QString &path, bool ok, const QString &message)
{
    Q_UNUSED(path);
    if (m_phase != Phase::Downloading) {
        return;
    }
    if (m_cancelled) {
        finish(tr("Cancelled."), false);
        return;
    }
    if (!ok) {
        finish(tr("Download from source failed: %1").arg(message), false);
        return;
    }
    // The download hop succeeded on its own — close its Transfers entry now
    // so a later upload failure doesn't misreport the source hop.
    if (m_sourceTransferId > 0) {
        TransferRegistry::instance().finishTransfer(m_sourceTransferId, true, QString());
        m_sourceTransferId = 0;
    }
    startUploadPhase();
}

void ServerTransferDialog::onTargetProgress(const QString &path, qint64 done, qint64 total)
{
    Q_UNUSED(path);
    if (m_phase == Phase::Uploading && total > 0) {
        m_progress->setValue(static_cast<int>(done * 100 / total));
        m_phaseLabel->setText(tr("Uploading to target… %1 %")
                                  .arg(done * 100 / total));
    }
}

void ServerTransferDialog::onTargetFinished(const QString &path, bool ok, const QString &message)
{
    Q_UNUSED(path);
    if (m_phase != Phase::Uploading) {
        return;
    }
    if (m_cancelled) {
        finish(tr("Cancelled."), false);
        return;
    }
    if (!ok) {
        finish(tr("Upload to target failed: %1").arg(message), false);
        return;
    }
    finish(tr("Transfer complete — both hops md5-verified."), true);
}

void ServerTransferDialog::onWorkerError(const QString &message)
{
    // Some failure paths (download queued while the connection never came
    // up) ONLY raise errorOccurred — no transferFinished ever follows, so a
    // passive display here would hang the dialog in the current phase.
    // Treating an error mid-run as terminal is safe: a later transferFinished
    // is ignored once the phase has moved on.
    if (m_phase == Phase::Downloading) {
        finish(tr("Source connection failed: %1").arg(message), false);
    } else if (m_phase == Phase::Uploading) {
        finish(tr("Target connection failed: %1").arg(message), false);
    }
}

} // namespace hssh

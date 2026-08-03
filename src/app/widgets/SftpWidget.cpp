#include "SftpWidget.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace hssh {

SftpWidget::SftpWidget(const SessionConfig &config, QWidget *parent)
    : QWidget(parent)
    , m_config(config)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    // Navigation bar.
    auto *navBar = new QHBoxLayout;
    const auto makeButton = [this](const QString &text, const QString &tooltip) {
        auto *button = new QToolButton(this);
        button->setText(text);
        button->setToolTip(tooltip);
        return button;
    };
    QToolButton *upButton = makeButton(QStringLiteral("↑"), tr("Up one directory"));
    QToolButton *homeButton = makeButton(QStringLiteral("⌂"), tr("Home directory"));
    QToolButton *refreshButton = makeButton(QStringLiteral("⟳"), tr("Refresh"));
    m_uploadButton = makeButton(QStringLiteral("⇪"), tr("Upload files here"));
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(tr("Remote path"));

    navBar->addWidget(upButton);
    navBar->addWidget(homeButton);
    navBar->addWidget(refreshButton);
    navBar->addWidget(m_pathEdit, 1);
    navBar->addWidget(m_uploadButton);
    layout->addLayout(navBar);

    connect(upButton, &QToolButton::clicked, this, &SftpWidget::goUp);
    connect(homeButton, &QToolButton::clicked, this, &SftpWidget::goHome);
    connect(refreshButton, &QToolButton::clicked, this, &SftpWidget::refresh);
    connect(m_uploadButton, &QToolButton::clicked, this, &SftpWidget::uploadFiles);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this]() {
        navigateTo(m_pathEdit->text().trimmed());
    });

    // File list.
    m_model = new QStandardItemModel(0, 4, this);
    m_model->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Modified"), tr("Permissions")});
    m_view = new QTableView(this);
    m_view->setModel(m_model);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setShowGrid(false);
    m_view->verticalHeader()->setVisible(false);
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setAlternatingRowColors(true);
    layout->addWidget(m_view, 1);

    connect(m_view, &QTableView::doubleClicked, this, &SftpWidget::onEntryActivated);
    connect(m_view, &QTableView::customContextMenuRequested, this, &SftpWidget::onContextMenu);

    m_statusLabel = new QLabel(tr("Connecting…"), this);
    layout->addWidget(m_statusLabel);

    setAcceptDrops(true);

    // SFTP worker. No parent: SftpSession moves itself to its worker thread,
    // and QObject::moveToThread refuses (and ignores) objects with a parent,
    // which would silently pin every sftp operation on the GUI thread.
    m_sftp = new SftpSession(m_config);
    connect(m_sftp, &SftpSession::connected, this, &SftpWidget::onConnected);
    connect(m_sftp, &SftpSession::errorOccurred, this, [this](const QString &message) {
        m_statusLabel->setText(tr("Error: %1").arg(message));
    });
    connect(m_sftp, &SftpSession::dirListed, this, &SftpWidget::onDirListed);
    connect(m_sftp, &SftpSession::operationFinished, this, &SftpWidget::onOperationFinished);
    connect(m_sftp, &SftpSession::transferProgress, this, &SftpWidget::onTransferProgress);
    connect(m_sftp, &SftpSession::transferStep, this, &SftpWidget::onTransferStep);
    connect(m_sftp, &SftpSession::transferFinished, this, &SftpWidget::onTransferFinished);
    connect(m_sftp, &SftpSession::dirTreeListed, this, &SftpWidget::dirTreeListed);
    m_sftp->start();
}

SftpWidget::~SftpWidget()
{
    m_sftp->stop();
    // stop() has joined the worker thread, so direct deletion is safe
    // (deleteLater would never run: the object's thread has no event loop).
    delete m_sftp;
}

QString SftpWidget::remoteJoin(const QString &dir, const QString &name) const
{
    if (dir.endsWith(QLatin1Char('/'))) {
        return dir + name;
    }
    return dir + QLatin1Char('/') + name;
}

void SftpWidget::navigateTo(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }
    m_statusLabel->setText(tr("Loading %1…").arg(path));
    m_sftp->listDir(path);
}

void SftpWidget::refresh()
{
    if (!m_currentPath.isEmpty()) {
        navigateTo(m_currentPath);
    }
}

void SftpWidget::goUp()
{
    if (m_currentPath.isEmpty() || m_currentPath == QStringLiteral("/")) {
        return;
    }
    QString parent = m_currentPath;
    const int slash = parent.lastIndexOf(QLatin1Char('/'));
    parent = slash <= 0 ? QStringLiteral("/") : parent.left(slash);
    navigateTo(parent);
}

void SftpWidget::goHome()
{
    navigateTo(m_homePath.isEmpty() ? QStringLiteral("/") : m_homePath);
}

void SftpWidget::onConnected(const QString &homePath)
{
    m_homePath = homePath;
    navigateTo(homePath);
}

void SftpWidget::onDirListed(const QString &path, const QList<SftpFileInfo> &entries)
{
    m_currentPath = path;
    m_entries = entries;
    m_pathEdit->setText(path);

    m_model->removeRows(0, m_model->rowCount());
    const QLocale locale;
    for (const SftpFileInfo &entry : entries) {
        auto *nameItem = new QStandardItem(entry.isDir ? QStringLiteral("📁 ") + entry.name : entry.name);
        const auto colorIt = m_rowColors.constFind(entry.name);
        if (colorIt != m_rowColors.constEnd()) {
            nameItem->setForeground(colorIt.value());
        }
        auto *sizeItem = new QStandardItem(entry.isDir ? QString() : locale.formattedDataSize(entry.size));
        auto *mtimeItem = new QStandardItem(
            entry.mtime > 0
                ? QDateTime::fromSecsSinceEpoch(entry.mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                : QString());
        auto *permItem = new QStandardItem(formatPermissions(entry.permissions, entry.isDir));
        sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_model->appendRow({nameItem, sizeItem, mtimeItem, permItem});
    }
    m_statusLabel->setText(tr("%1 items").arg(entries.size()));
    emit dirChanged(path);
}

void SftpWidget::setRowColors(const QHash<QString, QColor> &colors)
{
    m_rowColors = colors;
    // Repaint the name column with the new colors without a remote round-trip.
    for (int row = 0; row < m_model->rowCount() && row < m_entries.size(); ++row) {
        QStandardItem *nameItem = m_model->item(row, 0);
        const auto it = m_rowColors.constFind(m_entries.at(row).name);
        if (it != m_rowColors.constEnd()) {
            nameItem->setForeground(it.value());
        } else {
            nameItem->setData(QVariant(), Qt::ForegroundRole);
        }
    }
}

void SftpWidget::downloadTo(const QString &remotePath, const QString &localPath)
{
    trackTransfer(remotePath, TransferRegistry::Direction::Download);
    m_sftp->download(remotePath, localPath);
}

void SftpWidget::uploadTo(const QString &localPath, const QString &remotePath)
{
    trackTransfer(remotePath, TransferRegistry::Direction::Upload);
    m_sftp->upload(localPath, remotePath);
}

void SftpWidget::listDirTree(const QString &path)
{
    m_sftp->listDirRecursive(path);
}

void SftpWidget::trackTransfer(const QString &remotePath, TransferRegistry::Direction direction)
{
    const int id = TransferRegistry::instance().beginTransfer(
        remotePath.section(QLatin1Char('/'), -1), direction, m_config.displayName());
    m_transferIds.insert(remotePath, id);
}

void SftpWidget::onEntryActivated(const QModelIndex &index)
{
    if (!index.isValid() || index.row() >= m_entries.size()) {
        return;
    }
    const SftpFileInfo &entry = m_entries.at(index.row());
    if (entry.isDir) {
        navigateTo(remoteJoin(m_currentPath, entry.name));
    } else {
        confirmAndDownload({entry});
    }
}

QList<SftpFileInfo> SftpWidget::selectedEntries() const
{
    QList<SftpFileInfo> result;
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    for (const QModelIndex &index : rows) {
        if (index.row() < m_entries.size()) {
            result.append(m_entries.at(index.row()));
        }
    }
    return result;
}

void SftpWidget::onContextMenu(const QPoint &pos)
{
    const QList<SftpFileInfo> selection = selectedEntries();

    QMenu menu(this);
    QAction *downloadAction = menu.addAction(tr("Download…"));
    downloadAction->setEnabled(!selection.isEmpty());
    QAction *uploadAction = menu.addAction(tr("Upload files here…"));
    QAction *compareAction = menu.addAction(tr("Compare with local folder…"));
    menu.addSeparator();
    QAction *mkdirAction = menu.addAction(tr("New folder…"));
    QAction *renameAction = menu.addAction(tr("Rename…"));
    renameAction->setEnabled(selection.size() == 1);
    QAction *deleteAction = menu.addAction(tr("Delete"));
    deleteAction->setEnabled(!selection.isEmpty());
    menu.addSeparator();
    QAction *refreshAction = menu.addAction(tr("Refresh"));

    const QAction *chosen = menu.exec(m_view->viewport()->mapToGlobal(pos));
    if (chosen == downloadAction) {
        confirmAndDownload(selection);
    } else if (chosen == uploadAction) {
        uploadFiles();
    } else if (chosen == compareAction) {
        emit compareRequested();
    } else if (chosen == mkdirAction) {
        mkdirDialog();
    } else if (chosen == renameAction && selection.size() == 1) {
        renameEntry(selection.first());
    } else if (chosen == deleteAction) {
        deleteEntries(selection);
    } else if (chosen == refreshAction) {
        refresh();
    }
}

void SftpWidget::uploadFiles()
{
    if (m_currentPath.isEmpty()) {
        return;
    }
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Select files to upload"));
    for (const QString &localPath : files) {
        const QString name = localPath.section(QLatin1Char('/'), -1);
        const QString remotePath = remoteJoin(m_currentPath, name);
        trackTransfer(remotePath, TransferRegistry::Direction::Upload);
        m_sftp->upload(localPath, remotePath);
    }
}

void SftpWidget::confirmAndDownload(const QList<SftpFileInfo> &entries)
{
    if (entries.isEmpty()) {
        return;
    }

    const QLocale locale;
    QStringList lines;
    for (const SftpFileInfo &entry : entries) {
        lines.append(entry.isDir
                         ? tr("%1 (folder)").arg(entry.name)
                         : tr("%1 (%2)").arg(entry.name, locale.formattedDataSize(entry.size)));
    }

    QMessageBox box(QMessageBox::Question, tr("Download"),
                    tr("Download %n selected item(s)?", nullptr, static_cast<int>(entries.size())),
                    QMessageBox::Yes | QMessageBox::No, this);
    if (entries.size() <= 8) {
        box.setInformativeText(lines.join(QLatin1Char('\n')));
    } else {
        box.setDetailedText(lines.join(QLatin1Char('\n')));
    }
    if (box.exec() != QMessageBox::Yes) {
        return;
    }

    // A lone regular file keeps the save-as flow; anything else (folders or
    // multiple items) needs a destination directory.
    if (entries.size() == 1 && !entries.first().isDir) {
        downloadEntry(entries.first());
        return;
    }

    const QString localDir = QFileDialog::getExistingDirectory(this, tr("Select download destination"));
    if (localDir.isEmpty()) {
        return;
    }
    m_transferInProgress = true;
    const QDir base(localDir);
    for (const SftpFileInfo &entry : entries) {
        const QString remotePath = remoteJoin(m_currentPath, entry.name);
        trackTransfer(remotePath, TransferRegistry::Direction::Download);
        if (entry.isDir) {
            m_sftp->downloadDir(remotePath, base.filePath(entry.name));
        } else {
            m_sftp->download(remotePath, base.filePath(entry.name));
        }
    }
}

void SftpWidget::downloadEntry(const SftpFileInfo &entry)
{
    const QString localPath = QFileDialog::getSaveFileName(this, tr("Save as"), entry.name);
    if (localPath.isEmpty()) {
        return;
    }
    m_transferInProgress = true;
    const QString remotePath = remoteJoin(m_currentPath, entry.name);
    trackTransfer(remotePath, TransferRegistry::Direction::Download);
    m_sftp->download(remotePath, localPath);
}

void SftpWidget::mkdirDialog()
{
    const QString name = QInputDialog::getText(this, tr("New folder"), tr("Folder name:"));
    if (!name.isEmpty()) {
        m_sftp->makeDir(remoteJoin(m_currentPath, name));
    }
}

void SftpWidget::renameEntry(const SftpFileInfo &entry)
{
    const QString newName = QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                                  QLineEdit::Normal, entry.name);
    if (!newName.isEmpty() && newName != entry.name) {
        m_sftp->renameEntry(remoteJoin(m_currentPath, entry.name),
                            remoteJoin(m_currentPath, newName));
    }
}

void SftpWidget::deleteEntries(const QList<SftpFileInfo> &entries)
{
    if (entries.isEmpty()) {
        return;
    }
    QStringList names;
    for (const SftpFileInfo &entry : entries) {
        names.append(entry.name);
    }
    const int result = QMessageBox::question(this, tr("Confirm delete"),
                                             tr("Delete %1?\n\nThis cannot be undone.").arg(names.join(QStringLiteral(", "))),
                                             QMessageBox::Yes | QMessageBox::No);
    if (result != QMessageBox::Yes) {
        return;
    }
    for (const SftpFileInfo &entry : entries) {
        const QString path = remoteJoin(m_currentPath, entry.name);
        if (entry.isDir) {
            m_sftp->removeDir(path);
        } else {
            m_sftp->removeFile(path);
        }
    }
}

void SftpWidget::onOperationFinished(const QString &operation, bool ok, const QString &message)
{
    if (!ok) {
        m_statusLabel->setText(tr("%1 failed: %2").arg(operation, message));
    }
    // Any filesystem mutation is followed by a refresh.
    refresh();
}

QProgressDialog *SftpWidget::ensureProgressDialog()
{
    if (!m_progress) {
        m_progress = new QProgressDialog(tr("Transferring…"), tr("Cancel"), 0, 100, this);
        m_progress->setWindowModality(Qt::WindowModal);
        m_progress->setMinimumDuration(300);
        m_progress->setAutoClose(true);
        m_progress->setAutoReset(true);
        connect(m_progress, &QProgressDialog::canceled, this, [this]() {
            m_sftp->cancelTransfer();
        });
    }
    return m_progress;
}

void SftpWidget::onTransferProgress(const QString &path, qint64 done, qint64 total)
{
    const auto idIt = m_transferIds.constFind(path);
    if (idIt != m_transferIds.constEnd()) {
        TransferRegistry::instance().updateProgress(idIt.value(), done, total);
    }

    QProgressDialog *progress = ensureProgressDialog();
    if (total > 0) {
        progress->setMaximum(100);
        progress->setValue(static_cast<int>(done * 100 / total));
    }
}

void SftpWidget::onTransferStep(const QString &path, int index, int count, const QString &file)
{
    Q_UNUSED(path)
    QProgressDialog *progress = ensureProgressDialog();
    progress->setLabelText(tr("Downloading %1 (%2/%3)").arg(file).arg(index).arg(count));
}

void SftpWidget::onTransferFinished(const QString &path, bool ok, const QString &message)
{
    m_transferInProgress = false;
    const auto idIt = m_transferIds.constFind(path);
    if (idIt != m_transferIds.constEnd()) {
        TransferRegistry::instance().finishTransfer(idIt.value(), ok, message);
        m_transferIds.erase(idIt);
    }

    if (m_progress) {
        m_progress->reset();
        m_progress->deleteLater();
        m_progress = nullptr;
    }
    if (!ok && message != tr("Cancelled")) {
        m_statusLabel->setText(tr("Transfer failed: %1").arg(message));
    } else {
        m_statusLabel->setText(tr("Transfer finished: %1").arg(path));
    }
    emit transferDone(path, ok);
    refresh();
}

void SftpWidget::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() && !m_currentPath.isEmpty()) {
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    QWidget::dragEnterEvent(event);
}

void SftpWidget::dropEvent(QDropEvent *event)
{
    for (const QUrl &url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QString localPath = url.toLocalFile();
        const QFileInfo info(localPath);
        if (info.isFile()) {
            const QString remotePath = remoteJoin(m_currentPath, info.fileName());
            trackTransfer(remotePath, TransferRegistry::Direction::Upload);
            m_sftp->upload(localPath, remotePath);
        }
    }
    event->acceptProposedAction();
}

QString SftpWidget::formatPermissions(quint32 mode, bool isDir)
{
    QString result;
    result += isDir ? QLatin1Char('d') : QLatin1Char('-');
    const auto triplet = [&result, mode](quint32 bits) {
        result += (bits & 4) ? QLatin1Char('r') : QLatin1Char('-');
        result += (bits & 2) ? QLatin1Char('w') : QLatin1Char('-');
        result += (bits & 1) ? QLatin1Char('x') : QLatin1Char('-');
    };
    triplet((mode >> 6) & 7);
    triplet((mode >> 3) & 7);
    triplet(mode & 7);
    return result;
}

} // namespace hssh

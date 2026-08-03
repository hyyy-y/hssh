#include "FileCompareWidget.h"

#include "app/dialogs/DiffDialog.h"
#include "app/widgets/LocalFileWidget.h"
#include "app/widgets/SftpWidget.h"
#include "core/SessionRepository.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QTableView>
#include <QTemporaryFile>
#include <QToolButton>
#include <QVBoxLayout>

namespace hssh {

namespace {
const QColor kOnlyLocalColor(0x4e, 0x9a, 0x51);   // green
const QColor kOnlyRemoteColor(0x37, 0x94, 0xff);  // blue
const QColor kDifferentColor(0xd1, 0x86, 0x16);   // orange
constexpr qint64 kMaxDiffFileSize = 2 * 1024 * 1024; // bytes
// mtimes within this window are considered equal (filesystem granularity).
constexpr qint64 kMtimeToleranceSecs = 120;

QString statusMarker(FileCompareWidget::CompareStatus status)
{
    switch (status) {
    case FileCompareWidget::CompareStatus::OnlyLocal:
        return QStringLiteral("→");
    case FileCompareWidget::CompareStatus::OnlyRemote:
        return QStringLiteral("←");
    case FileCompareWidget::CompareStatus::Different:
        return QStringLiteral("≠");
    case FileCompareWidget::CompareStatus::Same:
        return QStringLiteral("=");
    }
    return QString();
}

QColor statusColor(FileCompareWidget::CompareStatus status)
{
    switch (status) {
    case FileCompareWidget::CompareStatus::OnlyLocal:
        return kOnlyLocalColor;
    case FileCompareWidget::CompareStatus::OnlyRemote:
        return kOnlyRemoteColor;
    case FileCompareWidget::CompareStatus::Different:
        return kDifferentColor;
    case FileCompareWidget::CompareStatus::Same:
        return QColor();
    }
    return QColor();
}

QString sizeText(qint64 size)
{
    return size < 0 ? QString() : QLocale().formattedDataSize(size);
}

QString mtimeText(qint64 mtime)
{
    return mtime <= 0 ? QString()
                      : QDateTime::fromSecsSinceEpoch(mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}
} // namespace

FileCompareWidget::FileCompareWidget(const SessionConfig &config, SessionRepository *repo, QWidget *parent)
    : QWidget(parent)
    , m_config(config)
    , m_repo(repo)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    // Toolbar.
    auto *toolBar = new QHBoxLayout;
    const auto makeButton = [this](const QString &text, const QString &tooltip) {
        auto *button = new QToolButton(this);
        button->setText(text);
        button->setToolTip(tooltip);
        return button;
    };
    QToolButton *compareButton = makeButton(tr("⟳ Compare"), tr("Re-run the directory comparison"));
    QToolButton *uploadButton = makeButton(tr("⇪ Upload"), tr("Upload selected local files to the remote directory"));
    QToolButton *downloadButton = makeButton(tr("⇩ Download"), tr("Download selected remote files to the local directory"));
    QToolButton *diffButton = makeButton(tr("⇄ Diff"), tr("Diff the selected file pair"));
    QToolButton *folderCompareButton = makeButton(tr("≣ Folder compare"), tr("Recursively compare the local and remote directory trees"));
    m_statusLabel = new QLabel(this);

    toolBar->addWidget(compareButton);
    toolBar->addWidget(uploadButton);
    toolBar->addWidget(downloadButton);
    toolBar->addWidget(diffButton);
    toolBar->addWidget(folderCompareButton);
    toolBar->addStretch(1);
    toolBar->addWidget(m_statusLabel);
    layout->addLayout(toolBar);

    connect(compareButton, &QToolButton::clicked, this, &FileCompareWidget::runCompare);
    connect(uploadButton, &QToolButton::clicked, this, &FileCompareWidget::uploadSelected);
    connect(downloadButton, &QToolButton::clicked, this, &FileCompareWidget::downloadSelected);
    connect(diffButton, &QToolButton::clicked, this, &FileCompareWidget::showDiff);
    connect(folderCompareButton, &QToolButton::clicked, this, &FileCompareWidget::startFolderCompare);

    // Project bar: saved local/remote folder pairs for this session.
    auto *projectBar = new QHBoxLayout;
    m_projectCombo = new QComboBox(this);
    m_projectCombo->setMinimumWidth(220);
    m_projectCombo->setPlaceholderText(tr("Compare project"));
    QToolButton *saveProjectButton = makeButton(tr("☆ Save project"), tr("Save the current local/remote folder pair as a project"));
    QToolButton *removeProjectButton = makeButton(tr("✕"), tr("Delete the selected project"));
    projectBar->addWidget(m_projectCombo);
    projectBar->addWidget(saveProjectButton);
    projectBar->addWidget(removeProjectButton);
    projectBar->addStretch(1);
    layout->addLayout(projectBar);

    connect(m_projectCombo, &QComboBox::activated, this, &FileCompareWidget::onProjectActivated);
    connect(saveProjectButton, &QToolButton::clicked, this, &FileCompareWidget::saveProject);
    connect(removeProjectButton, &QToolButton::clicked, this, &FileCompareWidget::removeCurrentProject);

    // Stacked pages: browse (dual pane) vs folder-compare results.
    m_stack = new QStackedWidget(this);

    auto *browsePage = new QWidget(m_stack);
    auto *browseLayout = new QVBoxLayout(browsePage);
    browseLayout->setContentsMargins(0, 0, 0, 0);
    auto *splitter = new QSplitter(browsePage);
    m_local = new LocalFileWidget(splitter);
    m_remote = new SftpWidget(config, splitter);
    splitter->addWidget(m_local);
    splitter->addWidget(m_remote);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    browseLayout->addWidget(splitter);
    m_stack->addWidget(browsePage);

    // Folder compare page: filter row, path bars, mirrored result table and
    // an inline diff pane (reference-UI layout).
    auto *treePage = new QWidget(m_stack);
    auto *treeLayout = new QVBoxLayout(treePage);
    treeLayout->setContentsMargins(0, 0, 0, 0);
    treeLayout->setSpacing(2);

    auto *filterBar = new QHBoxLayout;
    QToolButton *backButton = makeButton(tr("← Browse"), tr("Back to the directory browser"));
    QToolButton *reCompareButton = makeButton(tr("⟳"), tr("Re-run the folder comparison"));
    m_hideSameCheck = new QCheckBox(tr("Hide identical"), treePage);
    m_filterEdit = new QLineEdit(treePage);
    m_filterEdit->setPlaceholderText(tr("Filter by name"));
    m_filterEdit->setClearButtonEnabled(true);
    m_filterEdit->setMaximumWidth(240);
    filterBar->addWidget(backButton);
    filterBar->addWidget(reCompareButton);
    filterBar->addWidget(m_hideSameCheck);
    filterBar->addStretch(1);
    filterBar->addWidget(new QLabel(tr("Filter:"), treePage));
    filterBar->addWidget(m_filterEdit);
    treeLayout->addLayout(filterBar);

    // Path bars: local on the left, remote on the right (mirrored layout).
    auto *pathBar = new QHBoxLayout;
    m_localPathEdit = new QLineEdit(treePage);
    m_localPathEdit->setPlaceholderText(tr("Local folder"));
    QToolButton *localBrowseButton = makeButton(tr("📁"), tr("Choose a local folder"));
    m_remotePathEdit = new QLineEdit(treePage);
    m_remotePathEdit->setPlaceholderText(tr("Remote folder"));
    QToolButton *remoteBrowseButton = makeButton(tr("📁"), tr("Pick the remote folder in the browser page"));
    pathBar->addWidget(m_localPathEdit, 1);
    pathBar->addWidget(localBrowseButton);
    pathBar->addSpacing(8);
    pathBar->addWidget(m_remotePathEdit, 1);
    pathBar->addWidget(remoteBrowseButton);
    treeLayout->addLayout(pathBar);

    auto *treeSplit = new QSplitter(Qt::Vertical, treePage);

    // Mirrored columns: local name/size/date | direction | remote date/size/name.
    m_treeModel = new QStandardItemModel(0, 7, this);
    m_treeModel->setHorizontalHeaderLabels({tr("Name"), tr("Size"), tr("Modified"), QStringLiteral("*"),
                                            tr("Modified"), tr("Size"), tr("Name")});
    m_treeView = new QTableView(treeSplit);
    m_treeView->setModel(m_treeModel);
    m_treeView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_treeView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_treeView->setShowGrid(false);
    m_treeView->verticalHeader()->setVisible(false);
    m_treeView->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_treeView->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    m_treeView->setSortingEnabled(false);
    m_treeView->setAlternatingRowColors(true);
    treeSplit->addWidget(m_treeView);

    m_diffView = new DiffView(treeSplit);
    m_diffView->setHint(tr("Double-click a file row to preview the diff"));
    treeSplit->addWidget(m_diffView);
    treeSplit->setStretchFactor(0, 3);
    treeSplit->setStretchFactor(1, 2);
    treeLayout->addWidget(treeSplit, 1);
    m_stack->addWidget(treePage);

    layout->addWidget(m_stack, 1);

    connect(backButton, &QToolButton::clicked, this, &FileCompareWidget::backToBrowse);
    connect(reCompareButton, &QToolButton::clicked, this, &FileCompareWidget::onTreePathsEdited);
    connect(m_localPathEdit, &QLineEdit::returnPressed, this, &FileCompareWidget::onTreePathsEdited);
    connect(m_remotePathEdit, &QLineEdit::returnPressed, this, &FileCompareWidget::onTreePathsEdited);
    connect(localBrowseButton, &QToolButton::clicked, this, [this]() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("Choose a local folder"), m_localPathEdit->text());
        if (!dir.isEmpty()) {
            m_localPathEdit->setText(QDir::toNativeSeparators(dir));
            onTreePathsEdited();
        }
    });
    connect(remoteBrowseButton, &QToolButton::clicked, this, &FileCompareWidget::backToBrowse);
    connect(m_filterEdit, &QLineEdit::textChanged, this, &FileCompareWidget::rebuildTreeModel);
    connect(m_hideSameCheck, &QCheckBox::toggled, this, &FileCompareWidget::rebuildTreeModel);
    connect(m_treeView, &QTableView::doubleClicked, this, &FileCompareWidget::onTreeActivated);

    // Re-compare whenever either side navigates or a transfer lands.
    connect(m_local, &LocalFileWidget::pathChanged, this, &FileCompareWidget::runCompare);
    connect(m_remote, &SftpWidget::dirChanged, this, &FileCompareWidget::runCompare);
    connect(m_remote, &SftpWidget::transferDone, this, [this](const QString &, bool ok) {
        if (ok) {
            m_local->refresh();
        }
    });

    // Diff viewer plumbing: temp download finished -> open the dialog.
    connect(m_remote, &SftpWidget::transferDone, this, &FileCompareWidget::onDiffDownloadDone);
    // Recursive listing for the folder compare page.
    connect(m_remote, &SftpWidget::dirTreeListed, this, &FileCompareWidget::onDirTreeListed);

    // Restore the most recently used project (navigates both panes). The
    // remote navigate queues behind the session's connect, so it lands once
    // the connection is up.
    reloadProjects(true);
}

FileCompareWidget::~FileCompareWidget() = default;

void FileCompareWidget::navigateRemote(const QString &path)
{
    m_remote->navigateTo(path);
}

QString FileCompareWidget::remoteJoin(const QString &dir, const QString &name) const
{
    if (dir.endsWith(QLatin1Char('/'))) {
        return dir + name;
    }
    return dir + QLatin1Char('/') + name;
}

void FileCompareWidget::runCompare()
{
    const QString localDir = m_local->currentPath();
    const QList<SftpFileInfo> remoteEntries = m_remote->entries();
    if (localDir.isEmpty() || remoteEntries.isEmpty()) {
        return;
    }

    QHash<QString, QFileInfo> localByName;
    const QFileInfoList localEntries =
        QDir(localDir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name | QDir::DirsFirst);
    for (const QFileInfo &info : localEntries) {
        localByName.insert(info.fileName(), info);
    }

    QHash<QString, QColor> localColors;
    QHash<QString, QColor> remoteColors;
    int onlyLocal = 0;
    int onlyRemote = 0;
    int different = 0;
    int same = 0;

    for (const SftpFileInfo &remote : remoteEntries) {
        const auto localIt = localByName.constFind(remote.name);
        if (localIt == localByName.constEnd()) {
            remoteColors.insert(remote.name, kOnlyRemoteColor);
            ++onlyRemote;
            continue;
        }
        const QFileInfo &local = localIt.value();
        if (local.isDir() != remote.isDir) {
            // Same name, different kind.
            localColors.insert(remote.name, kDifferentColor);
            remoteColors.insert(remote.name, kDifferentColor);
            ++different;
        } else if (remote.isDir) {
            // Directories compare by name only.
            ++same;
        } else if (local.size() != remote.size
                   || qAbs(local.lastModified().toSecsSinceEpoch() - remote.mtime) > kMtimeToleranceSecs) {
            localColors.insert(remote.name, kDifferentColor);
            remoteColors.insert(remote.name, kDifferentColor);
            ++different;
        } else {
            ++same;
        }
        localByName.erase(localIt);
    }
    for (auto it = localByName.constBegin(); it != localByName.constEnd(); ++it) {
        localColors.insert(it.key(), kOnlyLocalColor);
        ++onlyLocal;
    }

    m_local->setRowColors(localColors);
    m_remote->setRowColors(remoteColors);
    m_statusLabel->setText(tr("same %1 · different %2 · only local %3 · only remote %4")
                               .arg(same)
                               .arg(different)
                               .arg(onlyLocal)
                               .arg(onlyRemote));
}

void FileCompareWidget::uploadSelected()
{
    if (m_stack->currentIndex() == 1) {
        // Folder compare page: sync the selected rows (files only).
        const QModelIndexList rows = m_treeView->selectionModel()->selectedRows();
        for (const QModelIndex &index : rows) {
            const int row = index.data(Qt::UserRole).toInt();
            if (row < 0 || row >= m_rows.size()) {
                continue;
            }
            const CompareRow &entry = m_rows.at(row);
            if (entry.isDir || entry.status == CompareStatus::OnlyRemote) {
                continue;
            }
            const QString localPath = QDir(m_treeLocalRoot).filePath(entry.relPath);
            m_remote->uploadTo(localPath, remoteJoin(m_treeRemoteRoot, entry.relPath));
        }
        return;
    }

    const QString remoteDir = m_remote->currentPath();
    if (remoteDir.isEmpty()) {
        return;
    }
    const QStringList paths = m_local->selectedPaths();
    for (const QString &localPath : paths) {
        const QFileInfo info(localPath);
        if (info.isFile()) {
            m_remote->uploadTo(localPath, remoteJoin(remoteDir, info.fileName()));
        }
    }
}

void FileCompareWidget::downloadSelected()
{
    if (m_stack->currentIndex() == 1) {
        const QModelIndexList rows = m_treeView->selectionModel()->selectedRows();
        for (const QModelIndex &index : rows) {
            const int row = index.data(Qt::UserRole).toInt();
            if (row < 0 || row >= m_rows.size()) {
                continue;
            }
            const CompareRow &entry = m_rows.at(row);
            if (entry.status == CompareStatus::OnlyLocal) {
                continue;
            }
            const QString remotePath = remoteJoin(m_treeRemoteRoot, entry.relPath);
            const QString localPath = QDir(m_treeLocalRoot).filePath(entry.relPath);
            if (entry.isDir) {
                // downloadTo is single-file; directories need the recursive API,
                // which the SFTP pane does not expose for arbitrary targets —
                // skip them here (use the SFTP tab's Download… for folders).
                continue;
            }
            m_remote->downloadTo(remotePath, localPath);
        }
        return;
    }

    const QString localDir = m_local->currentPath();
    const QString remoteDir = m_remote->currentPath();
    if (localDir.isEmpty() || remoteDir.isEmpty()) {
        return;
    }
    for (const SftpFileInfo &entry : m_remote->selectedEntries()) {
        if (!entry.isDir) {
            m_remote->downloadTo(remoteJoin(remoteDir, entry.name),
                                 QDir(localDir).filePath(entry.name));
        }
    }
}

void FileCompareWidget::showDiff()
{
    // Pair selection: prefer one local file + one remote file; otherwise a
    // local file whose name also exists remotely.
    QString localPath;
    QString remotePath;
    QString remoteName;

    const QStringList localSelection = m_local->selectedPaths();
    const QList<SftpFileInfo> remoteSelection = m_remote->selectedEntries();
    for (const QString &path : localSelection) {
        if (QFileInfo(path).isFile()) {
            localPath = path;
            break;
        }
    }
    for (const SftpFileInfo &entry : remoteSelection) {
        if (!entry.isDir) {
            remoteName = entry.name;
            remotePath = remoteJoin(m_remote->currentPath(), entry.name);
            break;
        }
    }
    if (localPath.isEmpty() && !remoteName.isEmpty()) {
        const QString candidate = QDir(m_local->currentPath()).filePath(remoteName);
        if (QFileInfo::exists(candidate)) {
            localPath = candidate;
        }
    }
    if (remotePath.isEmpty() && !localPath.isEmpty()) {
        const QString name = QFileInfo(localPath).fileName();
        for (const SftpFileInfo &entry : m_remote->entries()) {
            if (!entry.isDir && entry.name == name) {
                remoteName = name;
                remotePath = remoteJoin(m_remote->currentPath(), name);
                break;
            }
        }
    }
    if (localPath.isEmpty() || remotePath.isEmpty()) {
        QMessageBox::information(this, tr("Diff"),
                                 tr("Select a local file and a remote file (or one file that exists on both sides)."));
        return;
    }

    const qint64 localSize = QFileInfo(localPath).size();
    qint64 remoteSize = -1;
    for (const SftpFileInfo &entry : m_remote->entries()) {
        if (entry.name == remoteName) {
            remoteSize = entry.size;
            break;
        }
    }
    if (localSize > kMaxDiffFileSize || remoteSize > kMaxDiffFileSize) {
        QMessageBox::information(this, tr("Diff"),
                                 tr("Files larger than 2 MB are not supported by the diff viewer."));
        return;
    }

    // Download the remote side to a temp file; the dialog opens when the
    // transfer completes (see onDiffDownloadDone).
    if (m_diffTemp) {
        QMessageBox::information(this, tr("Diff"), tr("A diff download is already in progress."));
        return;
    }
    m_diffTemp = new QTemporaryFile(QDir::tempPath() + QStringLiteral("/hssh_diff_XXXXXX"), this);
    if (!m_diffTemp->open()) {
        QMessageBox::warning(this, tr("Diff"), tr("Failed to create a temporary file."));
        delete m_diffTemp;
        m_diffTemp = nullptr;
        return;
    }
    m_diffTemp->close();
    m_diffRemotePath = remotePath;
    m_diffLocalPath = localPath;
    m_statusLabel->setText(tr("Downloading %1 for diff…").arg(remoteName));
    m_remote->downloadTo(remotePath, m_diffTemp->fileName());
}

void FileCompareWidget::reloadProjects(bool restoreFirst)
{
    m_projectCombo->clear();
    if (!m_repo) {
        return;
    }

    const QList<SessionRepository::CompareProject> projects = m_repo->compareProjects(m_config.id());
    for (const auto &project : projects) {
        m_projectCombo->addItem(project.name);
        const int row = m_projectCombo->count() - 1;
        m_projectCombo->setItemData(row, project.id, Qt::UserRole);
        m_projectCombo->setItemData(row, project.localPath, Qt::UserRole + 1);
        m_projectCombo->setItemData(row, project.remotePath, Qt::UserRole + 2);
    }

    if (restoreFirst && m_projectCombo->count() > 0) {
        m_projectCombo->setCurrentIndex(0);
        m_local->navigateTo(m_projectCombo->itemData(0, Qt::UserRole + 1).toString());
        m_remote->navigateTo(m_projectCombo->itemData(0, Qt::UserRole + 2).toString());
        m_repo->touchCompareProject(m_projectCombo->itemData(0, Qt::UserRole).toString());
    }
}

void FileCompareWidget::saveProject()
{
    if (!m_repo) {
        return;
    }
    const QString localDir = m_local->currentPath();
    const QString remoteDir = m_remote->currentPath();
    if (localDir.isEmpty() || remoteDir.isEmpty()) {
        m_statusLabel->setText(tr("Open a local folder and a remote folder first."));
        return;
    }

    const QString localName = QDir(localDir).dirName();
    QString remoteName = remoteDir.section(QLatin1Char('/'), -1);
    if (remoteName.isEmpty()) {
        remoteName = QStringLiteral("/");
    }
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save project"), tr("Project name:"),
                                               QLineEdit::Normal,
                                               QStringLiteral("%1 ↔ %2").arg(localName, remoteName),
                                               &ok);
    if (!ok || name.trimmed().isEmpty()) {
        return;
    }

    const QString id = m_repo->saveCompareProject(m_config.id(), name.trimmed(), localDir, remoteDir);
    if (id.isEmpty()) {
        m_statusLabel->setText(tr("Failed to save the project."));
        return;
    }
    reloadProjects(false);
    const int row = m_projectCombo->findData(id, Qt::UserRole);
    if (row >= 0) {
        m_projectCombo->setCurrentIndex(row);
    }
    m_statusLabel->setText(tr("Project saved."));
}

void FileCompareWidget::removeCurrentProject()
{
    if (!m_repo || m_projectCombo->currentIndex() < 0) {
        return;
    }
    const QString id = m_projectCombo->currentData(Qt::UserRole).toString();
    if (id.isEmpty()) {
        return;
    }
    if (QMessageBox::question(this, tr("Delete project"),
                              tr("Delete project '%1'?").arg(m_projectCombo->currentText()))
        != QMessageBox::Yes) {
        return;
    }
    m_repo->removeCompareProject(id);
    reloadProjects(false);
}

void FileCompareWidget::onProjectActivated(int index)
{
    if (!m_repo || index < 0) {
        return;
    }
    const QString localPath = m_projectCombo->itemData(index, Qt::UserRole + 1).toString();
    const QString remotePath = m_projectCombo->itemData(index, Qt::UserRole + 2).toString();
    if (localPath.isEmpty() || remotePath.isEmpty()) {
        return;
    }
    m_local->navigateTo(localPath);
    m_remote->navigateTo(remotePath);
    m_repo->touchCompareProject(m_projectCombo->itemData(index, Qt::UserRole).toString());
    backToBrowse();
}

void FileCompareWidget::startFolderCompare()
{
    const QString localDir = m_local->currentPath();
    const QString remoteDir = m_remote->currentPath();
    if (localDir.isEmpty() || remoteDir.isEmpty()) {
        m_statusLabel->setText(tr("Open a local folder and a remote folder first."));
        return;
    }

    m_treeLocalRoot = localDir;
    m_treeRemoteRoot = remoteDir;
    m_rows.clear();

    // Local side walks instantly; the remote walk arrives via dirTreeListed.
    m_localTreeRows.clear();
    QDirIterator it(localDir, QDir::AllEntries | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    const QDir root(localDir);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        CompareRow row;
        row.relPath = root.relativeFilePath(info.filePath());
        row.isDir = info.isDir();
        row.localSize = info.isDir() ? 0 : info.size();
        row.localMtime = info.lastModified().toSecsSinceEpoch();
        m_localTreeRows.insert(row.relPath, row);
    }

    m_statusLabel->setText(tr("Scanning remote directory…"));
    m_remote->listDirTree(remoteDir);
}

void FileCompareWidget::onDirTreeListed(const QString &path, const QList<RemoteFileEntry> &entries)
{
    if (path != m_treeRemoteRoot || m_treeLocalRoot.isEmpty()) {
        return; // stale result from an earlier browse directory
    }

    QHash<QString, CompareRow> remaining = m_localTreeRows;
    m_rows.clear();
    int same = 0;
    int different = 0;
    int onlyLocal = 0;
    int onlyRemote = 0;

    for (const RemoteFileEntry &remote : entries) {
        CompareRow row;
        row.relPath = remote.relPath;
        row.isDir = remote.isDir;
        row.remoteSize = remote.isDir ? 0 : remote.size;
        row.remoteMtime = remote.mtime;

        const auto localIt = remaining.find(remote.relPath);
        if (localIt == remaining.end()) {
            row.status = CompareStatus::OnlyRemote;
            row.localSize = -1;
            ++onlyRemote;
        } else {
            const CompareRow local = localIt.value();
            row.localSize = local.localSize;
            row.localMtime = local.localMtime;
            if (local.isDir != remote.isDir) {
                row.status = CompareStatus::Different;
                ++different;
            } else if (remote.isDir) {
                row.status = CompareStatus::Same;
                ++same;
            } else if (local.localSize != remote.size
                       || qAbs(local.localMtime - remote.mtime) > kMtimeToleranceSecs) {
                row.status = CompareStatus::Different;
                ++different;
            } else {
                row.status = CompareStatus::Same;
                ++same;
            }
            remaining.erase(localIt);
        }
        m_rows.append(row);
    }
    for (auto it = remaining.begin(); it != remaining.end(); ++it) {
        CompareRow row = it.value();
        row.status = CompareStatus::OnlyLocal;
        row.remoteSize = -1;
        m_rows.append(row);
        ++onlyLocal;
    }

    std::sort(m_rows.begin(), m_rows.end(), [](const CompareRow &a, const CompareRow &b) {
        return QString::compare(a.relPath, b.relPath, Qt::CaseInsensitive) < 0;
    });

    rebuildTreeModel();
    m_stack->setCurrentIndex(1);
    m_statusLabel->setText(tr("same %1 · different %2 · only local %3 · only remote %4")
                               .arg(same)
                               .arg(different)
                               .arg(onlyLocal)
                               .arg(onlyRemote));
}

void FileCompareWidget::rebuildTreeModel()
{
    m_treeModel->removeRows(0, m_treeModel->rowCount());
    const QString filter = m_filterEdit->text().trimmed();
    const bool hideSame = m_hideSameCheck->isChecked();

    for (int i = 0; i < m_rows.size(); ++i) {
        const CompareRow &row = m_rows.at(i);
        if (hideSame && row.status == CompareStatus::Same) {
            continue;
        }
        if (!filter.isEmpty() && !row.relPath.contains(filter, Qt::CaseInsensitive)) {
            continue;
        }

        auto *nameItem = new QStandardItem((row.isDir ? QStringLiteral("📁 ") : QString()) + row.relPath);
        nameItem->setData(i, Qt::UserRole);
        auto *statusItem = new QStandardItem(statusMarker(row.status));
        statusItem->setTextAlignment(Qt::AlignCenter);
        auto *localSizeItem = new QStandardItem(row.isDir ? QString() : sizeText(row.localSize));
        auto *remoteSizeItem = new QStandardItem(row.isDir ? QString() : sizeText(row.remoteSize));
        auto *localMtimeItem = new QStandardItem(mtimeText(row.localMtime));
        auto *remoteMtimeItem = new QStandardItem(mtimeText(row.remoteMtime));
        if (row.localSize < 0) { // absent on this side
            localSizeItem->setText(QString());
            localMtimeItem->setText(QString());
        }
        if (row.remoteSize < 0) {
            remoteSizeItem->setText(QString());
            remoteMtimeItem->setText(QString());
        }

        const QColor color = statusColor(row.status);
        if (color.isValid()) {
            nameItem->setForeground(color);
            statusItem->setForeground(color);
        }
        m_treeModel->appendRow({nameItem, statusItem, localSizeItem, remoteSizeItem,
                                localMtimeItem, remoteMtimeItem});
    }
}

void FileCompareWidget::onTreeActivated(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }
    const int row = index.data(Qt::UserRole).toInt();
    if (row < 0 || row >= m_rows.size()) {
        return;
    }
    const CompareRow &entry = m_rows.at(row);
    // Diffing needs the file on both sides.
    if (entry.isDir || entry.status == CompareStatus::OnlyLocal
        || entry.status == CompareStatus::OnlyRemote) {
        return;
    }
    if (entry.localSize > kMaxDiffFileSize || entry.remoteSize > kMaxDiffFileSize) {
        QMessageBox::information(this, tr("Diff"),
                                 tr("Files larger than 2 MB are not supported by the diff viewer."));
        return;
    }
    if (m_diffTemp) {
        QMessageBox::information(this, tr("Diff"), tr("A diff download is already in progress."));
        return;
    }

    m_diffTemp = new QTemporaryFile(QDir::tempPath() + QStringLiteral("/hssh_diff_XXXXXX"), this);
    if (!m_diffTemp->open()) {
        QMessageBox::warning(this, tr("Diff"), tr("Failed to create a temporary file."));
        delete m_diffTemp;
        m_diffTemp = nullptr;
        return;
    }
    m_diffTemp->close();
    m_diffRemotePath = remoteJoin(m_treeRemoteRoot, entry.relPath);
    m_diffLocalPath = QDir(m_treeLocalRoot).filePath(entry.relPath);
    m_statusLabel->setText(tr("Downloading %1 for diff…").arg(entry.relPath));
    m_remote->downloadTo(m_diffRemotePath, m_diffTemp->fileName());
}

void FileCompareWidget::backToBrowse()
{
    m_stack->setCurrentIndex(0);
}

void FileCompareWidget::onDiffDownloadDone(const QString &remotePath, bool ok)
{
    if (!m_diffTemp || remotePath != m_diffRemotePath) {
        return;
    }

    const QString localPath = m_diffLocalPath;
    QString tempPath = m_diffTemp->fileName();
    // Keep the temp file alive until the dialog has read it; the guard below
    // deletes it right after loading.
    m_diffTemp->setAutoRemove(false);
    m_diffTemp->deleteLater();
    m_diffTemp = nullptr;
    m_diffRemotePath.clear();
    m_diffLocalPath.clear();

    if (!ok) {
        QFile::remove(tempPath);
        m_statusLabel->setText(tr("Diff download failed."));
        return;
    }

    const auto readText = [](const QString &path, QString *text) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return false;
        }
        *text = QString::fromUtf8(file.readAll());
        return true;
    };
    QString localText;
    QString remoteText;
    const bool localOk = readText(localPath, &localText);
    const bool remoteOk = readText(tempPath, &remoteText);
    QFile::remove(tempPath);
    if (!localOk || !remoteOk) {
        QMessageBox::warning(this, tr("Diff"), tr("Failed to read one of the files."));
        return;
    }

    auto *dialog = new DiffDialog(tr("Local: %1").arg(QDir::toNativeSeparators(localPath)), localText,
                                  tr("Remote: %1").arg(remotePath), remoteText,
                                  this);
    dialog->show();
    m_statusLabel->setText(tr("Diff ready."));
}

} // namespace hssh

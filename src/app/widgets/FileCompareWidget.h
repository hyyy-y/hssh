#ifndef HSSH_APP_WIDGETS_FILECOMPAREWIDGET_H
#define HSSH_APP_WIDGETS_FILECOMPAREWIDGET_H

#include "core/SessionConfig.h"
#include "core/SftpSession.h"

#include <QHash>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QStandardItem;
class QStandardItemModel;
class QStackedWidget;
class QTemporaryFile;
class QTreeView;
QT_END_NAMESPACE

namespace hssh {

class DiffView;
class LocalFileWidget;
class SessionRepository;
class SftpWidget;

// Dual-pane local/remote compare tab: local filesystem on the left, remote
// SFTP listing on the right. Rows are colored by comparison status and a
// selected file pair can be opened in the diff viewer. A second "folder
// compare" page shows the recursive tree comparison as one aligned list
// with direction markers. Local/remote folder pairs can be saved as
// projects (rooted at the session) and are restored automatically.
class FileCompareWidget : public QWidget {
    Q_OBJECT

public:
    explicit FileCompareWidget(const SessionConfig &config, SessionRepository *repo,
                               QWidget *parent = nullptr);
    ~FileCompareWidget() override;

    // Point the remote pane at a directory (e.g. from an SFTP tab's context
    // menu). No-op while the session is still connecting.
    void navigateRemote(const QString &path);

    // IDEA-style per-row sync action: the arrow column shows and edits the
    // action (click or Space to cycle → / ← / ⊘).
    enum class SyncAction {
        Upload,   // copy to remote (→)
        Download, // copy to local (←)
        Skip      // do nothing (⊘)
    };

    enum class CompareStatus { Same, Different, OnlyLocal, OnlyRemote };

private:
    // --- browse page (shallow, two panes) ---
    void runCompare();
    void uploadSelected();
    void downloadSelected();
    void showDiff();
    void onDiffDownloadDone(const QString &remotePath, bool ok);

    // --- projects ---
    void reloadProjects(bool restoreFirst = false);
    void saveProject();
    void removeCurrentProject();
    void onProjectActivated(int index);

    // --- folder compare page (recursive, merged list) ---
    void startFolderCompare();
    void startFolderCompare(const QString &localDir, const QString &remoteDir);
    void onTreePathsEdited();
    void onDirTreeListed(const QString &path, const QList<RemoteFileEntry> &entries);
    void onDirTreeProgress(const QString &path, int entriesScanned);
    void rebuildTreeModel();
    void onTreeActivated(const QModelIndex &index);
    void onTreeContextMenu(const QPoint &pos);
    void cycleRowAction(const QModelIndex &index);
    // Builds the 7 mirrored columns for one tree node (file or folder).
    QList<QStandardItem *> makeTreeRowItems(int rowIndex, const QString &displayName) const;
    // Maps any cell index to the CompareRow index (UserRole lives on column 0).
    [[nodiscard]] int compareRowFromIndex(const QModelIndex &index) const;
    bool eventFilter(QObject *watched, QEvent *event) override;
    // Sync selected rows (or every differing row when nothing is selected)
    // in one direction, with one batch confirmation. Only files are synced;
    // directories are created implicitly by the transfers.
    void syncSelection(bool toRemote);
    void startSyncBatch(const QList<int> &targets, bool toRemote);
    void pumpSyncQueue();
    void backToBrowse();

    [[nodiscard]] QString remoteJoin(const QString &dir, const QString &name) const;

    struct CompareRow {
        QString relPath;
        bool isDir = false;
        CompareStatus status = CompareStatus::Same;
        SyncAction action = SyncAction::Skip;
        qint64 localSize = -1;  // -1 = absent
        qint64 remoteSize = -1;
        qint64 localMtime = 0;
        qint64 remoteMtime = 0;
    };

    SessionConfig m_config;
    SessionRepository *m_repo = nullptr;
    LocalFileWidget *m_local = nullptr;
    SftpWidget *m_remote = nullptr;
    QLabel *m_statusLabel = nullptr;
    QComboBox *m_projectCombo = nullptr;

    // Folder compare page state.
    QStackedWidget *m_stack = nullptr;
    QTreeView *m_treeView = nullptr;
    QStandardItemModel *m_treeModel = nullptr;
    QLineEdit *m_filterEdit = nullptr;
    QCheckBox *m_hideSameCheck = nullptr;
    QLineEdit *m_localPathEdit = nullptr;
    QLineEdit *m_remotePathEdit = nullptr;
    QProgressBar *m_analysisProgress = nullptr;
    DiffView *m_diffView = nullptr;
    QString m_treeLocalRoot;
    QString m_treeRemoteRoot;
    QList<CompareRow> m_rows;
    QHash<QString, CompareRow> m_localTreeRows; // relPath -> local side of the walk

    // Pending diff download: remote path -> temp file holding the content.
    QTemporaryFile *m_diffTemp = nullptr;
    QString m_diffRemotePath;
    QString m_diffLocalPath;
    bool m_diffInline = false;     // true: result goes to the inline DiffView
    bool m_diffRemoteOnly = false; // true: the row exists only on the remote side

    // Sync batch state: rows are transferred in small timed batches so the
    // GUI stays responsive even with tens of thousands of files.
    QList<int> m_syncQueue;
    bool m_syncToRemote = false;
    int m_syncTotal = 0;
    int m_syncDone = 0;
    QTimer *m_syncTimer = nullptr;
};

} // namespace hssh

#endif // HSSH_APP_WIDGETS_FILECOMPAREWIDGET_H

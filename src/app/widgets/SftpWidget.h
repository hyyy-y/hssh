#ifndef HSSH_APP_WIDGETS_SFTPWIDGET_H
#define HSSH_APP_WIDGETS_SFTPWIDGET_H

#include "core/ChannelCopySession.h"
#include "core/SessionConfig.h"
#include "core/SftpSession.h"
#include "app/TransferRegistry.h"

#include <QColor>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QProgressDialog;
class QStandardItemModel;
class QTableView;
class QComboBox;
class QToolButton;
QT_END_NAMESPACE

namespace hssh {

// Remote file browser for one SSH session: directory listing, navigation,
// upload/download, mkdir/rename/delete.
class SftpWidget : public QWidget {
    Q_OBJECT

public:
    explicit SftpWidget(const SessionConfig &config, QWidget *parent = nullptr);
    ~SftpWidget() override;

    [[nodiscard]] SessionConfig config() const { return m_config; }

    // Compare-pane support.
    [[nodiscard]] QString currentPath() const { return m_currentPath; }
    [[nodiscard]] QList<SftpFileInfo> entries() const { return m_entries; }
    void navigateTo(const QString &path);
    // Download without a save dialog (used by the diff viewer).
    void downloadTo(const QString &remotePath, const QString &localPath);
    // Upload without a file dialog (used by the compare pane).
    void uploadTo(const QString &localPath, const QString &remotePath);
    // Recursive remote tree listing (folder compare); result via dirTreeListed.
    void listDirTree(const QString &path);
    // mkdir -p for a remote path (parent directories created as needed).
    void ensureRemoteDir(const QString &path);
    // Compare highlighting: filename -> text color; empty map clears.
    void setRowColors(const QHash<QString, QColor> &colors);
    [[nodiscard]] QList<SftpFileInfo> selectedEntries() const;

signals:
    // Emitted from the context menu: user wants a local/remote compare tab.
    void compareRequested();
    // Emitted after each successful directory listing.
    void dirChanged(const QString &path);
    // Emitted when any upload/download on this widget completes.
    void transferDone(const QString &remotePath, bool ok);
    // Forwarded from the session for listDirTree().
    void dirTreeListed(const QString &path, const QList<hssh::RemoteFileEntry> &entries);
    // Forwarded progress during a recursive tree walk.
    void dirTreeProgress(const QString &path, int entriesScanned);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void refresh();
    void goUp();
    void goHome();
    void uploadFiles();
    // Confirmation dialog, then download: a lone file goes through the
    // save-as dialog, anything else asks for a destination directory
    // (directories download recursively).
    void confirmAndDownload(const QList<SftpFileInfo> &entries);
    void downloadEntry(const SftpFileInfo &entry);
    void mkdirDialog();
    void renameEntry(const SftpFileInfo &entry);
    void deleteEntries(const QList<SftpFileInfo> &entries);
    // PH2-09: the selected transfer channel ("sftp"/"scp") and the SCP route
    // (a parentless ChannelCopySession worker, freed on finish).
    [[nodiscard]] QString transferMethod() const;
    void startCopyTransfer(bool isUpload, const QString &remotePath, const QString &localPath);

    void onConnected(const QString &homePath);
    void onDirListed(const QString &path, const QList<SftpFileInfo> &entries);
    void onEntryActivated(const QModelIndex &index);
    void onContextMenu(const QPoint &pos);
    void onOperationFinished(const QString &operation, bool ok, const QString &message);
    void onTransferProgress(const QString &path, qint64 done, qint64 total);
    void onTransferStep(const QString &path, int index, int count, const QString &file);
    void onTransferFinished(const QString &path, bool ok, const QString &message);

    [[nodiscard]] QString remoteJoin(const QString &dir, const QString &name) const;
    // quiet=true for compare/sync-driven transfers: no per-file progress
    // dialog, the Transfers panel carries the progress display.
    void trackTransfer(const QString &remotePath, TransferRegistry::Direction direction, bool quiet = false);
    QProgressDialog *ensureProgressDialog();
    static QString formatPermissions(quint32 mode, bool isDir);

    SessionConfig m_config;
    SftpSession *m_sftp = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QToolButton *m_uploadButton = nullptr;
    QComboBox *m_methodBox = nullptr;          // PH2-09: SFTP/SCP picker
    QPointer<class ChannelCopySession> m_copyWorker; // PH2-09: active SCP worker
    QTableView *m_view = nullptr;
    QStandardItemModel *m_model = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressDialog *m_progress = nullptr;

    QString m_currentPath;
    QString m_homePath;
    QList<SftpFileInfo> m_entries; // row-aligned with the model
    QHash<QString, QColor> m_rowColors;
    QHash<QString, int> m_transferIds; // remote path -> TransferRegistry id
    QSet<int> m_quietTransfers;        // transfer ids that never pop the progress dialog
    bool m_transferInProgress = false;
};

} // namespace hssh

#endif // HSSH_APP_WIDGETS_SFTPWIDGET_H

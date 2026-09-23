#ifndef HSSH_APP_REMOTEEDITMANAGER_H
#define HSSH_APP_REMOTEEDITMANAGER_H

#include <QHash>
#include <QObject>
#include <QString>

class QFileSystemWatcher;
class QTimer;

namespace hssh {

// PH3-11: app-level registry of in-flight remote edits ("Edit" in the SFTP
// browser: download to %TEMP%, open in the system editor, watch saves,
// upload back). A process-wide singleton because the SAME session can be
// open in several SFTP tabs — the sessionKey+remotePath pair is the
// concurrency lock that keeps two editors from clobbering one file.
//
// The manager never talks SFTP itself: widgets download/upload through their
// own SftpSession and drive the manager with beginWatch()/markSynced().
class RemoteEditManager : public QObject {
    Q_OBJECT

public:
    static RemoteEditManager &instance();

    // Register an edit and take the lock. Returns true when registration
    // succeeded; on conflict `conflict` names the holder ("user@host:port
    // remote/path") and nothing is registered.
    bool startEdit(const QString &sessionKey, const QString &remotePath,
                   const QString &localPath, quint32 permissions,
                   QString *conflict = nullptr);
    // Start watching after the initial download finished — watching during
    // the download itself would fire on the downloader's own writes.
    void beginWatch(const QString &localPath);
    // Record the mtime of the last synced content so identical mtimes never
    // trigger an upload (also called right after beginWatch).
    void markSynced(const QString &localPath);
    [[nodiscard]] bool isEdited(const QString &sessionKey, const QString &remotePath) const;
    [[nodiscard]] QString localPathFor(const QString &sessionKey,
                                       const QString &remotePath) const;
    // Stop one edit (keeps the local temp file; the editor may hold it).
    void endEdit(const QString &sessionKey, const QString &remotePath);
    // Stop every edit of one session (SFTP tab closing).
    void endEditsForSession(const QString &sessionKey);

signals:
    // The editor saved the local copy (debounced, mtime changed): the owning
    // widget uploads localPath back to remotePath and re-applies
    // `permissions` afterwards.
    void editSaved(const QString &sessionKey, const QString &remotePath,
                   const QString &localPath, quint32 permissions);
    // The local copy disappeared (deleted / moved by the editor): watching
    // stopped, the entry is gone.
    void editGone(const QString &sessionKey, const QString &remotePath);

private:
    explicit RemoteEditManager(QObject *parent = nullptr);

    struct Edit {
        QString sessionKey;
        QString remotePath;
        quint32 permissions = 0;
        qint64 syncedMsec = 0; // last mtime known to match the remote
        bool watching = false;
        bool dirty = false;    // changed since the last flush
    };

    void onFileChanged(const QString &localPath);
    void flushDirty();

    // Keyed by localPath (the watcher's vocabulary).
    QHash<QString, Edit> m_edits;
    QFileSystemWatcher *m_watcher = nullptr;
    QTimer *m_debounce = nullptr;
};

} // namespace hssh

#endif // HSSH_APP_REMOTEEDITMANAGER_H

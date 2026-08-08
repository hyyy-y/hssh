#ifndef HSSH_CORE_SFTPSESSION_H
#define HSSH_CORE_SFTPSESSION_H

#include "SessionConfig.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>

using ssh_session = struct ssh_session_struct *;
using sftp_session = struct sftp_session_struct *;

QT_BEGIN_NAMESPACE
class QFile;
QT_END_NAMESPACE

namespace hssh {

struct SftpFileInfo {
    QString name;
    qint64 size = 0;
    qint64 mtime = 0;        // seconds since epoch
    quint32 permissions = 0; // unix mode bits
    bool isDir = false;
};

// One entry of a recursive directory walk (used by directory downloads and
// the folder compare view).
struct RemoteFileEntry {
    QString remotePath;      // absolute path on the server
    QString relPath;         // path relative to the walked root, '/'-separated
    qint64 size = 0;
    qint64 mtime = 0;        // seconds since epoch
    bool isDir = false;
};

// SFTP client living on its own thread with its own SSH connection (libssh
// sessions are not thread-safe, so the shell channel's ssh_session is never
// shared). All public request methods are queued onto the worker thread;
// results arrive via signals.
class SftpSession : public QObject {
    Q_OBJECT

public:
    explicit SftpSession(const SessionConfig &config, QObject *parent = nullptr);
    ~SftpSession() override;

    // Spin up the worker thread and connect asynchronously.
    void start();
    // Cancel any running transfer, stop the thread, close the connection.
    void stop();

    void listDir(const QString &path);
    // Recursive walk of a remote directory tree (folder compare). Results
    // include directories as well as files.
    void listDirRecursive(const QString &path);
    void canonicalize(const QString &path);
    void makeDir(const QString &path);
    void renameEntry(const QString &oldPath, const QString &newPath);
    void removeFile(const QString &path);
    void removeDir(const QString &path);
    void download(const QString &remotePath, const QString &localPath);
    // Recursively download a remote directory into localDir (created if
    // missing). Progress/finished signals use remotePath as the key, with
    // byte counters cumulative across all files in the tree.
    void downloadDir(const QString &remotePath, const QString &localDir);
    void upload(const QString &localPath, const QString &remotePath);
    void cancelTransfer();

signals:
    void connected(const QString &homePath);
    void errorOccurred(const QString &message);
    void dirListed(const QString &path, const QList<hssh::SftpFileInfo> &entries);
    void dirTreeListed(const QString &path, const QList<hssh::RemoteFileEntry> &entries);
    // Periodic progress during a recursive tree walk (compare analysis).
    void dirTreeProgress(const QString &path, int entriesScanned);
    void canonicalized(const QString &path, const QString &canonicalPath);
    void operationFinished(const QString &operation, bool ok, const QString &message);
    void transferProgress(const QString &path, qint64 bytesDone, qint64 bytesTotal);
    // Directory downloads: which file is currently being transferred
    // (1-based index), so the UI can show "name (3/12)".
    void transferStep(const QString &path, int fileIndex, int fileCount, const QString &currentFile);
    void transferFinished(const QString &path, bool ok, const QString &message);

private:
    void doConnect();
    void doListDir(const QString &path);
    void doCanonicalize(const QString &path);
    void doMakeDir(const QString &path);
    void doRenameEntry(const QString &oldPath, const QString &newPath);
    void doRemoveFile(const QString &path);
    void doRemoveDir(const QString &path);
    void doDownload(const QString &remotePath, const QString &localPath);
    void doDownloadDir(const QString &remotePath, const QString &localDir);
    void doUpload(const QString &localPath, const QString &remotePath);
    void doListDirRecursive(const QString &path);

    // Recursive walk collecting entries under remoteDir. Files are always
    // collected; directories only when includeDirs is set (compare needs
    // them, downloads don't).
    bool collectRemoteFiles(const QString &remoteDir, const QString &relDir,
                            QList<RemoteFileEntry> &out, QString &error,
                            bool includeDirs = false);
    // Streams one remote file into an already-open local file; `done` is the
    // transfer-wide cumulative byte counter reported under `key`.
    bool streamDownload(const QString &remoteFile, QFile &local, const QString &key,
                        qint64 &done, qint64 total, QString &error);

    void doCleanup();
    [[nodiscard]] QString sftpError() const;

    SessionConfig m_config;
    QThread m_thread;
    ssh_session m_ssh = nullptr;
    sftp_session m_sftp = nullptr;
    std::atomic<bool> m_cancelTransfer{false};
    // Running counter for recursive tree walks (progress reporting).
    int m_treeWalkCount = 0;
    QString m_treeWalkPath;
};

} // namespace hssh

Q_DECLARE_METATYPE(hssh::SftpFileInfo)
Q_DECLARE_METATYPE(hssh::RemoteFileEntry)

#endif // HSSH_CORE_SFTPSESSION_H

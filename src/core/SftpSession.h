#ifndef HSSH_CORE_SFTPSESSION_H
#define HSSH_CORE_SFTPSESSION_H

#include "SessionConfig.h"
#include "TransferSession.h"

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
class SftpSession : public TransferSession {
    Q_OBJECT

public:
    explicit SftpSession(const SessionConfig &config, QObject *parent = nullptr);
    ~SftpSession() override;

    // Spin up the worker thread and connect asynchronously.
    void start() override;
    // Cancel any running transfer, stop the thread, close the connection.
    void stop() override;

    void listDir(const QString &path);
    // Recursive walk of a remote directory tree (folder compare). Results
    // include directories as well as files.
    void listDirRecursive(const QString &path);
    void canonicalize(const QString &path);
    void makeDir(const QString &path);
    void renameEntry(const QString &oldPath, const QString &newPath);
    void removeFile(const QString &path);
    void removeDir(const QString &path);
    // Downloads remotePath to localPath. When verify is set (default) the
    // downloaded content is checked against the remote (size re-stat + md5)
    // and a mismatch retries once with a FULL re-download over a fresh
    // connection — a partial file is never resumed into a suspect prefix
    // (null blocks from a concurrently rewritten remote file would
    // otherwise be cemented by size-only resume forever).
    void download(const QString &remotePath, const QString &localPath, bool verify = true) override;
    // Recursively download a remote directory into localDir (created if
    // missing). Progress/finished signals use remotePath as the key, with
    // byte counters cumulative across all files in the tree.
    void downloadDir(const QString &remotePath, const QString &localDir);
    // Uploads localPath to remotePath. When verify is set (default) the
    // remote content is checksummed after the transfer (remote md5sum, or
    // an SFTP read-back when no shell is available) and a mismatch makes
    // the transfer FAIL loudly instead of leaving silent corruption.
    void upload(const QString &localPath, const QString &remotePath, bool verify = true) override;
    void cancelTransfer() override;

signals:
    void connected(const QString &homePath);
    void dirListed(const QString &path, const QList<hssh::SftpFileInfo> &entries);
    void dirTreeListed(const QString &path, const QList<hssh::RemoteFileEntry> &entries);
    // Periodic progress during a recursive tree walk (compare analysis).
    void dirTreeProgress(const QString &path, int entriesScanned);
    void canonicalized(const QString &path, const QString &canonicalPath);
    void operationFinished(const QString &operation, bool ok, const QString &message);
    // errorOccurred / transferProgress / transferStep / transferFinished are
    // inherited from TransferSession.

private:
    void doConnect();
    void doListDir(const QString &path);
    void doCanonicalize(const QString &path);
    void doMakeDir(const QString &path);
    void doRenameEntry(const QString &oldPath, const QString &newPath);
    void doRemoveFile(const QString &path);
    void doRemoveDir(const QString &path);
    void doDownload(const QString &remotePath, const QString &localPath, bool verify);
    // One full download attempt. allowResume permits continuing a partial
    // local file (first attempt only — after a verification failure the
    // prefix is untrusted). On success `note` carries the MD5 hex (and
    // whether it was verified). `retryable` marks failures that deserve a
    // fresh-connection full retry (content mismatch / remote file changed
    // mid-transfer).
    bool downloadAttempt(const QString &remotePath, const QString &localPath, bool verify,
                         bool allowResume, QString *error, QString *note, bool *retryable);
    void doDownloadDir(const QString &remotePath, const QString &localDir);
    void doUpload(const QString &localPath, const QString &remotePath, bool verify);
    // One full upload attempt. On success `note` carries the local MD5 hex
    // (and whether it was verified against the remote). On failure
    // `retryable` marks failures that deserve a fresh-connection retry
    // (short write / content mismatch: the channel may be wedged).
    bool uploadAttempt(const QString &localPath, const QString &remotePath, bool verify,
                       QString *error, QString *note, bool *retryable);
    void doListDirRecursive(const QString &path);

    // Post-upload integrity check. Returns true when the remote file's MD5
    // matches localMd5Hex. Sets *unavailable (with a reason) when neither
    // a remote md5sum nor an SFTP read-back could be performed.
    bool verifyUpload(const QString &remotePath, const QByteArray &localMd5Hex,
                      QString *unavailable);
    // Runs "md5sum <path>" over an exec channel on the SFTP connection;
    // returns the lowercase hex digest or an empty string on any failure.
    QByteArray remoteMd5(const QString &remotePath);
    // Re-reads the remote file over SFTP and returns its MD5 hex digest,
    // or an empty string on failure.
    QByteArray remoteMd5ReadBack(const QString &remotePath);

    // Recursive walk collecting entries under remoteDir. Files are always
    // collected; directories only when includeDirs is set (compare needs
    // them, downloads don't).
    bool collectRemoteFiles(const QString &remoteDir, const QString &relDir,
                            QList<RemoteFileEntry> &out, QString &error,
                            bool includeDirs = false);
    // Streams one remote file into an already-open local file; `done` is the
    // transfer-wide cumulative byte counter reported under `key`. When
    // startOffset > 0 the remote read is seeked there first (resume).
    bool streamDownload(const QString &remoteFile, QFile &local, const QString &key,
                        qint64 &done, qint64 total, QString &error,
                        qint64 startOffset = 0);

    void doCleanup();
    // Tears down and re-establishes the SSH+SFTP connection (worker thread
    // only). Used before an upload retry: a connection that produced a
    // desynced byte stream may be wedged and would fail the retry too.
    bool reconnectSftp();
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

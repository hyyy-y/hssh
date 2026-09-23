#include "SftpSession.h"

#include "SshConnect.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <vector>

#include <fcntl.h>

#include <libssh/libssh.h>
#include <libssh/sftp.h>

namespace hssh {

namespace {
constexpr int transferChunkSize = 32 * 1024;
constexpr qint64 progressReportInterval = 256 * 1024;

// Defined further down (anonymous namespaces in one TU share scope).
QByteArray localMd5Hex(const QString &localPath);
} // namespace

SftpSession::SftpSession(const SessionConfig &config, QObject *parent)
    : TransferSession(parent)
    , m_config(config)
{
    qRegisterMetaType<hssh::SftpFileInfo>("hssh::SftpFileInfo");
    qRegisterMetaType<QList<hssh::SftpFileInfo>>("QList<hssh::SftpFileInfo>");
    qRegisterMetaType<hssh::RemoteFileEntry>("hssh::RemoteFileEntry");
    qRegisterMetaType<QList<hssh::RemoteFileEntry>>("QList<hssh::RemoteFileEntry>");
    moveToThread(&m_thread);
}

SftpSession::~SftpSession()
{
    stop();
}

void SftpSession::start()
{
    m_thread.start();
    QMetaObject::invokeMethod(this, [this]() {
        doConnect();
    }, Qt::QueuedConnection);
}

void SftpSession::stop()
{
    if (!m_thread.isRunning()) {
        return;
    }
    m_cancelTransfer = true;
    m_thread.quit();
    // A running transfer checks the cancel flag every chunk, so this returns
    // promptly; blocking network calls are bounded by the connect timeout.
    if (!m_thread.wait(5000)) {
        m_thread.terminate();
        m_thread.wait();
    }
    // The thread is down: no concurrent access, safe to clean up here.
    if (m_sftp) {
        sftp_free(m_sftp);
        m_sftp = nullptr;
    }
    sshDisconnectAndFree(m_ssh);
    m_ssh = nullptr;
}

void SftpSession::listDir(const QString &path)
{
    QMetaObject::invokeMethod(this, [this, path]() {
        doListDir(path);
    }, Qt::QueuedConnection);
}

void SftpSession::listDirRecursive(const QString &path, int maxDepth)
{
    QMetaObject::invokeMethod(this, [this, path, maxDepth]() {
        doListDirRecursive(path, maxDepth);
    }, Qt::QueuedConnection);
}

void SftpSession::canonicalize(const QString &path)
{
    QMetaObject::invokeMethod(this, [this, path]() {
        doCanonicalize(path);
    }, Qt::QueuedConnection);
}

void SftpSession::makeDir(const QString &path)
{
    QMetaObject::invokeMethod(this, [this, path]() {
        doMakeDir(path);
    }, Qt::QueuedConnection);
}

void SftpSession::renameEntry(const QString &oldPath, const QString &newPath)
{
    QMetaObject::invokeMethod(this, [this, oldPath, newPath]() {
        doRenameEntry(oldPath, newPath);
    }, Qt::QueuedConnection);
}

void SftpSession::removeFile(const QString &path)
{
    QMetaObject::invokeMethod(this, [this, path]() {
        doRemoveFile(path);
    }, Qt::QueuedConnection);
}

void SftpSession::removeDir(const QString &path)
{
    QMetaObject::invokeMethod(this, [this, path]() {
        doRemoveDir(path);
    }, Qt::QueuedConnection);
}

void SftpSession::setPermissions(const QString &path, quint32 mode)
{
    QMetaObject::invokeMethod(this, [this, path, mode]() {
        doSetPermissions(path, mode);
    }, Qt::QueuedConnection);
}

void SftpSession::setPermissionsRecursive(const QString &path, quint32 mode)
{
    QMetaObject::invokeMethod(this, [this, path, mode]() {
        doSetPermissionsRecursive(path, mode);
    }, Qt::QueuedConnection);
}

void SftpSession::download(const QString &remotePath, const QString &localPath, bool verify)
{
    QMetaObject::invokeMethod(this, [this, remotePath, localPath, verify]() {
        doDownload(remotePath, localPath, verify);
    }, Qt::QueuedConnection);
}

void SftpSession::downloadDir(const QString &remotePath, const QString &localDir)
{
    QMetaObject::invokeMethod(this, [this, remotePath, localDir]() {
        doDownloadDir(remotePath, localDir);
    }, Qt::QueuedConnection);
}

void SftpSession::upload(const QString &localPath, const QString &remotePath, bool verify)
{
    QMetaObject::invokeMethod(this, [this, localPath, remotePath, verify]() {
        doUpload(localPath, remotePath, verify);
    }, Qt::QueuedConnection);
}

void SftpSession::cancelTransfer()
{
    m_cancelTransfer = true;
}

QString SftpSession::sftpError() const
{
    const int err = sftp_get_error(m_sftp);
    QString message = QString::fromUtf8(ssh_get_error(m_ssh));
    if (message.isEmpty()) {
        message = QStringLiteral("SFTP error %1").arg(err);
    }
    return message;
}

void SftpSession::setHostKeyVerifier(const KeyStore::HostKeyVerifier &verifier)
{
    m_hostKeyVerifier = verifier;
}

void SftpSession::doConnect()
{
    QString error;
    m_ssh = sshConnectAndAuthenticate(m_config, &error, 0, m_hostKeyVerifier);
    if (!m_ssh) {
        emit errorOccurred(error);
        return;
    }

    m_sftp = sftp_new(m_ssh);
    if (!m_sftp || sftp_init(m_sftp) != SSH_OK) {
        error = sftpError();
        emit errorOccurred(error);
        doCleanup();
        return;
    }

    // Resolve the user's home directory as the initial browsing path.
    char *home = sftp_canonicalize_path(m_sftp, ".");
    QString homePath = home ? QString::fromUtf8(home) : QStringLiteral("/");
    if (home) {
        ssh_string_free_char(home);
    }
    emit connected(homePath);
}

void SftpSession::doListDir(const QString &path)
{
    if (!m_sftp) {
        emit errorOccurred(tr("SFTP session is not connected"));
        return;
    }

    sftp_dir dir = sftp_opendir(m_sftp, path.toUtf8().constData());
    if (!dir) {
        emit errorOccurred(tr("Failed to open directory %1: %2").arg(path, sftpError()));
        return;
    }

    QList<SftpFileInfo> entries;
    while (sftp_attributes attr = sftp_readdir(m_sftp, dir)) {
        const QString name = QString::fromUtf8(attr->name);
        if (name != QStringLiteral(".") && name != QStringLiteral("..")) {
            SftpFileInfo info;
            info.name = name;
            info.size = static_cast<qint64>(attr->size);
            info.mtime = static_cast<qint64>(attr->mtime);
            info.permissions = attr->permissions;
            info.isDir = attr->type == SSH_FILEXFER_TYPE_DIRECTORY;
            // Owner/group arrive as strings when the server sends the
            // OWNERGROUP attribute (display only; chown stays sudo-gated).
            info.owner = attr->owner ? QString::fromUtf8(attr->owner) : QString();
            info.group = attr->group ? QString::fromUtf8(attr->group) : QString();
            entries.append(info);
        }
        sftp_attributes_free(attr);
    }

    if (!sftp_dir_eof(dir)) {
        emit errorOccurred(tr("Error while reading directory %1: %2").arg(path, sftpError()));
    }
    sftp_closedir(dir);

    // Directories first, then by name.
    std::sort(entries.begin(), entries.end(), [](const SftpFileInfo &a, const SftpFileInfo &b) {
        if (a.isDir != b.isDir) {
            return a.isDir;
        }
        return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });

    emit dirListed(path, entries);
}

void SftpSession::doCanonicalize(const QString &path)
{
    if (!m_sftp) {
        emit errorOccurred(tr("SFTP session is not connected"));
        return;
    }
    char *canonical = sftp_canonicalize_path(m_sftp, path.toUtf8().constData());
    if (!canonical) {
        emit errorOccurred(tr("Failed to resolve path %1: %2").arg(path, sftpError()));
        return;
    }
    const QString result = QString::fromUtf8(canonical);
    ssh_string_free_char(canonical);
    emit canonicalized(path, result);
}

void SftpSession::doMakeDir(const QString &path)
{
    if (!m_sftp) {
        emit errorOccurred(tr("SFTP session is not connected"));
        return;
    }
    const int rc = sftp_mkdir(m_sftp, path.toUtf8().constData(), 0755);
    emit operationFinished(QStringLiteral("mkdir"), rc == SSH_OK,
                           rc == SSH_OK ? QString() : sftpError());
}

void SftpSession::doRenameEntry(const QString &oldPath, const QString &newPath)
{
    if (!m_sftp) {
        emit errorOccurred(tr("SFTP session is not connected"));
        return;
    }
    const int rc = sftp_rename(m_sftp, oldPath.toUtf8().constData(), newPath.toUtf8().constData());
    emit operationFinished(QStringLiteral("rename"), rc == SSH_OK,
                           rc == SSH_OK ? QString() : sftpError());
}

void SftpSession::doRemoveFile(const QString &path)
{
    if (!m_sftp) {
        emit errorOccurred(tr("SFTP session is not connected"));
        return;
    }
    const int rc = sftp_unlink(m_sftp, path.toUtf8().constData());
    emit operationFinished(QStringLiteral("delete"), rc == SSH_OK,
                           rc == SSH_OK ? QString() : sftpError());
}

void SftpSession::doRemoveDir(const QString &path)
{
    if (!m_sftp) {
        emit errorOccurred(tr("SFTP session is not connected"));
        return;
    }
    const int rc = sftp_rmdir(m_sftp, path.toUtf8().constData());
    emit operationFinished(QStringLiteral("rmdir"), rc == SSH_OK,
                           rc == SSH_OK ? QString() : sftpError());
}

void SftpSession::doSetPermissions(const QString &path, quint32 mode)
{
    if (!m_sftp) {
        emit errorOccurred(tr("SFTP session is not connected"));
        return;
    }
    const int rc = sftp_chmod(m_sftp, path.toUtf8().constData(), mode);
    emit operationFinished(QStringLiteral("chmod"), rc == SSH_OK,
                           rc == SSH_OK ? QString() : sftpError());
}

void SftpSession::doSetPermissionsRecursive(const QString &path, quint32 mode)
{
    if (!m_sftp) {
        emit errorOccurred(tr("SFTP session is not connected"));
        return;
    }

    // Walk inside the worker (no separate listing round-trip through the
    // GUI), then chmod every entry; the walk already reports progress.
    QList<RemoteFileEntry> entries;
    QString error;
    m_treeWalkCount = 0;
    m_treeWalkPath = path;
    m_cancelTransfer = false;
    if (!collectRemoteFiles(path, QString(), entries, error, true)) {
        emit errorOccurred(tr("Recursive chmod failed while listing %1: %2").arg(path, error));
        return;
    }

    int failed = 0;
    QString firstError;
    for (const RemoteFileEntry &entry : entries) {
        if (m_cancelTransfer) {
            emit operationFinished(QStringLiteral("chmod"), false, tr("Cancelled"));
            return;
        }
        if (sftp_chmod(m_sftp, entry.remotePath.toUtf8().constData(), mode) != SSH_OK) {
            ++failed;
            if (firstError.isEmpty()) {
                firstError = tr("%1: %2").arg(entry.remotePath, sftpError());
            }
        }
    }
    // The root itself is not part of the walk results.
    if (sftp_chmod(m_sftp, path.toUtf8().constData(), mode) != SSH_OK) {
        ++failed;
        if (firstError.isEmpty()) {
            firstError = sftpError();
        }
    }

    if (failed == 0) {
        emit operationFinished(QStringLiteral("chmod"), true,
                               tr("%1 entries updated").arg(entries.size() + 1));
    } else {
        emit operationFinished(QStringLiteral("chmod"), false,
                               tr("%1 of %2 entries failed (first: %3)")
                                   .arg(failed)
                                   .arg(entries.size() + 1)
                                   .arg(firstError));
    }
}

bool SftpSession::streamDownload(const QString &remoteFile, QFile &local,
                                 const QString &key, qint64 &done, qint64 total,
                                 QString &error, qint64 startOffset)
{
    sftp_file remote = sftp_open(m_sftp, remoteFile.toUtf8().constData(), O_RDONLY, 0);
    if (!remote) {
        error = sftpError();
        return false;
    }

    if (startOffset > 0) {
        const int seekRc = sftp_seek64(remote, static_cast<uint64_t>(startOffset));
        if (seekRc != SSH_OK) {
            error = tr("Failed to seek remote file: %1").arg(sftpError());
            sftp_close(remote);
            return false;
        }
    }

    std::vector<char> buffer(transferChunkSize);
    qint64 lastReport = done;
    bool ok = true;
    while (!m_cancelTransfer) {
        const ssize_t n = sftp_read(remote, buffer.data(), buffer.size());
        if (n == 0) {
            break; // EOF
        }
        if (n < 0) {
            ok = false;
            error = sftpError();
            break;
        }
        if (local.write(buffer.data(), n) != n) {
            ok = false;
            error = tr("Failed to write to %1").arg(local.fileName());
            break;
        }
        done += n;
        if (done - lastReport >= progressReportInterval) {
            lastReport = done;
            emit transferProgress(key, done, total);
        }
    }

    sftp_close(remote);
    return ok && !m_cancelTransfer;
}

void SftpSession::doDownload(const QString &remotePath, const QString &localPath, bool verify)
{
    // A content mismatch or a mid-transfer remote change means the local
    // prefix cannot be trusted (null blocks from a concurrently
    // ftruncate+rewritten remote file read back as real zeros — libssh
    // faithfully delivers what the server sends). Retrying with resume
    // would cement the corrupt prefix (size-only resume was exactly why
    // the 2026-08 "null block" corruption persisted), so the retry deletes
    // the partial file and re-downloads from scratch over a fresh
    // connection.
    for (int attempt = 1;; ++attempt) {
        QString error;
        QString note;
        bool retryable = false;
        const bool ok = downloadAttempt(remotePath, localPath, verify, attempt == 1,
                                        &error, &note, &retryable);
        if (ok) {
            emit transferFinished(remotePath, true, note);
            return;
        }
        if (retryable && attempt == 1 && !m_cancelTransfer && reconnectSftp()) {
            QFile::remove(localPath + QStringLiteral(".part")); // suspect prefix: never resume into it
            continue;
        }
        emit transferFinished(remotePath, false, error);
        return;
    }
}

bool SftpSession::downloadAttempt(const QString &remotePath, const QString &localPath,
                                  bool verify, bool allowResume,
                                  QString *outError, QString *outNote, bool *outRetryable)
{
    const auto fail = [outError](const QString &message) {
        if (outError) {
            *outError = message;
        }
        return false;
    };
    if (outRetryable) {
        *outRetryable = false;
    }

    if (!m_sftp) {
        return fail(tr("SFTP session is not connected"));
    }

    sftp_attributes attr = sftp_stat(m_sftp, remotePath.toUtf8().constData());
    if (!attr) {
        return fail(sftpError());
    }
    const qint64 total = static_cast<qint64>(attr->size);
    const qint64 mtimeBefore = static_cast<qint64>(attr->mtime);
    sftp_attributes_free(attr);
    // Report the size immediately: the agent's status endpoint shows
    // bytesTotal=0 until the first progress chunk otherwise (2026-09-20).
    emit transferProgress(remotePath, 0, total);

    // Atomic landing: bytes stream into "<local>.part" and are renamed to
    // the final name only after success (+ verification). A caller-side
    // timeout or crash therefore never leaves a half-written file under the
    // real name (2026-09-18: a 232 MB download outlived its MCP tool call;
    // the completed-looking partial would have been mistaken for the file).
    const QString partPath = localPath + QStringLiteral(".part");
    QFile local(partPath);
    // Nested targets (folder-compare sync) may need their parents created.
    QDir().mkpath(QFileInfo(localPath).absolutePath());

    // Resume: when the local .part is smaller than the remote file, continue
    // from its size instead of starting over (PH1-01). An intact final file
    // of equal size is verified (when verify is on) before being declared
    // up to date: size alone cannot rule out in-place corruption.
    qint64 startOffset = 0;
    if (QFile::exists(localPath)) {
        const qint64 localSize = QFileInfo(localPath).size();
        if (localSize == total) {
            if (!verify) {
                emit transferProgress(remotePath, total, total);
                if (outNote) {
                    *outNote = tr("Already up to date");
                }
                return true;
            }
            QByteArray remoteDigest = remoteMd5(remotePath);
            if (remoteDigest.isEmpty()) {
                remoteDigest = remoteMd5ReadBack(remotePath);
            }
            const QByteArray localDigest = localMd5Hex(localPath);
            if (remoteDigest.isEmpty()) {
                // No way to checksum the remote at all: keep the historic
                // size-only shortcut rather than re-downloading every time.
                emit transferProgress(remotePath, total, total);
                if (outNote) {
                    *outNote = tr("Already up to date (size match; content verification unavailable)");
                }
                return true;
            }
            if (remoteDigest == localDigest) {
                QFile::remove(partPath); // stale leftover from an old attempt
                emit transferProgress(remotePath, total, total);
                if (outNote) {
                    *outNote = tr("Already up to date (md5 verified: %1)")
                                   .arg(QString::fromLatin1(localDigest));
                }
                return true;
            }
            // Content differs: fall through to a full re-download. Size
            // says nothing about byte content.
        }
    }
    if (local.exists()) {
        const qint64 partSize = local.size();
        if (partSize < total && allowResume) {
            startOffset = partSize;
        }
    }

    if (!local.open(startOffset > 0 ? QIODevice::WriteOnly : QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(tr("Cannot write to %1").arg(partPath));
    }
    if (startOffset > 0) {
        local.seek(startOffset);
    }

    m_cancelTransfer = false;
    emit transferStep(remotePath, 1, 1, remotePath.section(QLatin1Char('/'), -1));
    QString error;
    qint64 done = startOffset;
    const bool ok = streamDownload(remotePath, local, remotePath, done, total, error, startOffset);
    local.close();

    if (m_cancelTransfer.load()) {
        return fail(tr("Cancelled"));
    }
    if (!ok) {
        return fail(tr("%1 (%2 of %3 bytes transferred; retry resumes from the partial file)")
                        .arg(error).arg(done).arg(total));
    }

    // Concurrent-modification detection: a file being regenerated while we
    // read it is the classic source of zero blocks (sparse gap reads).
    sftp_attributes after = sftp_stat(m_sftp, remotePath.toUtf8().constData());
    if (after) {
        const qint64 sizeAfter = static_cast<qint64>(after->size);
        const qint64 mtimeAfter = static_cast<qint64>(after->mtime);
        sftp_attributes_free(after);
        if (sizeAfter != total || mtimeAfter != mtimeBefore) {
            if (outRetryable) {
                *outRetryable = true;
            }
            return fail(tr("Remote file changed during download (size %1 -> %2); "
                           "it may be rewritten concurrently — retry")
                            .arg(total).arg(sizeAfter));
        }
    }
    if (done != total) {
        if (outRetryable) {
            *outRetryable = true;
        }
        return fail(tr("Short download: %1 of %2 bytes transferred").arg(done).arg(total));
    }

    // Content verification, mirroring the upload side: remote md5sum via an
    // exec channel, SFTP read-back when no shell is available. The digest
    // covers the .part file — it becomes the final file on success.
    if (verify) {
        const QByteArray localDigest = localMd5Hex(partPath);
        QByteArray remoteDigest = remoteMd5(remotePath);
        if (remoteDigest.isEmpty()) {
            remoteDigest = remoteMd5ReadBack(remotePath);
        }
        if (remoteDigest.isEmpty()) {
            if (outNote) {
                *outNote = tr("downloaded, md5 %1; content verification unavailable "
                              "(no remote md5sum)")
                               .arg(QString::fromLatin1(localDigest));
            }
        } else if (remoteDigest != localDigest) {
            if (outRetryable) {
                *outRetryable = true;
            }
            return fail(tr("Download verification failed: the local file content differs "
                           "from the remote (remote md5 %1, local md5 %2) — the remote "
                           "file may be rewritten concurrently")
                            .arg(QString::fromLatin1(remoteDigest),
                                 QString::fromLatin1(localDigest)));
        } else if (outNote) {
            *outNote = tr("downloaded, md5 verified: %1").arg(QString::fromLatin1(localDigest));
        }
    } else if (outNote) {
        *outNote = tr("downloaded (verification off)");
    }
    // Atomic landing: only now does the file appear under its real name.
    QFile::remove(localPath);
    if (!QFile::rename(partPath, localPath)) {
        return fail(tr("Downloaded to %1 but failed to finalize %2").arg(partPath, localPath));
    }
    emit transferProgress(remotePath, done, total);
    return true;
}

bool SftpSession::collectRemoteFiles(const QString &remoteDir, const QString &relDir,
                                     QList<RemoteFileEntry> &out, QString &error,
                                     bool includeDirs, int depth, int maxDepth)
{
    sftp_dir dir = sftp_opendir(m_sftp, remoteDir.toUtf8().constData());
    if (!dir) {
        error = tr("Failed to open directory %1: %2").arg(remoteDir, sftpError());
        return false;
    }

    bool ok = true;
    while (ok) {
        sftp_attributes attr = sftp_readdir(m_sftp, dir);
        if (!attr) {
            break;
        }
        const QString name = QString::fromUtf8(attr->name);
        const bool isDir = attr->type == SSH_FILEXFER_TYPE_DIRECTORY;
        const qint64 size = static_cast<qint64>(attr->size);
        const qint64 mtime = static_cast<qint64>(attr->mtime);
        sftp_attributes_free(attr);

        if (name == QStringLiteral(".") || name == QStringLiteral("..")) {
            continue;
        }
        const QString remoteChild = remoteDir.endsWith(QLatin1Char('/'))
                                        ? remoteDir + name
                                        : remoteDir + QLatin1Char('/') + name;
        const QString relChild = relDir.isEmpty() ? name : relDir + QLatin1Char('/') + name;
        if (isDir) {
            if (includeDirs) {
                out.append({remoteChild, relChild, 0, mtime, true});
            }
            // maxDepth 0 = unlimited; otherwise stop descending at the limit
            // (saves the network round-trips the filters would discard).
            if (maxDepth <= 0 || depth < maxDepth) {
                ok = collectRemoteFiles(remoteChild, relChild, out, error,
                                        includeDirs, depth + 1, maxDepth);
            }
        } else {
            out.append({remoteChild, relChild, size, mtime, false});
        }
        ++m_treeWalkCount;
        if ((m_treeWalkCount & 0x3F) == 0) {
            emit dirTreeProgress(m_treeWalkPath, m_treeWalkCount);
        }
        if (m_cancelTransfer) {
            ok = false;
        }
    }

    if (ok && !sftp_dir_eof(dir)) {
        error = tr("Error while reading directory %1: %2").arg(remoteDir, sftpError());
        ok = false;
    }
    sftp_closedir(dir);
    return ok;
}

void SftpSession::doListDirRecursive(const QString &path, int maxDepth)
{
    if (!m_sftp) {
        emit errorOccurred(tr("SFTP session is not connected"));
        return;
    }
    QList<RemoteFileEntry> entries;
    QString error;
    m_treeWalkCount = 0;
    m_treeWalkPath = path;
    // A cancel flag left over from a previous transfer would abort this walk
    // on its first entry — every walk starts cancellable, not pre-cancelled.
    m_cancelTransfer = false;
    if (!collectRemoteFiles(path, QString(), entries, error, true, 0, maxDepth)) {
        emit errorOccurred(error);
        return;
    }
    emit dirTreeListed(path, entries);
}

void SftpSession::doDownloadDir(const QString &remotePath, const QString &localDir)
{
    if (!m_sftp) {
        emit transferFinished(remotePath, false, tr("SFTP session is not connected"));
        return;
    }

    // Walk first so the progress dialog can show a real total from the start.
    m_cancelTransfer = false;
    QList<RemoteFileEntry> files;
    QString error;
    if (!collectRemoteFiles(remotePath, QString(), files, error)) {
        emit transferFinished(remotePath, false,
                              m_cancelTransfer ? tr("Cancelled") : error);
        return;
    }

    qint64 total = 0;
    for (const RemoteFileEntry &f : files) {
        total += f.size;
    }

    const QDir base(localDir);
    base.mkpath(QStringLiteral("."));

    bool ok = true;
    qint64 done = 0;
    int index = 0;
    for (const RemoteFileEntry &f : files) {
        if (m_cancelTransfer) {
            break;
        }
        ++index;
        emit transferStep(remotePath, index, static_cast<int>(files.size()), f.relPath);

        const QString localPath = base.filePath(f.relPath);
        QDir().mkpath(QFileInfo(localPath).absolutePath());
        // Atomic landing: stream into "<file>.part", rename on completion.
        const QString partPath = localPath + QStringLiteral(".part");
        QFile local(partPath);

        // Resume support: continue from the existing partial file.
        qint64 fileStart = 0;
        if (local.exists() && local.size() < f.size) {
            fileStart = local.size();
        }
        if (!local.open(fileStart > 0 ? QIODevice::WriteOnly
                                      : QIODevice::WriteOnly | QIODevice::Truncate)) {
            error = tr("Cannot write to %1").arg(partPath);
            ok = false;
            break;
        }
        if (fileStart > 0) {
            local.seek(fileStart);
        }
        const bool fileOk = streamDownload(f.remotePath, local, remotePath, done, total, error, fileStart);
        local.close();
        if (fileOk) {
            QFile::remove(localPath);
            if (!QFile::rename(partPath, localPath)) {
                error = tr("Failed to finalize %1").arg(localPath);
                ok = false;
                break;
            }
        }
        if (!fileOk && !m_cancelTransfer) {
            ok = false;
            break;
        }
    }

    if (m_cancelTransfer) {
        emit transferFinished(remotePath, false, tr("Cancelled"));
    } else {
        emit transferProgress(remotePath, done, total);
        emit transferFinished(remotePath, ok, error);
    }
}

void SftpSession::doUpload(const QString &localPath, const QString &remotePath, bool verify)
{
    // Content-mismatch or a short write means the SFTP byte stream desynced
    // on this connection; that connection may be wedged, and a retry over it
    // kept failing (observed 2026-08-19). Rebuild the connection and
    // re-upload from scratch once before giving up.
    for (int attempt = 1;; ++attempt) {
        QString error;
        QString note;
        bool retryable = false;
        const bool ok = uploadAttempt(localPath, remotePath, verify,
                                      &error, &note, &retryable);
        if (ok) {
            emit transferFinished(localPath, true, note);
            return;
        }
        if (retryable && attempt == 1 && !m_cancelTransfer && reconnectSftp()) {
            // Never resume after a desync: the remote prefix may be
            // scrambled. Remove the file so the retry truly starts fresh.
            sftp_unlink(m_sftp, remotePath.toUtf8().constData());
            continue; // fresh connection; re-upload from scratch
        }
        emit transferFinished(localPath, false, error);
        return;
    }
}

bool SftpSession::uploadAttempt(const QString &localPath, const QString &remotePath, bool verify,
                                QString *outError, QString *outNote, bool *outRetryable)
{
    const auto fail = [outError](const QString &message) {
        if (outError) {
            *outError = message;
        }
        return false;
    };
    if (outRetryable) {
        *outRetryable = false;
    }

    if (!m_sftp) {
        return fail(tr("SFTP session is not connected"));
    }

    QFile local(localPath);
    if (!local.open(QIODevice::ReadOnly)) {
        return fail(tr("Cannot read %1").arg(localPath));
    }
    const qint64 total = local.size();

    // Resume: when the remote file is smaller than the local one, continue
    // from the remote size; equal/larger remote means overwrite from scratch.
    qint64 startOffset = 0;
    sftp_attributes remoteAttr = sftp_stat(m_sftp, remotePath.toUtf8().constData());
    if (remoteAttr) {
        const qint64 remoteSize = static_cast<qint64>(remoteAttr->size);
        sftp_attributes_free(remoteAttr);
        if (remoteSize > 0 && remoteSize < total) {
            startOffset = remoteSize;
        }
    }

    const int openFlags = startOffset > 0
                              ? O_WRONLY | O_CREAT            // keep existing bytes
                              : O_WRONLY | O_CREAT | O_TRUNC;
    sftp_file remote = sftp_open(m_sftp, remotePath.toUtf8().constData(), openFlags, 0644);
    if (!remote) {
        return fail(sftpError());
    }

    // The digest must cover the whole file, so on resume first hash the
    // prefix that is already on the server, then continue from startOffset.
    QCryptographicHash md5(QCryptographicHash::Md5);
    if (startOffset > 0) {
        if (!local.seek(0)) {
            sftp_close(remote);
            return fail(tr("Failed to read %1").arg(localPath));
        }
        qint64 remaining = startOffset;
        while (remaining > 0) {
            const QByteArray part = local.read(qMin(remaining, static_cast<qint64>(transferChunkSize)));
            if (part.isEmpty()) {
                sftp_close(remote);
                return fail(tr("Failed to read %1: %2").arg(localPath, local.errorString()));
            }
            md5.addData(part);
            remaining -= part.size();
        }
    }

    if (startOffset > 0) {
        if (sftp_seek64(remote, static_cast<uint64_t>(startOffset)) != SSH_OK) {
            sftp_close(remote);
            return fail(tr("Failed to seek remote file: %1").arg(sftpError()));
        }
        local.seek(startOffset);
    }

    m_cancelTransfer = false;
    bool ok = true;
    QString error;
    qint64 done = startOffset;
    qint64 lastReport = 0;

    while (!m_cancelTransfer) {
        const QByteArray chunk = local.read(transferChunkSize);
        if (chunk.isEmpty()) {
            if (local.error() != QFileDevice::NoError) {
                ok = false;
                error = tr("Failed to read %1: %2").arg(localPath, local.errorString());
            }
            break; // EOF
        }
        const ssize_t n = sftp_write(remote, chunk.constData(), static_cast<size_t>(chunk.size()));
        if (n < 0 || n != chunk.size()) {
            // libssh can return a SHORT count without an error when an
            // internal blocking wait times out (a congested channel window).
            // sftp_write only logs that case; the SFTP byte stream is then
            // desynced and continuing would silently scramble the remote
            // file, so fail loudly instead.
            ok = false;
            error = n < 0 ? sftpError()
                          : tr("Short write on SFTP channel (%1 of %2 bytes written); "
                               "aborting to avoid silent corruption").arg(n).arg(chunk.size());
            if (outRetryable) {
                // A short write desyncs the byte stream and indicates a
                // possibly wedged channel: retry over a fresh connection.
                *outRetryable = true;
            }
            break;
        }
        md5.addData(chunk);
        done += n;
        if (done - lastReport >= progressReportInterval) {
            lastReport = done;
            emit transferProgress(localPath, done, total);
        }
    }

    const bool cancelled = m_cancelTransfer.load();
    sftp_close(remote);
    local.close();

    const QByteArray md5Hex = md5.result().toHex();

    // Content verification: a transfer that reports success must be
    // byte-identical on the server. A mismatch fails as retryable so
    // doUpload retries once over a fresh connection.
    if (!cancelled && ok && verify) {
        QString unavailable;
        if (!verifyUpload(remotePath, md5Hex, &unavailable)) {
            if (unavailable.isEmpty()) {
                if (outRetryable) {
                    *outRetryable = true;
                }
                return fail(tr("Upload verification failed: the remote file content differs "
                               "from the local file (local md5 %1)").arg(QString::fromLatin1(md5Hex)));
            }
            if (outNote) {
                *outNote = tr("uploaded, md5 %1; content verification unavailable: %2")
                               .arg(QString::fromLatin1(md5Hex), unavailable);
            }
            emit transferProgress(localPath, done, total);
            return true;
        }
    }

    // Keep the partial remote file on cancel/error so a later attempt resumes.
    if (cancelled) {
        return fail(tr("Cancelled"));
    }
    if (!ok) {
        return fail(tr("%1 (%2 of %3 bytes transferred; retry resumes from the partial file)")
                        .arg(error).arg(done).arg(total));
    }
    if (outNote) {
        *outNote = verify
            ? tr("uploaded, md5 verified: %1").arg(QString::fromLatin1(md5Hex))
            : tr("uploaded, md5 %1 (verification off)").arg(QString::fromLatin1(md5Hex));
    }
    emit transferProgress(localPath, done, total);
    return true;
}

namespace {

// POSIX single-quote escaping for passing a path through a remote shell.
QByteArray shellQuote(const QString &path)
{
    QByteArray quoted = path.toUtf8();
    quoted.replace('\'', "'\\''");
    return "'" + quoted + "'";
}

// MD5 hex digest of a local file, or empty on read failure.
QByteArray localMd5Hex(const QString &localPath)
{
    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash md5(QCryptographicHash::Md5);
    char buffer[transferChunkSize];
    qint64 n = 0;
    while ((n = file.read(buffer, sizeof(buffer))) > 0) {
        md5.addData(QByteArrayView(buffer, static_cast<qsizetype>(n)));
    }
    if (n < 0) {
        return {};
    }
    return md5.result().toHex();
}

} // namespace

QByteArray SftpSession::remoteMd5(const QString &remotePath)
{
    if (!m_ssh || !ssh_is_connected(m_ssh)) {
        return {};
    }
    ssh_channel channel = ssh_channel_new(m_ssh);
    if (!channel) {
        return {};
    }

    QByteArray digest;
    for (;;) {
        if (ssh_channel_open_session(channel) != SSH_OK) {
            break;
        }
        const QByteArray command = "md5sum -- " + shellQuote(remotePath);
        if (ssh_channel_request_exec(channel, command.constData()) != SSH_OK) {
            break;
        }
        QByteArray output;
        char buffer[512];
        int n = 0;
        while ((n = ssh_channel_read(channel, buffer, sizeof(buffer), 0)) > 0) {
            output.append(buffer, n);
        }
        // Drain stderr so no packets linger on the channel.
        while (ssh_channel_read(channel, buffer, sizeof(buffer), 1) > 0) {
        }
        ssh_channel_send_eof(channel);
        if (ssh_channel_get_exit_status(channel) != 0) {
            break; // md5sum missing or unreadable file
        }
        const QByteArray token = output.trimmed().split(' ').first().toLower();
        bool hex = token.size() == 32;
        for (const char c : token) {
            hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
        }
        if (hex) {
            digest = token;
        }
        break;
    }

    ssh_channel_close(channel);
    ssh_channel_free(channel);
    return digest;
}

QByteArray SftpSession::remoteMd5ReadBack(const QString &remotePath)
{
    sftp_file remote = sftp_open(m_sftp, remotePath.toUtf8().constData(), O_RDONLY, 0);
    if (!remote) {
        return {};
    }
    QCryptographicHash md5(QCryptographicHash::Md5);
    std::vector<char> buffer(transferChunkSize);
    bool ok = true;
    for (;;) {
        const ssize_t n = sftp_read(remote, buffer.data(), buffer.size());
        if (n == 0) {
            break; // EOF
        }
        if (n < 0) {
            ok = false;
            break;
        }
        md5.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(n)));
    }
    sftp_close(remote);
    return ok ? md5.result().toHex() : QByteArray();
}

bool SftpSession::verifyUpload(const QString &remotePath, const QByteArray &localMd5Hex,
                               QString *unavailable)
{
    const QByteArray digest = remoteMd5(remotePath);
    if (!digest.isEmpty()) {
        return digest == localMd5Hex;
    }
    // No shell (or no md5sum) on the server: re-read the file over SFTP and
    // compare digests instead. Costs one download of the file, but a silent
    // corruption is never acceptable.
    const QByteArray readBack = remoteMd5ReadBack(remotePath);
    if (!readBack.isEmpty()) {
        return readBack == localMd5Hex;
    }
    if (unavailable) {
        *unavailable = tr("neither remote md5sum nor SFTP read-back succeeded");
    }
    return false;
}

void SftpSession::doCleanup()
{
    if (m_sftp) {
        sftp_free(m_sftp);
        m_sftp = nullptr;
    }
    sshDisconnectAndFree(m_ssh);
    m_ssh = nullptr;
}

bool SftpSession::reconnectSftp()
{
    // Worker-thread only (called from doUpload between attempts).
    doCleanup();
    // SFTP needs unbounded post-connect blocking calls (default 0): a
    // bounded channel write is a silent short write -> corruption.
    QString error;
    m_ssh = sshConnectAndAuthenticate(m_config, &error, 0, m_hostKeyVerifier);
    if (!m_ssh) {
        return false;
    }
    m_sftp = sftp_new(m_ssh);
    if (!m_sftp || sftp_init(m_sftp) != SSH_OK) {
        doCleanup();
        return false;
    }
    return true;
}

} // namespace hssh

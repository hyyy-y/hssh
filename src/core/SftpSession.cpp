#include "SftpSession.h"

#include "SshConnect.h"

#include <QDir>
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
} // namespace

SftpSession::SftpSession(const SessionConfig &config, QObject *parent)
    : QObject(parent)
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

void SftpSession::listDirRecursive(const QString &path)
{
    QMetaObject::invokeMethod(this, [this, path]() {
        doListDirRecursive(path);
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

void SftpSession::download(const QString &remotePath, const QString &localPath)
{
    QMetaObject::invokeMethod(this, [this, remotePath, localPath]() {
        doDownload(remotePath, localPath);
    }, Qt::QueuedConnection);
}

void SftpSession::downloadDir(const QString &remotePath, const QString &localDir)
{
    QMetaObject::invokeMethod(this, [this, remotePath, localDir]() {
        doDownloadDir(remotePath, localDir);
    }, Qt::QueuedConnection);
}

void SftpSession::upload(const QString &localPath, const QString &remotePath)
{
    QMetaObject::invokeMethod(this, [this, localPath, remotePath]() {
        doUpload(localPath, remotePath);
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

void SftpSession::doConnect()
{
    QString error;
    m_ssh = sshConnectAndAuthenticate(m_config, &error);
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

bool SftpSession::streamDownload(const QString &remoteFile, QFile &local,
                                 const QString &key, qint64 &done, qint64 total,
                                 QString &error)
{
    sftp_file remote = sftp_open(m_sftp, remoteFile.toUtf8().constData(), O_RDONLY, 0);
    if (!remote) {
        error = sftpError();
        return false;
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

void SftpSession::doDownload(const QString &remotePath, const QString &localPath)
{
    if (!m_sftp) {
        emit transferFinished(remotePath, false, tr("SFTP session is not connected"));
        return;
    }

    sftp_attributes attr = sftp_stat(m_sftp, remotePath.toUtf8().constData());
    if (!attr) {
        emit transferFinished(remotePath, false, sftpError());
        return;
    }
    const qint64 total = static_cast<qint64>(attr->size);
    sftp_attributes_free(attr);

    QFile local(localPath);
    // Nested targets (folder-compare sync) may need their parents created.
    QDir().mkpath(QFileInfo(localPath).absolutePath());
    if (!local.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit transferFinished(remotePath, false, tr("Cannot write to %1").arg(localPath));
        return;
    }

    m_cancelTransfer = false;
    emit transferStep(remotePath, 1, 1, remotePath.section(QLatin1Char('/'), -1));
    QString error;
    qint64 done = 0;
    const bool ok = streamDownload(remotePath, local, remotePath, done, total, error);
    local.close();

    const bool cancelled = m_cancelTransfer.load();
    if (cancelled || !ok) {
        local.remove();
    }
    if (cancelled) {
        emit transferFinished(remotePath, false, tr("Cancelled"));
    } else {
        emit transferProgress(remotePath, done, total);
        emit transferFinished(remotePath, ok, error);
    }
}

bool SftpSession::collectRemoteFiles(const QString &remoteDir, const QString &relDir,
                                     QList<RemoteFileEntry> &out, QString &error,
                                     bool includeDirs)
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
            ok = collectRemoteFiles(remoteChild, relChild, out, error, includeDirs);
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

void SftpSession::doListDirRecursive(const QString &path)
{
    if (!m_sftp) {
        emit errorOccurred(tr("SFTP session is not connected"));
        return;
    }
    QList<RemoteFileEntry> entries;
    QString error;
    m_treeWalkCount = 0;
    m_treeWalkPath = path;
    if (!collectRemoteFiles(path, QString(), entries, error, true)) {
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
        QFile local(localPath);
        if (!local.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            error = tr("Cannot write to %1").arg(localPath);
            ok = false;
            break;
        }
        const bool fileOk = streamDownload(f.remotePath, local, remotePath, done, total, error);
        local.close();
        if (!fileOk) {
            local.remove(); // drop the partial file
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

void SftpSession::doUpload(const QString &localPath, const QString &remotePath)
{
    if (!m_sftp) {
        emit transferFinished(localPath, false, tr("SFTP session is not connected"));
        return;
    }

    QFile local(localPath);
    if (!local.open(QIODevice::ReadOnly)) {
        emit transferFinished(localPath, false, tr("Cannot read %1").arg(localPath));
        return;
    }
    const qint64 total = local.size();

    sftp_file remote = sftp_open(m_sftp, remotePath.toUtf8().constData(),
                                 O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!remote) {
        emit transferFinished(localPath, false, sftpError());
        return;
    }

    m_cancelTransfer = false;
    bool ok = true;
    QString error;
    qint64 done = 0;
    qint64 lastReport = 0;

    while (!m_cancelTransfer) {
        const QByteArray chunk = local.read(transferChunkSize);
        if (chunk.isEmpty()) {
            break; // EOF
        }
        const ssize_t n = sftp_write(remote, chunk.constData(), static_cast<size_t>(chunk.size()));
        if (n < 0) {
            ok = false;
            error = sftpError();
            break;
        }
        done += n;
        if (done - lastReport >= progressReportInterval) {
            lastReport = done;
            emit transferProgress(localPath, done, total);
        }
    }

    const bool cancelled = m_cancelTransfer.load();
    sftp_close(remote);
    local.close();

    if (cancelled || !ok) {
        // Remove the partial remote file.
        sftp_unlink(m_sftp, remotePath.toUtf8().constData());
    }
    if (cancelled) {
        emit transferFinished(localPath, false, tr("Cancelled"));
    } else {
        emit transferProgress(localPath, done, total);
        emit transferFinished(localPath, ok, error);
    }
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

} // namespace hssh

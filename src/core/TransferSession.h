#ifndef HSSH_CORE_TRANSFERSESSION_H
#define HSSH_CORE_TRANSFERSESSION_H

#include <QObject>
#include <QString>

namespace hssh {

// Common interface for single-file transfer workers (SFTP, scp, shell/base64).
// Implementations live on their own thread with their own SSH connection;
// request methods are queued, results arrive via signals:
//
//   transferProgress(path, bytesDone, bytesTotal)
//   transferFinished(path, ok, message)   — message carries the md5 note on success
//   errorOccurred(message)                — connection/setup failure
class TransferSession : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void upload(const QString &localPath, const QString &remotePath, bool verify) = 0;
    virtual void download(const QString &remotePath, const QString &localPath, bool verify) = 0;
    virtual void cancelTransfer() = 0;

signals:
    void errorOccurred(const QString &message);
    void transferProgress(const QString &path, qint64 bytesDone, qint64 bytesTotal);
    void transferStep(const QString &path, int fileIndex, int fileCount, const QString &currentFile);
    void transferFinished(const QString &path, bool ok, const QString &message);
};

} // namespace hssh

#endif // HSSH_CORE_TRANSFERSESSION_H

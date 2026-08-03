#ifndef HSSH_APP_TRANSFERREGISTRY_H
#define HSSH_APP_TRANSFERREGISTRY_H

#include <QObject>
#include <QString>

namespace hssh {

// App-level singleton collecting file transfers from every SFTP/compare
// widget so the bottom "Transfers" dock can show a unified queue.
// All methods must be called on the GUI thread.
class TransferRegistry : public QObject {
    Q_OBJECT

public:
    enum class Direction {
        Upload,
        Download,
    };

    static TransferRegistry &instance();

    // Returns a transfer id used for the follow-up calls.
    int beginTransfer(const QString &name, Direction direction, const QString &sessionName);
    void updateProgress(int id, qint64 bytesDone, qint64 bytesTotal);
    void finishTransfer(int id, bool ok, const QString &message);

signals:
    void transferAdded(int id);
    void transferUpdated(int id);
    void transferRemoved(int id);

private:
    friend class TransfersWidget;

    struct Transfer {
        int id = 0;
        QString name;
        Direction direction = Direction::Upload;
        QString sessionName;
        qint64 bytesDone = 0;
        qint64 bytesTotal = 0;
        bool finished = false;
        bool ok = false;
        QString message;
    };

    TransferRegistry() = default;

    [[nodiscard]] const Transfer *transfer(int id) const;

    QList<Transfer> m_transfers;
    int m_nextId = 1;
};

} // namespace hssh

#endif // HSSH_APP_TRANSFERREGISTRY_H

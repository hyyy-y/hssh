#ifndef HSSH_CORE_CONNECTIONMANAGER_H
#define HSSH_CORE_CONNECTIONMANAGER_H

#include "core/SessionConfig.h"

#include <QMap>
#include <QObject>
#include <QString>
#include <memory>

namespace hssh {

class SshSession;

class ConnectionManager : public QObject {
    Q_OBJECT

public:
    static ConnectionManager &instance();

    [[nodiscard]] QString createSession(const SessionConfig &config);
    void closeSession(const QString &sessionId);
    void closeAll();

    [[nodiscard]] SshSession *session(const QString &sessionId) const;
    [[nodiscard]] QList<QString> sessionIds() const;
    [[nodiscard]] SessionConfig config(const QString &sessionId) const;

signals:
    void sessionConnected(const QString &sessionId);
    void sessionDisconnected(const QString &sessionId);
    void sessionError(const QString &sessionId, const QString &message);

private:
    explicit ConnectionManager(QObject *parent = nullptr);
    ~ConnectionManager() override;

    ConnectionManager(const ConnectionManager &) = delete;
    ConnectionManager &operator=(const ConnectionManager &) = delete;

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace hssh

#endif // HSSH_CORE_CONNECTIONMANAGER_H

#ifndef HSSH_CORE_PORTFORWARD_H
#define HSSH_CORE_PORTFORWARD_H

#include <QList>
#include <QObject>
#include <QString>
#include <memory>

namespace hssh {

class SshSession;

// One port-forwarding rule. Local: listen on bindPort and forward to
// targetHost:targetPort over the SSH connection. Remote: have the server
// listen on bindPort and forward to targetHost:targetPort on this machine.
// Dynamic: SOCKS5 proxy listening on bindPort; targets come from each client.
struct ForwardSpec {
    enum class Type {
        Local,
        Remote,
        Dynamic
    };

    Type type = Type::Local;
    QString name;
    QString bindAddress = QStringLiteral("127.0.0.1");
    quint16 bindPort = 0;
    QString targetHost;
    quint16 targetPort = 0;

    [[nodiscard]] QString description() const;
};

// Manages port forwards for one SSH session. Runs its listeners on a
// dedicated worker thread; all libssh calls on the session are serialized
// with the session's own mutex (see SshSession::sessionMutex()).
class PortForwardManager : public QObject {
    Q_OBJECT

public:
    explicit PortForwardManager(SshSession *session, QObject *parent = nullptr);
    ~PortForwardManager() override;

    // Starts the forward; false + errorMessage on invalid spec or bind
    // failure. Remote forwards may fail asynchronously (server-side bind).
    bool addForward(const ForwardSpec &spec, QString *errorMessage = nullptr);
    void removeForward(int index);
    void clear();

    [[nodiscard]] QList<ForwardSpec> forwards() const;
    [[nodiscard]] bool isActive(int index) const;
    // Human-readable status: "listening on 127.0.0.1:8080" or an error.
    [[nodiscard]] QString statusText(int index) const;
    [[nodiscard]] int count() const;

signals:
    void forwardsChanged();

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace hssh

#endif // HSSH_CORE_PORTFORWARD_H

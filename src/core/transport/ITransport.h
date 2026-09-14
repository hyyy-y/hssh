#ifndef HSSH_CORE_TRANSPORT_ITRANSPORT_H
#define HSSH_CORE_TRANSPORT_ITRANSPORT_H

#include <QByteArray>
#include <QString>

class QObject;

namespace hssh {

class SessionConfig;

// Transport abstraction for a connection to a remote endpoint.
//
// Every implementation is also a QObject and emits these signals (names and
// signatures are part of the contract):
//   - void stateChanged(State state)
//   - void connected()
//   - void disconnected()
//   - void errorOccurred(const QString &message)
//   - void connectionLost()          // transport dropped; auto-reconnect may run
//   - void dataReceived(const QByteArray &data)
//
// Use qobjectFromTransport() to reach the QObject for signal connections.
// SshSession implements this interface; Telnet/Serial/Raw transports
// (Phase 3) plug in behind the same seam so TerminalSession/SessionTab never
// depend on a concrete protocol.
class ITransport {
public:
    enum class State {
        Disconnected,
        Connecting,
        Authenticating,
        Connected,
        Error
    };

    virtual ~ITransport() = default;

    // Start an asynchronous connection. Progress/failure arrives via signals.
    virtual void connectToHost() = 0;
    virtual void disconnect() = 0;
    // Write raw bytes to the remote endpoint.
    virtual void write(const QByteArray &data) = 0;
    // Terminal size change (PTY/window size for SSH/Telnet).
    virtual void resize(int columns, int rows) = 0;
    [[nodiscard]] virtual bool isConnected() const = 0;
};

// Dynamic_cast helper for connecting to an ITransport's signals.
inline QObject *qobjectFromTransport(ITransport *transport)
{
    return dynamic_cast<QObject *>(transport);
}

} // namespace hssh

#endif // HSSH_CORE_TRANSPORT_ITRANSPORT_H

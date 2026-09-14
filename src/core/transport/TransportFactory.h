#ifndef HSSH_CORE_TRANSPORT_TRANSPORTFACTORY_H
#define HSSH_CORE_TRANSPORT_TRANSPORTFACTORY_H

#include <QString>

namespace hssh {

class SessionConfig;
class ITransport;

// Creates the transport for a SessionConfig. SSH is always available;
// Telnet/Serial/Raw are planned (Phase 3) and return nullptr with an error
// message until implemented.
class TransportFactory {
public:
    // Returns nullptr + errorMessage when the transport type is unavailable.
    static ITransport *create(const SessionConfig &config, QString *errorMessage = nullptr);
};

} // namespace hssh

#endif // HSSH_CORE_TRANSPORT_TRANSPORTFACTORY_H

#include "TransportFactory.h"

#include "core/SessionConfig.h"
#include "core/transport/ITransport.h"

#include <QCoreApplication>

namespace hssh {

ITransport *TransportFactory::create(const SessionConfig &config, QString *errorMessage)
{
    switch (config.sessionType()) {
    case SessionType::Ssh:
        // The SSH transport is created through ConnectionManager (it owns the
        // session lifecycle and reader thread); SshShellProcess wires it up.
        // Returning nullptr here tells callers to use the SSH-specific path.
        return nullptr;
    case SessionType::Telnet:
    case SessionType::Serial:
    case SessionType::Raw:
        if (errorMessage) {
            *errorMessage = QCoreApplication::translate("TransportFactory",
                                                        "Transport not implemented yet (Phase 3).");
        }
        return nullptr;
    case SessionType::Local:
        if (errorMessage) {
            *errorMessage = QCoreApplication::translate("TransportFactory",
                                                        "Local shells do not use a network transport.");
        }
        return nullptr;
    }
    if (errorMessage) {
        *errorMessage = QCoreApplication::translate("TransportFactory", "Unknown session type.");
    }
    return nullptr;
}

} // namespace hssh

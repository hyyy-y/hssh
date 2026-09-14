#ifndef HSSH_AGENT_AGENTSUDOAUTH_H
#define HSSH_AGENT_AGENTSUDOAUTH_H

#include "core/SessionConfig.h"
#include "utils/Config.h"

#include <QString>
#include <QStringList>

namespace hssh::AgentSudoAuth {

// Stable sudo identity of a session/tab for the permanent-allow list:
// "session:<id>" for SAVED sessions (name comes from the repository), else
// "host:<user@host>" for ad-hoc connections, empty for anything else (local
// tabs are never permanent). NB: ad-hoc SessionConfigs carry a random
// auto-generated id, so their id is useless as a permanent key - only the
// host identity is stable across runs.
inline QString identityFor(const SessionConfig &config)
{
    if (!config.name().isEmpty() && !config.id().isEmpty()) {
        return QStringLiteral("session:") + config.id();
    }
    if (!config.host().isEmpty()) {
        return QStringLiteral("host:") + config.username()
               + QLatin1Char('@') + config.host();
    }
    return QString();
}

inline QString configKey()
{
    return QStringLiteral("agent/sudoAlwaysAllow");
}

// Permanent sudo grant list (Config key agent/sudoAlwaysAllow). Entries are
// added by the "Always Allow" choice in the sudo consent dialogs; revoke by
// editing the config file (no UI for list editing yet).
inline bool isAlwaysAllowed(const QString &identity)
{
    if (identity.isEmpty()) {
        return false;
    }
    return Config::instance().value(configKey()).toStringList().contains(identity);
}

inline void setAlwaysAllowed(const QString &identity)
{
    if (identity.isEmpty()) {
        return;
    }
    QStringList list = Config::instance().value(configKey()).toStringList();
    if (!list.contains(identity)) {
        list.append(identity);
        Config::instance().setValue(configKey(), list);
        Config::instance().sync();
    }
}

} // namespace hssh::AgentSudoAuth

#endif // HSSH_AGENT_AGENTSUDOAUTH_H

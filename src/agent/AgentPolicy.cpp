#include "AgentPolicy.h"

#include "utils/Config.h"

#include <QHash>
#include <QStringList>

namespace hssh {

namespace {

const QString &configKey()
{
    static const QString key = QStringLiteral("agent/policyRules");
    return key;
}

QString entryKey(const QString &identity, AgentPolicy::Operation operation)
{
    return QStringLiteral("%1|%2").arg(identity, AgentPolicy::operationName(operation));
}

} // namespace

AgentPolicy &AgentPolicy::instance()
{
    static AgentPolicy policy;
    return policy;
}

AgentPolicy::Decision AgentPolicy::check(const QString &identity, Operation operation) const
{
    if (identity.isEmpty()) {
        return Decision::Allow; // local tabs: nothing remote to protect
    }
    if (m_sessionGrants.contains(entryKey(identity, operation))) {
        return Decision::Allow;
    }
    const QString rule = ruleFor(identity, operation);
    if (rule == QLatin1String("allow")) {
        return Decision::Allow;
    }
    if (rule == QLatin1String("deny")) {
        return Decision::Deny;
    }
    if (rule == QLatin1String("ask")) {
        return Decision::Ask;
    }
    return defaultDecision(operation);
}

void AgentPolicy::grantSession(const QString &identity, Operation operation)
{
    if (identity.isEmpty()) {
        return;
    }
    m_sessionGrants.append(entryKey(identity, operation));
}

void AgentPolicy::grantAlways(const QString &identity, Operation operation)
{
    writeRule(identity, operation, QStringLiteral("allow"));
}

void AgentPolicy::setRule(const QString &identity, Operation operation, Decision decision)
{
    writeRule(identity, operation,
              decision == Decision::Allow ? QStringLiteral("allow")
              : decision == Decision::Deny ? QStringLiteral("deny")
                                           : QStringLiteral("ask"));
}

AgentPolicy::Decision AgentPolicy::defaultDecision(Operation operation)
{
    // First use per host asks; the dialog offers "always"/"this session".
    switch (operation) {
    case Operation::Upload:
    case Operation::Download:
    case Operation::Forward:
        return Decision::Ask;
    }
    return Decision::Ask;
}

QString AgentPolicy::operationName(Operation operation)
{
    switch (operation) {
    case Operation::Upload: return QStringLiteral("upload");
    case Operation::Download: return QStringLiteral("download");
    case Operation::Forward: return QStringLiteral("forward");
    }
    return QStringLiteral("unknown");
}

void AgentPolicy::clearSessionGrants()
{
    m_sessionGrants.clear();
}

QString AgentPolicy::ruleFor(const QString &identity, Operation operation) const
{
    const QStringList rules = Config::instance().value(configKey()).toStringList();
    const QString wanted = entryKey(identity, operation) + QLatin1Char('=');
    for (const QString &rule : rules) {
        if (rule.startsWith(wanted)) {
            return rule.mid(wanted.size());
        }
    }
    return QString();
}

void AgentPolicy::writeRule(const QString &identity, Operation operation,
                            const QString &decision)
{
    if (identity.isEmpty()) {
        return;
    }
    const QString entry = entryKey(identity, operation) + QLatin1Char('=') + decision;
    QStringList rules = Config::instance().value(configKey()).toStringList();
    const QString prefix = entryKey(identity, operation) + QLatin1Char('=');
    for (int i = 0; i < rules.size(); ++i) {
        if (rules.at(i).startsWith(prefix)) {
            rules[i] = entry;
            Config::instance().setValue(configKey(), rules);
            Config::instance().sync();
            return;
        }
    }
    rules.append(entry);
    Config::instance().setValue(configKey(), rules);
    Config::instance().sync();
}

} // namespace hssh

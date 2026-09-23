#ifndef HSSH_AGENT_AGENTPOLICY_H
#define HSSH_AGENT_AGENTPOLICY_H

#include <QString>
#include <QStringList>

namespace hssh {

// B5-2: per-host consent gate for the agent's sensitive operations.
// Decisions combine, in order:
//   1. session grants  — in-memory "allow for this GUI run" (the "allow for
//                        this session" dialog choice; resets on restart)
//   2. persistent rules — Config "agent/policyRules" entries
//                        "<identity>|<operation>=(allow|ask|deny)" written by
//                        the "always allow" dialog choice or by hand
//   3. defaults         — Upload/Download/Forward: Ask (a dialog in the GUI
//                        must confirm the first use per host); Exec: Allow
//                        (the visible terminal tab IS the audit trail).
// sudo is NOT covered here: it keeps its own consent chain (confirmSudo /
// AgentSudoAuth), which is stricter (per-command dialog).
//
// All methods are GUI-thread only (like TransferRegistry). Both the GUI and
// the --agent-mcp bridge read the same Config file, so rules are shared.
class AgentPolicy {
public:
    enum class Operation {
        Upload,
        Download,
        Forward,
    };
    enum class Decision {
        Allow,
        Ask,
        Deny,
    };

    static AgentPolicy &instance();

    // Rule lookup for identity+operation. An EMPTY identity (local tabs)
    // always yields Allow — there is no remote surface to protect.
    [[nodiscard]] Decision check(const QString &identity, Operation operation) const;

    // "Allow for this session" dialog choice.
    void grantSession(const QString &identity, Operation operation);
    // "Always allow" dialog choice — persisted for restarts.
    void grantAlways(const QString &identity, Operation operation);
    // Explicit rule (persisted); used by tests and hand-edited configs.
    void setRule(const QString &identity, Operation operation, Decision decision);
    [[nodiscard]] static Decision defaultDecision(Operation operation);
    [[nodiscard]] static QString operationName(Operation operation);

    // Test support: wipe the in-memory session grants.
    void clearSessionGrants();

private:
    AgentPolicy() = default;

    // "<identity>|<operation>" -> decision text, from Config.
    [[nodiscard]] QString ruleFor(const QString &identity, Operation operation) const;
    void writeRule(const QString &identity, Operation operation, const QString &decision);

    // In-memory session grants: "identity|operation" set.
    QStringList m_sessionGrants;
};

} // namespace hssh

#endif // HSSH_AGENT_AGENTPOLICY_H

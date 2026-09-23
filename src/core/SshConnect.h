#ifndef HSSH_CORE_SSHCONNECT_H
#define HSSH_CORE_SSHCONNECT_H

#include "KeyStore.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

using ssh_session = struct ssh_session_struct *;

namespace hssh {

class SessionConfig;

// PH2-13: interactive keyboard-interactive (2FA) round. Fills answers to
// match prompts; returns false when the user cancelled.
using KbdintPrompter = std::function<bool(const QString &name, const QString &instruction,
                                          const QStringList &prompts, const QList<bool> &echo,
                                          QStringList *answers)>;

// Blocking connect + authenticate. Intended for use on a worker thread;
// never call from the GUI thread. On success returns a connected,
// authenticated session owned by the caller. On failure returns nullptr and
// fills errorMessage (connect timeout is 10 seconds).
// postConnectTimeoutSec bounds every blocking call AFTER connect (channel
// open, flush, ...); 0 (default) waits indefinitely. Bulk SFTP sessions must
// keep 0 — a timed-out channel write is a SILENT short write (upload
// corruption); exec-style sessions should pass a bound so a wedged sshd
// cannot hang the worker forever.
// hostKeyVerifier (PH2-12) is consulted when the server key is unknown or
// changed; a null verifier auto-accepts new keys, rejects changed ones.
ssh_session sshConnectAndAuthenticate(const SessionConfig &config, QString *errorMessage,
                                      long postConnectTimeoutSec = 0,
                                      const KeyStore::HostKeyVerifier &hostKeyVerifier = {},
                                      const KbdintPrompter &kbdintPrompter = {});

// Releases a session returned by sshConnectAndAuthenticate.
void sshDisconnectAndFree(ssh_session session);

} // namespace hssh

#endif // HSSH_CORE_SSHCONNECT_H

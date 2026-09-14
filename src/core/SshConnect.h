#ifndef HSSH_CORE_SSHCONNECT_H
#define HSSH_CORE_SSHCONNECT_H

#include <QString>

using ssh_session = struct ssh_session_struct *;

namespace hssh {

class SessionConfig;

// Blocking connect + authenticate. Intended for use on a worker thread;
// never call from the GUI thread. On success returns a connected,
// authenticated session owned by the caller. On failure returns nullptr and
// fills errorMessage (connect timeout is 10 seconds).
// postConnectTimeoutSec bounds every blocking call AFTER connect (channel
// open, flush, ...); 0 (default) waits indefinitely. Bulk SFTP sessions must
// keep 0 — a timed-out channel write is a SILENT short write (upload
// corruption); exec-style sessions should pass a bound so a wedged sshd
// cannot hang the worker forever.
ssh_session sshConnectAndAuthenticate(const SessionConfig &config, QString *errorMessage,
                                      long postConnectTimeoutSec = 0);

// Releases a session returned by sshConnectAndAuthenticate.
void sshDisconnectAndFree(ssh_session session);

} // namespace hssh

#endif // HSSH_CORE_SSHCONNECT_H

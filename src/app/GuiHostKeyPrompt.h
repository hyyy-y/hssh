#ifndef HSSH_APP_GUIHOSTKEYPROMPT_H
#define HSSH_APP_GUIHOSTKEYPROMPT_H

#include "core/KeyStore.h"

namespace hssh {

// Thread-safe host-key decision honoring security/hostKeyPolicy
// ("accept-new" default / "ask" / "accept-all"):
//   accept-all — always Accept.
//   accept-new — Accept unknown keys (TOFU), Reject changed keys.
//   ask        — Accept unknown/changed keys only after a GUI dialog
//                approves (60 s auto-reject; works from ANY thread via a
//                blocking queued hop to the GUI thread).
// Background channels (monitor/process/docker panels, transfers) call this
// instead of passing no verifier — the "ask" policy previously reached only
// the visible terminal tab, leaving every background connection on plain
// TOFU even when the user asked to be prompted.
[[nodiscard]] KeyStore::HostKeyDecision decideHostKey(const KeyStore::HostKeyInfo &info,
                                                      bool changed);

} // namespace hssh

#endif // HSSH_APP_GUIHOSTKEYPROMPT_H

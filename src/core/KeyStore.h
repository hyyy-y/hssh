#ifndef HSSH_CORE_KEYSTORE_H
#define HSSH_CORE_KEYSTORE_H

#include <QString>

struct ssh_key_struct;
typedef struct ssh_key_struct *ssh_key;
typedef struct ssh_session_struct *ssh_session;

namespace hssh {

// Wraps libssh pki + known_hosts operations. Used by the key manager
// (PH1-09) and the host fingerprint flow (PH2-12). All functions are safe
// to call from a worker thread.
class KeyStore {
public:
    enum class KeyType { Ed25519, Rsa2048, Rsa4096, Ecdsa256, Ecdsa384 };
    enum class KnownHostStatus {
        Unknown,     // not present in known_hosts
        KnownOk,     // host key matches
        Changed,     // host key differs from the stored one
        Other,       // other failure (read error etc.)
    };

    // Directory where generated/imported private keys are stored
    // (%APPDATA%/hssh/keys on Windows, ~/.hssh/keys elsewhere).
    [[nodiscard]] static QString keysDir();
    // Path of the known_hosts file used by hssh.
    [[nodiscard]] static QString knownHostsPath();

    // Generate a key pair; privateKeyPath receives the private key file.
    // Returns false + errorMessage on failure. Passphrase may be empty.
    static bool generateKeyPair(KeyType type, const QString &privateKeyPath,
                                const QString &passphrase, QString *errorMessage = nullptr);

    // Import a private key file (OpenSSH / PEM). Returns nullptr + error on
    // failure; caller frees the key with ssh_key_free.
    static ssh_key importPrivateKey(const QString &path, const QString &passphrase,
                                    QString *errorMessage = nullptr);

    // Export the public key in OpenSSH authorized_keys format.
    [[nodiscard]] static QString publicKeyAuthorized(ssh_key key, QString *errorMessage = nullptr);
    // SHA256 base64 fingerprint ("SHA256:...").
    [[nodiscard]] static QString fingerprintSha256(ssh_key key);
    // MD5 hex fingerprint ("aa:bb:...").
    [[nodiscard]] static QString fingerprintMd5(ssh_key key);

    // --- known_hosts (operates on a connected ssh_session) ---
    [[nodiscard]] static KnownHostStatus checkKnownHost(ssh_session session);
    // Persist the host key the session just saw.
    static bool writeKnownHost(ssh_session session, QString *errorMessage = nullptr);
    // Remove all entries for a host (host may include port as host:port).
    static bool removeKnownHost(const QString &host, QString *errorMessage = nullptr);
};

} // namespace hssh

#endif // HSSH_CORE_KEYSTORE_H

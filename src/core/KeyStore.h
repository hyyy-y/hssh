#ifndef HSSH_CORE_KEYSTORE_H
#define HSSH_CORE_KEYSTORE_H

#include <QString>
#include <QVector>

#include <functional>

struct ssh_key_struct;
typedef struct ssh_key_struct *ssh_key;
typedef struct ssh_session_struct *ssh_session;

namespace hssh {

// Wraps libssh pki + known_hosts operations. Used by the key manager
// (PH1-09) and the host fingerprint flow (PH2-12). All functions are safe
// to call from a worker thread.
class KeyStore {
public:
    // One key in KeyStore::keysDir() as shown by listKeys().
    struct KeyInfo {
        QString fileName;
        QString filePath;
        QString type;               // "ssh-ed25519" etc. (empty when encrypted)
        QString fingerprintSha256;  // "SHA256:..." (empty when encrypted)
        bool encrypted = false;     // needs a passphrase to read
    };

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

    // --- high-level API used by the Key Manager dialog (PH1-09) ---

    // Scan keysDir() for private keys. Keys protected by a passphrase are
    // listed with encrypted=true (type/fingerprint stay empty).
    [[nodiscard]] static QVector<KeyInfo> listKeys();

    // Generate a key pair into keysDir() under name (path separators are
    // stripped, the name is uniquified when it collides). Also writes the
    // matching <name>.pub in authorized_keys format.
    static bool generateKey(KeyType type, const QString &name, const QString &passphrase,
                            QString *filePath = nullptr, QString *errorMessage = nullptr);

    // Copy an existing private key file into keysDir() after verifying it
    // imports with the given passphrase (may be empty). The stored name is
    // uniquified; filePath receives the destination.
    static bool importKeyFile(const QString &sourcePath, const QString &passphrase,
                              QString *filePath = nullptr, QString *errorMessage = nullptr);

    // authorized_keys line ("ssh-ed25519 AAAA... comment") for a key file.
    static bool exportPublicKey(const QString &filePath, const QString &passphrase,
                                QString *authorizedLine, QString *errorMessage = nullptr);

    // Remove a key file and its .pub sibling from keysDir().
    static bool deleteKey(const QString &filePath, QString *errorMessage = nullptr);

    // --- low-level pki helpers ---

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

    // --- host key verification flow (PH2-12) ---
    struct HostKeyInfo {
        QString host;               // "host" or "host:port"
        QString keyType;            // "ssh-ed25519" etc.
        QString fingerprintSha256;  // "SHA256:..."
        QString fingerprintMd5;     // "aa:bb:..."
    };
    enum class HostKeyDecision { Accept, Reject };
    using HostKeyVerifier = std::function<HostKeyDecision(const HostKeyInfo &, bool changed)>;

    // Verify the server key of a CONNECTED session against known_hosts.
    // KnownOk passes; Unknown/Changed ask the verifier (a null verifier
    // auto-accepts new keys but rejects changed ones — TOFU). Accepting a
    // changed key removes the stale entry first. Reject fails with a
    // descriptive error.
    static bool verifyAndStoreHostKey(ssh_session session, const HostKeyVerifier &verifier,
                                      QString *errorMessage = nullptr);

    // One known_hosts line for the manager UI.
    struct KnownHostEntry {
        QString hosts;              // "host[,ip]" column
        QString keyType;            // "ssh-ed25519" etc.
        QString fingerprintSha256;  // "SHA256:..." of the key blob
    };
    [[nodiscard]] static QVector<KnownHostEntry> listKnownHosts();
};

} // namespace hssh

#endif // HSSH_CORE_KEYSTORE_H

#ifndef HSSH_UTILS_CRYPTO_H
#define HSSH_UTILS_CRYPTO_H

#include <QByteArray>
#include <QString>
#include <memory>

namespace hssh {

// Holds a sensitive byte string in heap memory and wipes it on destruction.
// This is a memory-hygiene helper only; persistence encryption lives in
// namespace Crypto (AES-256-GCM).
class SecureString {
public:
    SecureString();
    explicit SecureString(const QString &value);
    explicit SecureString(const QByteArray &value);
    ~SecureString();

    SecureString(const SecureString &other);
    SecureString(SecureString &&other) noexcept;
    SecureString &operator=(const SecureString &other);
    SecureString &operator=(SecureString &&other) noexcept;

    [[nodiscard]] QString toString() const;
    [[nodiscard]] QByteArray toByteArray() const;
    [[nodiscard]] bool isEmpty() const;

    void clear();

private:
    void resize(std::size_t size);

    std::unique_ptr<char[]> m_data;
    std::size_t m_size = 0;
};

// At-rest encryption for session secrets (AES-256-GCM via mbedTLS).
//
// Blob format (stored in the sessions table):
//   "HSE1" | 12-byte nonce | ciphertext | 16-byte GCM tag
//
// Two key modes, chosen for multi-device deployment:
//   - Machine mode (default, transparent): key derived from the machine's
//     unique id. The encrypted DB is bound to this machine; moving it to
//     another device will not decrypt. Use JSON export for portability.
//   - Master password mode (opt-in, portable): key derived from a user
//     password. The DB decrypts on any device where the same password is
//     entered. Unlock is required once per app start.
namespace Crypto {

// False when built without mbedTLS (encryptData then passes plaintext
// through, documented degradation).
bool isAvailable();

// Encrypts with the active key. Returns plaintext unchanged when crypto is
// unavailable; returns an empty array when locked (master mode, not
// unlocked).
QByteArray encryptData(const QByteArray &plaintext);
// Decrypts a blob produced by encryptData. Data without the magic prefix is
// treated as legacy plaintext (returned unchanged, ok=true). ok=false on
// wrong key, tampering, or locked state.
QByteArray decryptData(const QByteArray &blob, bool *ok = nullptr);

// Key mode management.
bool usesMasterPassword();
bool isUnlocked();
// Verifies the password against the stored verifier and activates the
// master key. False on wrong password.
bool unlock(const QString &password);
void lock();
// Enables master password mode and stores the verifier. The caller must
// re-encrypt existing data with the new key afterwards.
bool setupMasterPassword(const QString &password);
// Disables master password mode (falls back to the machine key).
void clearMasterPassword();

// --- Agent RSA key pair (password cipher channel) -------------------------
// The agent never handles plaintext passwords: clients encrypt with the
// public key, only this process decrypts. The private key is stored
// GCM-encrypted, so it additionally requires an unlocked active key (when
// master password mode is enabled, locked means unusable).

// Generates the key pair on first call; loads it afterwards. False when
// crypto is compiled out or the private key cannot be unlocked.
bool ensureAgentKeyPair();
// PEM-encoded RSA public key (empty when unavailable/locked).
QByteArray agentPublicKeyPem();
// SHA-256 fingerprint of the public key (hex string).
QString agentKeyFingerprint();
// Decrypts base64(RSA-OAEP-SHA256(password)) with the private key.
// ok=false on locked state, malformed input, or decrypt failure.
QByteArray rsaDecrypt(const QByteArray &cipherBase64, bool *ok = nullptr);
// Encrypts with an RSA public key (OAEP-SHA256); used by tests and by
// clients that want to verify the cipher locally.
QByteArray rsaEncryptWithPublicKey(const QByteArray &plaintext, const QByteArray &publicKeyPem,
                                   bool *ok = nullptr);

// Legacy helpers kept for API compatibility (no longer used).
QByteArray deriveKey(const QString &password, const QByteArray &salt);
QByteArray encrypt(const QByteArray &plaintext, const QByteArray &key, QByteArray &outNonce);
QByteArray decrypt(const QByteArray &ciphertext, const QByteArray &key, const QByteArray &nonce);

} // namespace Crypto

} // namespace hssh

#endif // HSSH_UTILS_CRYPTO_H

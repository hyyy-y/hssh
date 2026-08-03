#ifndef HSSH_UTILS_CRYPTO_H
#define HSSH_UTILS_CRYPTO_H

#include <QByteArray>
#include <QString>
#include <memory>

namespace hssh {

// SecureString holds a secret in locked heap memory and clears it on destruction.
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

    QString toString() const;
    QByteArray toByteArray() const;
    bool isEmpty() const;
    void clear();

private:
    void resize(std::size_t size);

    std::unique_ptr<char[]> m_data;
    std::size_t m_size = 0;
};

// Placeholder AES-256-GCM encryption helpers.
// The actual implementation requires OpenSSL or a similar library and will be
// introduced when user-data encryption is wired end-to-end.
namespace Crypto {

QByteArray deriveKey(const QString &password, const QByteArray &salt);

// Stubs: currently return data unchanged, marking the boundary where encryption
// will be applied once the crypto dependency is integrated.
QByteArray encrypt(const QByteArray &plaintext, const QByteArray &key, QByteArray &outNonce);
QByteArray decrypt(const QByteArray &ciphertext, const QByteArray &key, const QByteArray &nonce);

} // namespace Crypto

} // namespace hssh

#endif // HSSH_UTILS_CRYPTO_H

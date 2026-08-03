#include "Crypto.h"

#include <QCryptographicHash>
#include <QtGlobal>

#include <cstring>

namespace hssh {

SecureString::SecureString() = default;

SecureString::SecureString(const QString &value)
    : SecureString(value.toUtf8())
{
}

SecureString::SecureString(const QByteArray &value)
{
    resize(static_cast<std::size_t>(value.size()));
    if (m_size > 0) {
        std::memcpy(m_data.get(), value.constData(), m_size);
    }
}

SecureString::~SecureString()
{
    clear();
}

SecureString::SecureString(const SecureString &other)
    : SecureString(other.toByteArray())
{
}

SecureString::SecureString(SecureString &&other) noexcept
    : m_data(std::move(other.m_data))
    , m_size(other.m_size)
{
    other.m_size = 0;
}

SecureString &SecureString::operator=(const SecureString &other)
{
    if (this != &other) {
        clear();
        const QByteArray data = other.toByteArray();
        resize(static_cast<std::size_t>(data.size()));
        if (m_size > 0) {
            std::memcpy(m_data.get(), data.constData(), m_size);
        }
    }
    return *this;
}

SecureString &SecureString::operator=(SecureString &&other) noexcept
{
    if (this != &other) {
        clear();
        m_data = std::move(other.m_data);
        m_size = other.m_size;
        other.m_size = 0;
    }
    return *this;
}

QString SecureString::toString() const
{
    return QString::fromUtf8(reinterpret_cast<const char *>(m_data.get()), static_cast<int>(m_size));
}

QByteArray SecureString::toByteArray() const
{
    return QByteArray(m_data.get(), static_cast<int>(m_size));
}

bool SecureString::isEmpty() const
{
    return m_size == 0;
}

void SecureString::clear()
{
    if (m_data) {
        std::memset(m_data.get(), 0, m_size);
    }
    m_data.reset();
    m_size = 0;
}

void SecureString::resize(std::size_t size)
{
    clear();
    if (size > 0) {
        m_data = std::make_unique<char[]>(size + 1);
        m_data[size] = '\0';
    }
    m_size = size;
}

namespace Crypto {

QByteArray deriveKey(const QString &password, const QByteArray &salt)
{
    QByteArray data = password.toUtf8() + salt;
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QByteArray encrypt(const QByteArray &plaintext, const QByteArray &key, QByteArray &outNonce)
{
    Q_UNUSED(key)
    outNonce.clear();
    return plaintext;
}

QByteArray decrypt(const QByteArray &ciphertext, const QByteArray &key, const QByteArray &nonce)
{
    Q_UNUSED(key)
    Q_UNUSED(nonce)
    return ciphertext;
}

} // namespace Crypto

} // namespace hssh

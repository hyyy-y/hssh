#include "Crypto.h"

#include "utils/Config.h"

#include <QCryptographicHash>
#include <QtGlobal>

#ifdef HSSH_HAS_LIBSSH
#include <QRandomGenerator>
#include <QSysInfo>
#include <mbedtls/gcm.h>
#include <mbedtls/pk.h>
#endif

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

#ifdef HSSH_HAS_LIBSSH

namespace {

constexpr char kMagic[] = "HSE1";
constexpr std::size_t kMagicLen = 4;
constexpr std::size_t kNonceLen = 12;
constexpr std::size_t kTagLen = 16;
constexpr char kKeyInfo[] = "hssh-crypto-v2";
constexpr char kVerifierInfo[] = "hssh-crypto-verifier-v2";

QByteArray sha256(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

// Machine-bound default key: derived from the OS machine GUID, so the
// encrypted database cannot be copied to another device and decrypted there.
QByteArray machineKey()
{
    QByteArray id = QSysInfo::machineUniqueId();
    if (id.isEmpty()) {
        id = QSysInfo::machineHostName().toUtf8();
    }
    return sha256(id + QByteArray(kKeyInfo));
}

QByteArray masterKey(const QString &password)
{
    return sha256(password.toUtf8() + QByteArray(kKeyInfo));
}

QByteArray verifierFor(const QString &password)
{
    return sha256(masterKey(password) + QByteArray(kVerifierInfo));
}

bool g_unlocked = false;
QByteArray g_masterKey;

// Active encryption key; empty when master mode is on but locked.
QByteArray activeKey()
{
    if (Config::instance().boolValue(QStringLiteral("security/masterEnabled"), false)) {
        return g_unlocked ? g_masterKey : QByteArray();
    }
    return machineKey();
}

QByteArray gcmCrypt(bool encrypt, const QByteArray &in, const QByteArray &key,
                    const QByteArray &nonce, QByteArray *tag)
{
    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    QByteArray out(in.size(), Qt::Uninitialized);
    int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES,
                                 reinterpret_cast<const unsigned char *>(key.constData()), 256);
    if (rc == 0) {
        if (encrypt) {
            rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT,
                                            static_cast<size_t>(in.size()),
                                            reinterpret_cast<const unsigned char *>(nonce.constData()),
                                            nonce.size(), nullptr, 0,
                                            reinterpret_cast<const unsigned char *>(in.constData()),
                                            reinterpret_cast<unsigned char *>(out.data()),
                                            tag->size(),
                                            reinterpret_cast<unsigned char *>(tag->data()));
        } else {
            rc = mbedtls_gcm_auth_decrypt(&ctx, static_cast<size_t>(in.size()),
                                           reinterpret_cast<const unsigned char *>(nonce.constData()),
                                           nonce.size(), nullptr, 0,
                                           reinterpret_cast<const unsigned char *>(tag->constData()),
                                           tag->size(),
                                           reinterpret_cast<const unsigned char *>(in.constData()),
                                           reinterpret_cast<unsigned char *>(out.data()));
        }
    }
    mbedtls_gcm_free(&ctx);
    if (rc != 0) {
        return {};
    }
    return out;
}

} // namespace

bool isAvailable()
{
    return true;
}

QByteArray encryptData(const QByteArray &plaintext)
{
    if (plaintext.isEmpty()) {
        return plaintext;
    }
    const QByteArray key = activeKey();
    if (key.isEmpty()) {
        return {}; // Locked master mode: refuse to write secrets.
    }
    QByteArray nonce(kNonceLen, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(nonce.data()),
                                          kNonceLen / sizeof(quint32));
    QByteArray tag(kTagLen, Qt::Uninitialized);
    const QByteArray ciphertext = gcmCrypt(true, plaintext, key, nonce, &tag);
    if (ciphertext.isEmpty() && !plaintext.isEmpty()) {
        return {};
    }
    QByteArray blob(kMagic, kMagicLen);
    blob.append(nonce).append(ciphertext).append(tag);
    return blob;
}

QByteArray decryptData(const QByteArray &blob, bool *ok)
{
    const auto fail = [ok]() {
        if (ok) {
            *ok = false;
        }
        return QByteArray();
    };

    if (blob.isEmpty()) {
        if (ok) {
            *ok = true;
        }
        return blob;
    }
    if (!blob.startsWith(kMagic)) {
        // Legacy plaintext record (pre-encryption builds).
        if (ok) {
            *ok = true;
        }
        return blob;
    }
    if (blob.size() < static_cast<int>(kMagicLen + kNonceLen + kTagLen)) {
        return fail();
    }
    const QByteArray key = activeKey();
    if (key.isEmpty()) {
        return fail(); // Locked.
    }
    const QByteArray nonce = blob.mid(kMagicLen, kNonceLen);
    const QByteArray tag = blob.right(kTagLen);
    const QByteArray ciphertext = blob.mid(kMagicLen + kNonceLen,
                                           blob.size() - kMagicLen - kNonceLen - kTagLen);
    QByteArray tagCopy = tag;
    const QByteArray plaintext = gcmCrypt(false, ciphertext, key, nonce, &tagCopy);
    if (plaintext.isEmpty() && !ciphertext.isEmpty()) {
        return fail();
    }
    if (ok) {
        *ok = true;
    }
    return plaintext;
}

bool usesMasterPassword()
{
    return Config::instance().boolValue(QStringLiteral("security/masterEnabled"), false);
}

bool isUnlocked()
{
    return !usesMasterPassword() || g_unlocked;
}

bool unlock(const QString &password)
{
    if (!usesMasterPassword()) {
        return true;
    }
    const QString stored = Config::instance().stringValue(QStringLiteral("security/verifier"));
    if (stored.isEmpty() || verifierFor(password).toHex() != stored) {
        return false;
    }
    g_masterKey = masterKey(password);
    g_unlocked = true;
    return true;
}

void lock()
{
    g_masterKey.fill('\0');
    g_masterKey.clear();
    g_unlocked = false;
}

bool setupMasterPassword(const QString &password)
{
    if (password.isEmpty()) {
        return false;
    }
    Config::instance().setValue(QStringLiteral("security/verifier"),
                                QString::fromLatin1(verifierFor(password).toHex()));
    Config::instance().setValue(QStringLiteral("security/masterEnabled"), true);
    Config::instance().sync();
    g_masterKey = masterKey(password);
    g_unlocked = true;
    return true;
}

void clearMasterPassword()
{
    lock();
    Config::instance().setValue(QStringLiteral("security/masterEnabled"), false);
    Config::instance().remove(QStringLiteral("security/verifier"));
    Config::instance().sync();
}

// --- Agent RSA key pair ---

namespace {

int qtRng(void *, unsigned char *buf, size_t len)
{
    auto *gen = QRandomGenerator::system();
    size_t done = 0;
    while (done < len) {
        const quint32 v = gen->generate();
        const size_t chunk = qMin<size_t>(sizeof(v), len - done);
        std::memcpy(buf + done, &v, chunk);
        done += chunk;
    }
    return 0;
}

constexpr int kRsaBits = 2048;
QByteArray g_publicKeyPem;

bool generateAgentKeyPair(QByteArray *pubPem, QByteArray *privPem)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = false;
    if (mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == 0) {
        mbedtls_rsa_context *rsa = mbedtls_pk_rsa(pk);
        mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);
        if (mbedtls_rsa_gen_key(rsa, qtRng, nullptr, kRsaBits, 65537) == 0) {
            QByteArray pub(4000, Qt::Uninitialized);
            QByteArray priv(8000, Qt::Uninitialized);
            // mbedTLS 2.28 pk write_pem returns 0 on success (length via buf).
            const int pubRc = mbedtls_pk_write_pubkey_pem(
                &pk, reinterpret_cast<unsigned char *>(pub.data()), pub.size());
            const int privRc = mbedtls_pk_write_key_pem(
                &pk, reinterpret_cast<unsigned char *>(priv.data()), priv.size());
            if (pubRc == 0 && privRc == 0) {
                pub.truncate(static_cast<int>(strlen(pub.constData())));
                priv.truncate(static_cast<int>(strlen(priv.constData())));
                *pubPem = pub;
                *privPem = priv;
                ok = true;
            }
        }
    }
    mbedtls_pk_free(&pk);
    return ok;
}

// Decrypts the stored private key with the active key (requires unlocked).
QByteArray privateKeyPem(bool *ok)
{
    if (ok) {
        *ok = false;
    }
    if (!isUnlocked()) {
        return {};
    }
    const QByteArray enc = Config::instance()
                               .value(QStringLiteral("security/agentPrivKeyEnc"))
                               .toByteArray();
    if (enc.isEmpty()) {
        return {};
    }
    bool decOk = false;
    const QByteArray pem = decryptData(enc, &decOk);
    if (!decOk) {
        return {};
    }
    if (ok) {
        *ok = true;
    }
    return pem;
}

} // namespace

bool ensureAgentKeyPair()
{
    if (!isUnlocked()) {
        return false;
    }

    const QByteArray storedPub = Config::instance()
                                     .stringValue(QStringLiteral("security/agentPubKey"))
                                     .toUtf8();
    const QByteArray storedPrivEnc = Config::instance()
                                         .value(QStringLiteral("security/agentPrivKeyEnc"))
                                         .toByteArray();
    if (!storedPub.isEmpty() && !storedPrivEnc.isEmpty()) {
        bool ok = false;
        privateKeyPem(&ok);
        if (ok) {
            g_publicKeyPem = storedPub;
            return true;
        }
        return false; // Stored but not decryptable in the current state.
    }

    QByteArray pub;
    QByteArray priv;
    if (!generateAgentKeyPair(&pub, &priv)) {
        return false;
    }
    Config::instance().setValue(QStringLiteral("security/agentPubKey"), QString::fromUtf8(pub));
    Config::instance().setValue(QStringLiteral("security/agentPrivKeyEnc"), encryptData(priv));
    Config::instance().sync();
    priv.fill('\0');
    g_publicKeyPem = pub;
    return true;
}

QByteArray agentPublicKeyPem()
{
    if (!ensureAgentKeyPair()) {
        return {};
    }
    return g_publicKeyPem;
}

QString agentKeyFingerprint()
{
    const QByteArray pem = agentPublicKeyPem();
    if (pem.isEmpty()) {
        return {};
    }
    return QString::fromLatin1(sha256(pem).toHex());
}

QByteArray rsaDecrypt(const QByteArray &cipherBase64, bool *ok)
{
    const auto fail = [ok]() {
        if (ok) {
            *ok = false;
        }
        return QByteArray();
    };

    if (!isUnlocked()) {
        return fail();
    }
    const QByteArray cipher = QByteArray::fromBase64(cipherBase64);
    if (cipher.isEmpty() || cipher.size() > kRsaBits / 8) {
        return fail();
    }
    bool pemOk = false;
    const QByteArray pem = privateKeyPem(&pemOk);
    if (!pemOk || pem.isEmpty()) {
        return fail();
    }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    QByteArray pemNul = pem;
    if (!pemNul.endsWith('\0')) {
        pemNul.append('\0');
    }
    if (mbedtls_pk_parse_key(&pk, reinterpret_cast<const unsigned char *>(pemNul.constData()),
                             pemNul.size(), nullptr, 0) != 0) {
        mbedtls_pk_free(&pk);
        return fail();
    }
    mbedtls_rsa_set_padding(mbedtls_pk_rsa(pk), MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);

    QByteArray buf(kRsaBits / 8, Qt::Uninitialized);
    size_t olen = 0;
    const int rc = mbedtls_pk_decrypt(
        &pk, reinterpret_cast<const unsigned char *>(cipher.constData()), cipher.size(),
        reinterpret_cast<unsigned char *>(buf.data()), &olen, buf.size(), qtRng, nullptr);
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        return fail();
    }
    buf.truncate(static_cast<int>(olen));
    if (ok) {
        *ok = true;
    }
    return buf;
}

QByteArray rsaEncryptWithPublicKey(const QByteArray &plaintext, const QByteArray &publicKeyPem,
                                   bool *ok)
{
    const auto fail = [ok]() {
        if (ok) {
            *ok = false;
        }
        return QByteArray();
    };

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    QByteArray pemNul = publicKeyPem;
    if (!pemNul.endsWith('\0')) {
        pemNul.append('\0');
    }
    if (mbedtls_pk_parse_public_key(&pk, reinterpret_cast<const unsigned char *>(pemNul.constData()),
                                    pemNul.size()) != 0) {
        mbedtls_pk_free(&pk);
        return fail();
    }
    mbedtls_rsa_set_padding(mbedtls_pk_rsa(pk), MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA256);

    QByteArray buf(kRsaBits / 8, Qt::Uninitialized);
    size_t olen = 0;
    const int rc = mbedtls_pk_encrypt(
        &pk, reinterpret_cast<const unsigned char *>(plaintext.constData()), plaintext.size(),
        reinterpret_cast<unsigned char *>(buf.data()), &olen, buf.size(), qtRng, nullptr);
    mbedtls_pk_free(&pk);
    if (rc != 0) {
        return fail();
    }
    buf.truncate(static_cast<int>(olen));
    if (ok) {
        *ok = true;
    }
    return buf.toBase64();
}

#else // !HSSH_HAS_LIBSSH

bool isAvailable() { return false; }
QByteArray encryptData(const QByteArray &plaintext) { return plaintext; }
QByteArray decryptData(const QByteArray &blob, bool *ok)
{
    if (ok) { *ok = true; }
    return blob;
}
bool usesMasterPassword() { return false; }
bool isUnlocked() { return true; }
bool unlock(const QString &) { return true; }
void lock() {}
bool setupMasterPassword(const QString &) { return false; }
void clearMasterPassword() {}
bool ensureAgentKeyPair() { return false; }
QByteArray agentPublicKeyPem() { return {}; }
QString agentKeyFingerprint() { return {}; }
QByteArray rsaDecrypt(const QByteArray &, bool *ok)
{
    if (ok) { *ok = false; }
    return {};
}
QByteArray rsaEncryptWithPublicKey(const QByteArray &, const QByteArray &, bool *ok)
{
    if (ok) { *ok = false; }
    return {};
}

#endif // HSSH_HAS_LIBSSH

// Legacy no-op API (kept so existing callers keep compiling).
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

#include "KeyStore.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

#ifdef HSSH_HAS_LIBSSH
#include <libssh/libssh.h>
#endif

namespace hssh {

QString KeyStore::keysDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir = base + QDir::separator() + QStringLiteral("keys");
    QDir().mkpath(dir);
    return dir;
}

QString KeyStore::knownHostsPath()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir = base + QDir::separator() + QStringLiteral("known_hosts");
    return dir;
}

#ifdef HSSH_HAS_LIBSSH
namespace {

ssh_keytypes_e keyTypeToLibSsh(KeyStore::KeyType type)
{
    switch (type) {
    case KeyStore::KeyType::Ed25519:  return SSH_KEYTYPE_ED25519;
    case KeyStore::KeyType::Rsa2048:  return SSH_KEYTYPE_RSA;
    case KeyStore::KeyType::Rsa4096:  return SSH_KEYTYPE_RSA;
    case KeyStore::KeyType::Ecdsa256: return SSH_KEYTYPE_ECDSA_P256;
    case KeyStore::KeyType::Ecdsa384: return SSH_KEYTYPE_ECDSA_P384;
    }
    return SSH_KEYTYPE_RSA;
}

int keyTypeBits(KeyStore::KeyType type)
{
    switch (type) {
    case KeyStore::KeyType::Rsa2048:  return 2048;
    case KeyStore::KeyType::Rsa4096:  return 4096;
    case KeyStore::KeyType::Ecdsa256: return 256;
    case KeyStore::KeyType::Ecdsa384: return 384;
    default:                          return 0; // native size
    }
}

QString sshErrorString(ssh_session session)
{
    return session ? QString::fromUtf8(ssh_get_error(session)) : QStringLiteral("unknown error");
}

} // namespace
#endif // HSSH_HAS_LIBSSH

bool KeyStore::generateKeyPair(KeyType type, const QString &privateKeyPath,
                               const QString &passphrase, QString *errorMessage)
{
#ifdef HSSH_HAS_LIBSSH
    ssh_key key = nullptr;
    const int rc = ssh_pki_generate(keyTypeToLibSsh(type), keyTypeBits(type), &key);
    if (rc != SSH_OK) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to generate key: %1").arg(ssh_get_error(nullptr));
        }
        return false;
    }

    int exportRc = SSH_ERROR;
    if (passphrase.isEmpty()) {
        exportRc = ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr,
                                                privateKeyPath.toUtf8().constData());
    } else {
        exportRc = ssh_pki_export_privkey_file(key, passphrase.toUtf8().constData(),
                                                nullptr, nullptr,
                                                privateKeyPath.toUtf8().constData());
    }
    ssh_key_free(key);
    if (exportRc != SSH_OK) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to write private key file.");
        }
        return false;
    }
    return true;
#else
    Q_UNUSED(type)
    Q_UNUSED(privateKeyPath)
    Q_UNUSED(passphrase)
    if (errorMessage) {
        *errorMessage = QStringLiteral("libssh backend not available.");
    }
    return false;
#endif
}

ssh_key KeyStore::importPrivateKey(const QString &path, const QString &passphrase,
                                   QString *errorMessage)
{
#ifdef HSSH_HAS_LIBSSH
    ssh_key key = nullptr;
    const QByteArray pass = passphrase.isEmpty() ? QByteArray() : passphrase.toUtf8();
    const int rc = ssh_pki_import_privkey_file(path.toUtf8().constData(),
                                                pass.isEmpty() ? nullptr : pass.constData(),
                                                nullptr, nullptr, &key);
    if (rc != SSH_OK) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to import private key (wrong passphrase or unsupported format).");
        }
        return nullptr;
    }
    return key;
#else
    Q_UNUSED(path)
    Q_UNUSED(passphrase)
    if (errorMessage) {
        *errorMessage = QStringLiteral("libssh backend not available.");
    }
    return nullptr;
#endif
}

QString KeyStore::publicKeyAuthorized(ssh_key key, QString *errorMessage)
{
#ifdef HSSH_HAS_LIBSSH
    if (!key) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("No key.");
        }
        return {};
    }
    char *b64 = nullptr;
    const int rc = ssh_pki_export_pubkey_base64(key, &b64);
    if (rc != SSH_OK || !b64) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to export public key.");
        }
        return {};
    }
    QString result = QString::fromUtf8(b64);
    ssh_string_free_char(b64);
    const char *type = ssh_key_type_to_char(ssh_key_type(key));
    if (type) {
        result.prepend(QLatin1Char(' ')).prepend(QString::fromUtf8(type));
    }
    return result;
#else
    Q_UNUSED(key)
    if (errorMessage) {
        *errorMessage = QStringLiteral("libssh backend not available.");
    }
    return {};
#endif
}

QString KeyStore::fingerprintSha256(ssh_key key)
{
#ifdef HSSH_HAS_LIBSSH
    if (!key) {
        return {};
    }
    unsigned char *hash = nullptr;
    size_t hashLen = 0;
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLen) != SSH_OK || !hash) {
        return {};
    }
    char *hex = ssh_get_hexa(hash, hashLen);
    ssh_clean_pubkey_hash(&hash);
    if (!hex) {
        return {};
    }
    QString result = QString::fromUtf8(hex);
    ssh_string_free_char(hex);
    return QStringLiteral("SHA256:") + result;
#else
    Q_UNUSED(key)
    return {};
#endif
}

QString KeyStore::fingerprintMd5(ssh_key key)
{
#ifdef HSSH_HAS_LIBSSH
    if (!key) {
        return {};
    }
    unsigned char *hash = nullptr;
    size_t hashLen = 0;
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_MD5, &hash, &hashLen) != SSH_OK || !hash) {
        return {};
    }
    char *hex = ssh_get_hexa(hash, hashLen);
    ssh_clean_pubkey_hash(&hash);
    if (!hex) {
        return {};
    }
    QString result = QString::fromUtf8(hex);
    ssh_string_free_char(hex);
    // Format as colon-separated bytes, matching OpenSSH display.
    QStringList parts;
    for (int i = 0; i < result.size(); i += 2) {
        parts.append(result.mid(i, 2));
    }
    return parts.join(QLatin1Char(':'));
#else
    Q_UNUSED(key)
    return {};
#endif
}

KeyStore::KnownHostStatus KeyStore::checkKnownHost(ssh_session session)
{
#ifdef HSSH_HAS_LIBSSH
    if (!session) {
        return KnownHostStatus::Other;
    }
    ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, knownHostsPath().toUtf8().constData());
    const int rc = ssh_is_server_known(session);
    switch (rc) {
    case SSH_SERVER_KNOWN_OK:     return KnownHostStatus::KnownOk;
    case SSH_SERVER_KNOWN_CHANGED:return KnownHostStatus::Changed;
    case SSH_SERVER_FILE_NOT_FOUND:
    case SSH_SERVER_NOT_KNOWN:    return KnownHostStatus::Unknown;
    default:                      return KnownHostStatus::Other;
    }
#else
    Q_UNUSED(session)
    return KnownHostStatus::Other;
#endif
}

bool KeyStore::writeKnownHost(ssh_session session, QString *errorMessage)
{
#ifdef HSSH_HAS_LIBSSH
    if (!session) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("No session.");
        }
        return false;
    }
    ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, knownHostsPath().toUtf8().constData());
    const int rc = ssh_write_knownhost(session);
    if (rc != SSH_OK) {
        if (errorMessage) {
            *errorMessage = sshErrorString(session);
        }
        return false;
    }
    return true;
#else
    Q_UNUSED(session)
    if (errorMessage) {
        *errorMessage = QStringLiteral("libssh backend not available.");
    }
    return false;
#endif
}

bool KeyStore::removeKnownHost(const QString &host, QString *errorMessage)
{
#ifdef HSSH_HAS_LIBSSH
    // Rewrite known_hosts without the matching lines.
    const QString path = knownHostsPath();
    QFile file(path);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot read known_hosts.");
        }
        return false;
    }
    QStringList kept;
    const QByteArray needle = host.toUtf8();
    while (!file.atEnd()) {
        const QByteArray line = file.readLine();
        if (line.trimmed().startsWith(needle)) {
            continue;
        }
        kept.append(QString::fromUtf8(line));
    }
    file.close();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot write known_hosts.");
        }
        return false;
    }
    for (const QString &line : kept) {
        file.write(line.toUtf8());
    }
    file.close();
    return true;
#else
    Q_UNUSED(host)
    if (errorMessage) {
        *errorMessage = QStringLiteral("libssh backend not available.");
    }
    return false;
#endif
}

} // namespace hssh

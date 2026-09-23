#include "KeyStore.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    // OpenSSH display convention: unpadded base64, "SHA256:" prefix.
    const QByteArray b64 = QByteArray(reinterpret_cast<const char *>(hash), int(hashLen))
                               .toBase64(QByteArray::Base64Encoding | QByteArray::OmitTrailingEquals);
    ssh_clean_pubkey_hash(&hash);
    return QStringLiteral("SHA256:") + QString::fromLatin1(b64);
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
    // ssh_is_server_known is deprecated since 0.9; the 0.10 API is
    // ssh_session_is_known_server.
    const int rc = ssh_session_is_known_server(session);
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
    // ssh_write_knownhost is deprecated; ssh_session_update_known_hosts is
    // the 0.10 replacement.
    const int rc = ssh_session_update_known_hosts(session);
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

namespace {

// Strip path separators and other characters that are unsafe as a file name.
QString sanitizedKeyName(const QString &name)
{
    QString clean = name.trimmed();
    QString kept;
    for (const QChar c : clean) {
        const ushort u = c.unicode();
        const bool invalid = u == '/' || u == '\\' || u == ':' || u == '*' || u == '?'
                             || u == '"' || u == '<' || u == '>' || u == '|' || c.isSpace();
        if (!invalid) {
            kept.append(c);
        }
    }
    if (kept.isEmpty() || kept == QLatin1String(".") || kept == QLatin1String("..")) {
        return QStringLiteral("key");
    }
    return kept;
}

// First free "<stem>[-N]<suffix>" combination in dir (also avoids collisions
// with existing .pub siblings).
QString uniqueKeyPath(const QString &dir, const QString &name)
{
    const QFileInfo info(name);
    const QString stem = info.completeBaseName().isEmpty() ? QStringLiteral("key")
                                                           : info.completeBaseName();
    const QString suffix = info.suffix().isEmpty() ? QString()
                                                   : QLatin1Char('.') + info.suffix();
    const QString separator = QDir::separator();
    for (int i = 1; i < 1000; ++i) {
        const QString candidate = i == 1 ? stem + suffix
                                         : stem + QLatin1Char('-') + QString::number(i) + suffix;
        const QString path = dir + separator + candidate;
        if (!QFile::exists(path) && !QFile::exists(path + QStringLiteral(".pub"))) {
            return path;
        }
    }
    return dir + separator + stem + QLatin1Char('-')
           + QString::number(QDateTime::currentMSecsSinceEpoch());
}

} // namespace

QVector<KeyStore::KeyInfo> KeyStore::listKeys()
{
    QVector<KeyInfo> result;
#ifdef HSSH_HAS_LIBSSH
    QDir dir(keysDir());
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::Readable | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.suffix() == QLatin1String("pub")) {
            continue;
        }
        KeyInfo info;
        info.fileName = entry.fileName();
        info.filePath = entry.absoluteFilePath();
        ssh_key key = importPrivateKey(info.filePath, QString());
        if (key) {
            const char *type = ssh_key_type_to_char(ssh_key_type(key));
            if (type) {
                info.type = QString::fromUtf8(type);
            }
            info.fingerprintSha256 = fingerprintSha256(key);
            ssh_key_free(key);
        } else {
            // Passphrase-protected (or unreadable); still list it.
            info.encrypted = true;
        }
        result.append(info);
    }
#endif
    return result;
}

bool KeyStore::generateKey(KeyType type, const QString &name, const QString &passphrase,
                           QString *filePath, QString *errorMessage)
{
    const QString path = uniqueKeyPath(keysDir(), sanitizedKeyName(name));
    if (!generateKeyPair(type, path, passphrase, errorMessage)) {
        return false;
    }
    if (filePath) {
        *filePath = path;
    }
    // Best-effort <name>.pub in authorized_keys format.
    QString line;
    if (exportPublicKey(path, passphrase, &line)) {
        QFile pub(path + QStringLiteral(".pub"));
        if (pub.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            pub.write(line.toUtf8());
            if (!line.endsWith(QLatin1Char('\n'))) {
                pub.write("\n");
            }
            pub.close();
        }
    }
    return true;
}

bool KeyStore::importKeyFile(const QString &sourcePath, const QString &passphrase,
                             QString *filePath, QString *errorMessage)
{
    // Verify the file imports with the given passphrase before copying it.
    ssh_key key = importPrivateKey(sourcePath, passphrase, errorMessage);
    if (!key) {
        return false;
    }
    ssh_key_free(key);

    const QString dest = uniqueKeyPath(keysDir(), QFileInfo(sourcePath).fileName());
    if (!QFile::copy(sourcePath, dest)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to copy the key into the key store.");
        }
        return false;
    }
    QFile::setPermissions(dest, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const QString sourcePub = sourcePath + QStringLiteral(".pub");
    if (QFile::exists(sourcePub) && !QFile::exists(dest + QStringLiteral(".pub"))) {
        QFile::copy(sourcePub, dest + QStringLiteral(".pub"));
    }
    if (filePath) {
        *filePath = dest;
    }
    return true;
}

bool KeyStore::exportPublicKey(const QString &filePath, const QString &passphrase,
                               QString *authorizedLine, QString *errorMessage)
{
    ssh_key key = importPrivateKey(filePath, passphrase, errorMessage);
    if (!key) {
        return false;
    }
    const QString line = publicKeyAuthorized(key, errorMessage);
    ssh_key_free(key);
    if (authorizedLine) {
        *authorizedLine = line;
    }
    return !line.isEmpty();
}

bool KeyStore::deleteKey(const QString &filePath, QString *errorMessage)
{
    const QString dir = keysDir();
    if (!filePath.startsWith(dir + QDir::separator())) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Refusing to delete a file outside the key store.");
        }
        return false;
    }
    bool ok = !QFile::exists(filePath) || QFile::remove(filePath);
    const QString pub = filePath + QStringLiteral(".pub");
    if (QFile::exists(pub)) {
        ok = QFile::remove(pub) && ok;
    }
    if (!ok && errorMessage) {
        *errorMessage = QStringLiteral("Failed to delete the key file.");
    }
    return ok;
}

bool KeyStore::verifyAndStoreHostKey(ssh_session session, const HostKeyVerifier &verifier,
                                     QString *errorMessage)
{
#ifdef HSSH_HAS_LIBSSH
    const KnownHostStatus status = checkKnownHost(session);
    if (status == KnownHostStatus::KnownOk) {
        return true;
    }
    if (status == KnownHostStatus::Other) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not read the known_hosts file.");
        }
        return false;
    }

    // Collect what the server presented for the prompt / error message.
    HostKeyInfo info;
    {
        char *host = nullptr;
        unsigned int port = 0;
        ssh_options_get(session, SSH_OPTIONS_HOST, &host);
        ssh_options_get_port(session, &port);
        if (host) {
            info.host = QString::fromUtf8(host);
            ssh_string_free_char(host);
        }
        if (port > 0 && port != 22) {
            info.host += QStringLiteral(":%1").arg(port);
        }
        ssh_key key = nullptr;
        if (ssh_get_server_publickey(session, &key) == SSH_OK && key) {
            const char *type = ssh_key_type_to_char(ssh_key_type(key));
            if (type) {
                info.keyType = QString::fromUtf8(type);
            }
            info.fingerprintSha256 = fingerprintSha256(key);
            info.fingerprintMd5 = fingerprintMd5(key);
            ssh_key_free(key);
        }
    }

    const bool changed = (status == KnownHostStatus::Changed);
    HostKeyDecision decision = changed ? HostKeyDecision::Reject : HostKeyDecision::Accept;
    if (verifier) {
        decision = verifier(info, changed);
    }
    if (decision == HostKeyDecision::Reject) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Host key for %1 rejected%2 (%3)")
                                 .arg(info.host,
                                      changed ? QStringLiteral(" — it CHANGED, possible "
                                                                "man-in-the-middle attack")
                                              : QString(),
                                      info.fingerprintSha256);
        }
        return false;
    }
    if (changed) {
        // Drop the stale entry under both known_hosts spellings, then re-add.
        const int colon = info.host.lastIndexOf(QLatin1Char(':'));
        const QString bareHost = colon > 0 ? info.host.left(colon) : info.host;
        const QString bracketed = colon > 0
                                      ? QStringLiteral("[%1]:%2").arg(bareHost).arg(info.host.mid(colon + 1))
                                      : QString();
        removeKnownHost(info.host);
        removeKnownHost(bareHost);
        if (!bracketed.isEmpty()) {
            removeKnownHost(bracketed);
        }
    }
    if (!writeKnownHost(session, errorMessage)) {
        return false;
    }
    return true;
#else
    Q_UNUSED(session)
    Q_UNUSED(verifier)
    if (errorMessage) {
        *errorMessage = QStringLiteral("libssh backend not available.");
    }
    return false;
#endif
}

QVector<KeyStore::KnownHostEntry> KeyStore::listKnownHosts()
{
    QVector<KnownHostEntry> result;
#ifdef HSSH_HAS_LIBSSH
    QFile file(knownHostsPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return result;
    }
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) {
            continue;
        }
        const QList<QByteArray> parts = line.split(' ');
        if (parts.size() < 3) {
            continue;
        }
        KnownHostEntry entry;
        entry.hosts = QString::fromUtf8(parts.at(0));
        entry.keyType = QString::fromUtf8(parts.at(1));
        ssh_key key = nullptr;
        // ssh_key_type_from_char is not in this libssh build; map the
        // well-known spellings by hand.
        const QByteArray type = parts.at(1);
        ssh_keytypes_e keyType = SSH_KEYTYPE_UNKNOWN;
        if (type == "ssh-ed25519") {
            keyType = SSH_KEYTYPE_ED25519;
        } else if (type == "ssh-rsa") {
            keyType = SSH_KEYTYPE_RSA;
        } else if (type == "ecdsa-sha2-nistp256") {
            keyType = SSH_KEYTYPE_ECDSA_P256;
        } else if (type == "ecdsa-sha2-nistp384") {
            keyType = SSH_KEYTYPE_ECDSA_P384;
        } else if (type == "ecdsa-sha2-nistp521") {
            keyType = SSH_KEYTYPE_ECDSA_P521;
        } else if (type == "ssh-dss") {
            keyType = SSH_KEYTYPE_DSS;
        }
        if (keyType != SSH_KEYTYPE_UNKNOWN
            && ssh_pki_import_pubkey_base64(parts.at(2).constData(), keyType, &key) == SSH_OK
            && key) {
            entry.fingerprintSha256 = fingerprintSha256(key);
            ssh_key_free(key);
        }
        result.append(entry);
    }
#endif
    return result;
}

} // namespace hssh

#include "SessionConfig.h"

namespace hssh {

SessionConfig::SessionConfig()
    : m_id(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
}

SessionConfig::SessionConfig(const QString &id)
    : m_id(id)
{
}

QString SessionConfig::id() const
{
    return m_id;
}

QString SessionConfig::name() const
{
    return m_name;
}

void SessionConfig::setName(const QString &name)
{
    m_name = name;
}

QString SessionConfig::group() const
{
    return m_group;
}

void SessionConfig::setGroup(const QString &group)
{
    m_group = group;
}

SessionType SessionConfig::sessionType() const
{
    return m_sessionType;
}

void SessionConfig::setSessionType(SessionType type)
{
    m_sessionType = type;
}

QString SessionConfig::shellType() const
{
    return m_shellType;
}

void SessionConfig::setShellType(const QString &type)
{
    m_shellType = type;
}

QString SessionConfig::host() const
{
    return m_host;
}

void SessionConfig::setHost(const QString &host)
{
    m_host = host;
}

int SessionConfig::port() const
{
    return m_port;
}

void SessionConfig::setPort(int port)
{
    m_port = (port > 0 && port <= 65535) ? port : 22;
}

QString SessionConfig::username() const
{
    return m_username;
}

void SessionConfig::setUsername(const QString &username)
{
    m_username = username;
}

AuthMethod SessionConfig::authMethod() const
{
    return m_authMethod;
}

void SessionConfig::setAuthMethod(AuthMethod method)
{
    m_authMethod = method;
}

SecureString SessionConfig::password() const
{
    return m_password;
}

void SessionConfig::setPassword(const SecureString &password)
{
    m_password = password;
}

QString SessionConfig::privateKeyPath() const
{
    return m_privateKeyPath;
}

void SessionConfig::setPrivateKeyPath(const QString &path)
{
    m_privateKeyPath = path;
}

SecureString SessionConfig::keyPassphrase() const
{
    return m_keyPassphrase;
}

void SessionConfig::setKeyPassphrase(const SecureString &passphrase)
{
    m_keyPassphrase = passphrase;
}

QStringList SessionConfig::postLoginCommands() const
{
    return m_postLoginCommands;
}

void SessionConfig::setPostLoginCommands(const QStringList &commands)
{
    m_postLoginCommands = commands;
}

QString SessionConfig::serialPort() const
{
    return m_serialPort;
}

void SessionConfig::setSerialPort(const QString &port)
{
    m_serialPort = port;
}

int SessionConfig::serialBaudRate() const
{
    return m_serialBaudRate;
}

void SessionConfig::setSerialBaudRate(int baud)
{
    m_serialBaudRate = baud > 0 ? baud : 115200;
}

int SessionConfig::keepAliveSeconds() const
{
    return m_keepAliveSeconds;
}

void SessionConfig::setKeepAliveSeconds(int seconds)
{
    m_keepAliveSeconds = qBound(0, seconds, 86400);
}

bool SessionConfig::autoReconnect() const
{
    return m_autoReconnect;
}

void SessionConfig::setAutoReconnect(bool enabled)
{
    m_autoReconnect = enabled;
}

bool SessionConfig::forwardAgent() const
{
    return m_forwardAgent;
}

void SessionConfig::setForwardAgent(bool enabled)
{
    m_forwardAgent = enabled;
}

QStringList SessionConfig::tags() const
{
    return m_tags;
}

void SessionConfig::setTags(const QStringList &tags)
{
    m_tags = tags;
}

bool SessionConfig::favorite() const
{
    return m_favorite;
}

void SessionConfig::setFavorite(bool favorite)
{
    m_favorite = favorite;
}

QString SessionConfig::proxyType() const
{
    return m_proxyType;
}

void SessionConfig::setProxyType(const QString &type)
{
    m_proxyType = (type == QLatin1String("none")) ? QString() : type;
}

QString SessionConfig::proxyHost() const
{
    return m_proxyHost;
}

void SessionConfig::setProxyHost(const QString &host)
{
    m_proxyHost = host;
}

int SessionConfig::proxyPort() const
{
    return m_proxyPort;
}

void SessionConfig::setProxyPort(int port)
{
    m_proxyPort = port;
}

QString SessionConfig::proxyUsername() const
{
    return m_proxyUsername;
}

void SessionConfig::setProxyUsername(const QString &username)
{
    m_proxyUsername = username;
}

SecureString SessionConfig::proxyPassword() const
{
    return m_proxyPassword;
}

void SessionConfig::setProxyPassword(const SecureString &password)
{
    m_proxyPassword = password;
}

QString SessionConfig::jumpHost() const
{
    return m_jumpHost;
}

void SessionConfig::setJumpHost(const QString &host)
{
    m_jumpHost = host;
}

int SessionConfig::jumpPort() const
{
    return m_jumpPort;
}

void SessionConfig::setJumpPort(int port)
{
    m_jumpPort = port;
}

QString SessionConfig::jumpUsername() const
{
    return m_jumpUsername;
}

void SessionConfig::setJumpUsername(const QString &username)
{
    m_jumpUsername = username;
}

SecureString SessionConfig::jumpPassword() const
{
    return m_jumpPassword;
}

void SessionConfig::setJumpPassword(const SecureString &password)
{
    m_jumpPassword = password;
}

QString SessionConfig::jumpPrivateKeyPath() const
{
    return m_jumpPrivateKeyPath;
}

void SessionConfig::setJumpPrivateKeyPath(const QString &path)
{
    m_jumpPrivateKeyPath = path;
}

bool SessionConfig::isValid() const
{
    if (m_sessionType == SessionType::Local) {
        return !m_shellType.isEmpty();
    }
    // Telnet/Serial/Raw are accepted structurally (host/port or device); the
    // transports arrive in Phase 3.
    if (m_sessionType == SessionType::Telnet || m_sessionType == SessionType::Raw) {
        return !m_host.isEmpty() && m_port > 0 && m_port <= 65535;
    }
    if (m_sessionType == SessionType::Serial) {
        return !m_serialPort.isEmpty();
    }
    return !m_host.isEmpty() && m_port > 0 && m_port <= 65535;
}

QString SessionConfig::displayName() const
{
    if (!m_name.isEmpty()) {
        return m_name;
    }
    if (m_sessionType == SessionType::Local) {
        return m_shellType.isEmpty() ? QStringLiteral("Local Terminal") : m_shellType;
    }
    if (m_sessionType == SessionType::Serial) {
        return m_serialPort.isEmpty() ? QStringLiteral("Serial") : QStringLiteral("Serial: %1").arg(m_serialPort);
    }
    if (m_sessionType == SessionType::Telnet) {
        return m_host.isEmpty() ? QStringLiteral("Telnet") : QStringLiteral("Telnet: %1:%2").arg(m_host).arg(m_port);
    }
    if (m_sessionType == SessionType::Raw) {
        return m_host.isEmpty() ? QStringLiteral("Raw TCP") : QStringLiteral("Raw: %1:%2").arg(m_host).arg(m_port);
    }
    if (!m_host.isEmpty()) {
        if (!m_username.isEmpty()) {
            return QStringLiteral("%1@%2").arg(m_username, m_host);
        }
        return m_host;
    }
    return QStringLiteral("New Session");
}

QVariantMap SessionConfig::toMap() const
{
    QVariantMap map;
    map[QStringLiteral("id")] = m_id;
    map[QStringLiteral("name")] = m_name;
    map[QStringLiteral("group")] = m_group;
    map[QStringLiteral("sessionType")] = static_cast<int>(m_sessionType);
    map[QStringLiteral("shellType")] = m_shellType;
    map[QStringLiteral("host")] = m_host;
    map[QStringLiteral("port")] = m_port;
    map[QStringLiteral("username")] = m_username;
    map[QStringLiteral("authMethod")] = static_cast<int>(m_authMethod);
    map[QStringLiteral("password")] = QString::fromUtf8(m_password.toByteArray().toBase64());
    map[QStringLiteral("privateKeyPath")] = m_privateKeyPath;
    map[QStringLiteral("keyPassphrase")] = QString::fromUtf8(m_keyPassphrase.toByteArray().toBase64());
    map[QStringLiteral("postLoginCommands")] = m_postLoginCommands;
    map[QStringLiteral("serialPort")] = m_serialPort;
    map[QStringLiteral("serialBaudRate")] = m_serialBaudRate;
    map[QStringLiteral("keepAliveSeconds")] = m_keepAliveSeconds;
    map[QStringLiteral("autoReconnect")] = m_autoReconnect;
    map[QStringLiteral("forwardAgent")] = m_forwardAgent;
    map[QStringLiteral("proxyType")] = m_proxyType;
    map[QStringLiteral("proxyHost")] = m_proxyHost;
    map[QStringLiteral("proxyPort")] = m_proxyPort;
    map[QStringLiteral("proxyUsername")] = m_proxyUsername;
    if (!m_proxyPassword.isEmpty()) {
        map[QStringLiteral("proxyPassword")] =
            QString::fromUtf8(m_proxyPassword.toByteArray().toBase64());
    }
    map[QStringLiteral("jumpHost")] = m_jumpHost;
    map[QStringLiteral("jumpPort")] = m_jumpPort;
    map[QStringLiteral("jumpUsername")] = m_jumpUsername;
    if (!m_jumpPassword.isEmpty()) {
        map[QStringLiteral("jumpPassword")] =
            QString::fromUtf8(m_jumpPassword.toByteArray().toBase64());
    }
    map[QStringLiteral("jumpPrivateKeyPath")] = m_jumpPrivateKeyPath;
    map[QStringLiteral("tags")] = m_tags;
    map[QStringLiteral("favorite")] = m_favorite;
    return map;
}

SessionConfig SessionConfig::fromMap(const QVariantMap &map)
{
    SessionConfig config(map.value(QStringLiteral("id")).toString());
    config.setName(map.value(QStringLiteral("name")).toString());
    config.setGroup(map.value(QStringLiteral("group")).toString());
    config.setSessionType(static_cast<SessionType>(map.value(QStringLiteral("sessionType"), 0).toInt()));
    config.setShellType(map.value(QStringLiteral("shellType")).toString());
    config.setHost(map.value(QStringLiteral("host")).toString());
    config.setPort(map.value(QStringLiteral("port"), 22).toInt());
    config.setUsername(map.value(QStringLiteral("username")).toString());
    config.setAuthMethod(static_cast<AuthMethod>(map.value(QStringLiteral("authMethod"), 0).toInt()));

    const QByteArray passwordBytes = QByteArray::fromBase64(map.value(QStringLiteral("password")).toByteArray());
    config.setPassword(SecureString(passwordBytes));

    config.setPrivateKeyPath(map.value(QStringLiteral("privateKeyPath")).toString());

    const QByteArray passphraseBytes = QByteArray::fromBase64(map.value(QStringLiteral("keyPassphrase")).toByteArray());
    config.setKeyPassphrase(SecureString(passphraseBytes));

    config.setPostLoginCommands(map.value(QStringLiteral("postLoginCommands")).toStringList());
    config.setSerialPort(map.value(QStringLiteral("serialPort")).toString());
    config.setSerialBaudRate(map.value(QStringLiteral("serialBaudRate"), 115200).toInt());
    config.setKeepAliveSeconds(map.value(QStringLiteral("keepAliveSeconds"), 30).toInt());
    config.setAutoReconnect(map.value(QStringLiteral("autoReconnect")).toBool());
    config.setForwardAgent(map.value(QStringLiteral("forwardAgent")).toBool());
    config.setProxyType(map.value(QStringLiteral("proxyType")).toString());
    config.setProxyHost(map.value(QStringLiteral("proxyHost")).toString());
    config.setProxyPort(map.value(QStringLiteral("proxyPort"), 0).toInt());
    config.setProxyUsername(map.value(QStringLiteral("proxyUsername")).toString());
    const QByteArray proxyPass = QByteArray::fromBase64(
        map.value(QStringLiteral("proxyPassword")).toByteArray());
    if (!proxyPass.isEmpty()) {
        config.setProxyPassword(SecureString(QString::fromUtf8(proxyPass)));
    }
    config.setJumpHost(map.value(QStringLiteral("jumpHost")).toString());
    config.setJumpPort(map.value(QStringLiteral("jumpPort"), 22).toInt());
    config.setJumpUsername(map.value(QStringLiteral("jumpUsername")).toString());
    const QByteArray jumpPass = QByteArray::fromBase64(
        map.value(QStringLiteral("jumpPassword")).toByteArray());
    if (!jumpPass.isEmpty()) {
        config.setJumpPassword(SecureString(QString::fromUtf8(jumpPass)));
    }
    config.setJumpPrivateKeyPath(map.value(QStringLiteral("jumpPrivateKeyPath")).toString());
    config.setTags(map.value(QStringLiteral("tags")).toStringList());
    config.setFavorite(map.value(QStringLiteral("favorite"), false).toBool());
    return config;
}

} // namespace hssh

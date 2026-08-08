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

bool SessionConfig::isValid() const
{
    if (m_sessionType == SessionType::Local) {
        return !m_shellType.isEmpty();
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
    map[QStringLiteral("keepAliveSeconds")] = m_keepAliveSeconds;
    map[QStringLiteral("autoReconnect")] = m_autoReconnect;
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
    config.setKeepAliveSeconds(map.value(QStringLiteral("keepAliveSeconds"), 30).toInt());
    config.setAutoReconnect(map.value(QStringLiteral("autoReconnect")).toBool());
    return config;
}

} // namespace hssh

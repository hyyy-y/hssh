#ifndef HSSH_CORE_SESSIONCONFIG_H
#define HSSH_CORE_SESSIONCONFIG_H

#include "utils/Crypto.h"

#include <QList>
#include <QMetaType>
#include <QString>
#include <QUuid>
#include <QVariantMap>

namespace hssh {

enum class SessionType {
    Ssh,
    Local
};

enum class AuthMethod {
    Password,
    PublicKey,
    KeyboardInteractive,
    Agent
};

class SessionConfig {
public:
    SessionConfig();
    explicit SessionConfig(const QString &id);

    [[nodiscard]] QString id() const;
    [[nodiscard]] QString name() const;
    void setName(const QString &name);

    [[nodiscard]] QString group() const;
    void setGroup(const QString &group);

    [[nodiscard]] SessionType sessionType() const;
    void setSessionType(SessionType type);

    [[nodiscard]] QString shellType() const;
    void setShellType(const QString &type);

    [[nodiscard]] QString host() const;
    void setHost(const QString &host);

    [[nodiscard]] int port() const;
    void setPort(int port);

    [[nodiscard]] QString username() const;
    void setUsername(const QString &username);

    [[nodiscard]] AuthMethod authMethod() const;
    void setAuthMethod(AuthMethod method);

    [[nodiscard]] SecureString password() const;
    void setPassword(const SecureString &password);

    [[nodiscard]] QString privateKeyPath() const;
    void setPrivateKeyPath(const QString &path);

    [[nodiscard]] SecureString keyPassphrase() const;
    void setKeyPassphrase(const SecureString &passphrase);

    [[nodiscard]] QStringList postLoginCommands() const;
    void setPostLoginCommands(const QStringList &commands);

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] QString displayName() const;

    [[nodiscard]] QVariantMap toMap() const;
    static SessionConfig fromMap(const QVariantMap &map);

private:
    QString m_id;
    QString m_name;
    QString m_group;
    SessionType m_sessionType = SessionType::Ssh;
    QString m_shellType;
    QString m_host;
    int m_port = 22;
    QString m_username;
    AuthMethod m_authMethod = AuthMethod::Password;
    SecureString m_password;
    QString m_privateKeyPath;
    SecureString m_keyPassphrase;
    QStringList m_postLoginCommands;
};

} // namespace hssh

Q_DECLARE_METATYPE(hssh::SessionConfig)

#endif // HSSH_CORE_SESSIONCONFIG_H

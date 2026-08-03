#include "SessionRepository.h"

#include "utils/Database.h"

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QUuid>

namespace hssh {

class SessionRepository::Impl {
public:
    Database database;
    QString lastError;
};

SessionRepository::SessionRepository(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
}

SessionRepository::~SessionRepository() = default;

bool SessionRepository::initialize()
{
    if (d->database.isOpen()) {
        return true;
    }

    if (!d->database.open()) {
        d->lastError = d->database.lastError();
        return false;
    }

    return true;
}

bool SessionRepository::isInitialized() const
{
    return d->database.isOpen();
}

QString SessionRepository::lastError() const
{
    return d->lastError;
}

QString SessionRepository::addFolder(const QString &name, const QString &parentId)
{
    if (!isInitialized() && !initialize()) {
        return {};
    }

    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral(
        "INSERT INTO folders (id, parent_id, name) VALUES (:id, :parent_id, :name)"));
    query.bindValue(QStringLiteral(":id"), id);
    query.bindValue(QStringLiteral(":parent_id"), parentId.isEmpty() ? QVariant() : QVariant(parentId));
    query.bindValue(QStringLiteral(":name"), name);

    if (!query.exec()) {
        d->lastError = query.lastError().text();
        return {};
    }

    emit sessionsChanged();
    return id;
}

bool SessionRepository::updateFolder(const QString &id, const QString &name)
{
    if (!isInitialized() && !initialize()) {
        return false;
    }

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral("UPDATE folders SET name = :name WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);
    query.bindValue(QStringLiteral(":name"), name);

    if (!query.exec()) {
        d->lastError = query.lastError().text();
        return false;
    }

    emit sessionsChanged();
    return true;
}

bool SessionRepository::removeFolder(const QString &id)
{
    if (!isInitialized() && !initialize()) {
        return false;
    }

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral("DELETE FROM folders WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);

    if (!query.exec()) {
        d->lastError = query.lastError().text();
        return false;
    }

    emit sessionsChanged();
    return true;
}

QMap<QString, QString> SessionRepository::folderPaths() const
{
    QMap<QString, QString> result;
    if (!isInitialized()) {
        return result;
    }

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral("SELECT id, parent_id FROM folders"));
    if (!query.exec()) {
        return result;
    }

    while (query.next()) {
        const QString id = query.value(0).toString();
        const QString parentId = query.value(1).toString();
        result[id] = parentId;
    }

    return result;
}

QMap<QString, QString> SessionRepository::folderNames() const
{
    QMap<QString, QString> result;
    if (!isInitialized()) {
        return result;
    }

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral("SELECT id, name FROM folders"));
    if (!query.exec()) {
        return result;
    }

    while (query.next()) {
        const QString id = query.value(0).toString();
        const QString name = query.value(1).toString();
        result[id] = name;
    }

    return result;
}

bool SessionRepository::saveSession(const SessionConfig &config)
{
    if (!isInitialized() && !initialize()) {
        return false;
    }

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO sessions ("
        "  id, folder_id, name, host, port, username, auth_method,"
        "  password_encrypted, private_key_path, key_passphrase_encrypted, post_login_commands"
        ") VALUES ("
        "  :id, :folder_id, :name, :host, :port, :username, :auth_method,"
        "  :password_encrypted, :private_key_path, :key_passphrase_encrypted, :post_login_commands"
        ")"));

    query.bindValue(QStringLiteral(":id"), config.id());
    query.bindValue(QStringLiteral(":folder_id"), config.group().isEmpty() ? QVariant() : QVariant(config.group()));
    query.bindValue(QStringLiteral(":name"), config.name());
    query.bindValue(QStringLiteral(":host"), config.host());
    query.bindValue(QStringLiteral(":port"), config.port());
    query.bindValue(QStringLiteral(":username"), config.username());
    query.bindValue(QStringLiteral(":auth_method"), static_cast<int>(config.authMethod()));
    query.bindValue(QStringLiteral(":password_encrypted"), config.password().toByteArray());
    query.bindValue(QStringLiteral(":private_key_path"), config.privateKeyPath());
    query.bindValue(QStringLiteral(":key_passphrase_encrypted"), config.keyPassphrase().toByteArray());
    query.bindValue(QStringLiteral(":post_login_commands"), config.postLoginCommands().join('\n'));

    if (!query.exec()) {
        d->lastError = query.lastError().text();
        return false;
    }

    emit sessionsChanged();
    return true;
}

bool SessionRepository::removeSession(const QString &id)
{
    if (!isInitialized() && !initialize()) {
        return false;
    }

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral("DELETE FROM sessions WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);

    if (!query.exec()) {
        d->lastError = query.lastError().text();
        return false;
    }

    emit sessionsChanged();
    return true;
}

SessionConfig SessionRepository::loadSession(const QString &id) const
{
    if (!isInitialized()) {
        return {};
    }

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral("SELECT * FROM sessions WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);

    if (!query.exec()) {
        return SessionConfig(QString());
    }

    if (!query.next()) {
        return SessionConfig(QString());
    }

    SessionConfig config(query.value(QStringLiteral("id")).toString());
    config.setName(query.value(QStringLiteral("name")).toString());
    config.setGroup(query.value(QStringLiteral("folder_id")).toString());
    config.setHost(query.value(QStringLiteral("host")).toString());
    config.setPort(query.value(QStringLiteral("port")).toInt());
    config.setUsername(query.value(QStringLiteral("username")).toString());
    config.setAuthMethod(static_cast<AuthMethod>(query.value(QStringLiteral("auth_method")).toInt()));
    config.setPassword(SecureString(query.value(QStringLiteral("password_encrypted")).toByteArray()));
    config.setPrivateKeyPath(query.value(QStringLiteral("private_key_path")).toString());
    config.setKeyPassphrase(SecureString(query.value(QStringLiteral("key_passphrase_encrypted")).toByteArray()));
    config.setPostLoginCommands(query.value(QStringLiteral("post_login_commands")).toString().split('\n', Qt::SkipEmptyParts));

    return config;
}

QList<SessionConfig> SessionRepository::loadAllSessions() const
{
    QList<SessionConfig> result;
    if (!isInitialized()) {
        return result;
    }

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral("SELECT id FROM sessions"));
    if (!query.exec()) {
        return result;
    }

    while (query.next()) {
        const QString id = query.value(0).toString();
        result.append(loadSession(id));
    }

    return result;
}

SessionRepository::TreeData SessionRepository::loadTree() const
{
    TreeData data;
    data.sessions = loadAllSessions();
    data.folderPaths = folderPaths();
    data.folderNames = folderNames();
    return data;
}

QList<SessionRepository::CompareProject> SessionRepository::compareProjects(const QString &sessionId) const
{
    QList<CompareProject> result;
    if (!isInitialized()) {
        return result;
    }

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral(
        "SELECT id, session_id, name, local_path, remote_path, last_used "
        "FROM compare_projects WHERE session_id = :session_id ORDER BY last_used DESC"));
    query.bindValue(QStringLiteral(":session_id"), sessionId);
    if (!query.exec()) {
        return result;
    }

    while (query.next()) {
        CompareProject project;
        project.id = query.value(0).toString();
        project.sessionId = query.value(1).toString();
        project.name = query.value(2).toString();
        project.localPath = query.value(3).toString();
        project.remotePath = query.value(4).toString();
        project.lastUsed = query.value(5).toLongLong();
        result.append(project);
    }
    return result;
}

QString SessionRepository::saveCompareProject(const QString &sessionId, const QString &name,
                                              const QString &localPath, const QString &remotePath)
{
    if (!isInitialized() && !initialize()) {
        return {};
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery query(d->database.db());

    // Same (session, name) -> update in place, keeping the original id.
    query.prepare(QStringLiteral(
        "SELECT id FROM compare_projects WHERE session_id = :session_id AND name = :name"));
    query.bindValue(QStringLiteral(":session_id"), sessionId);
    query.bindValue(QStringLiteral(":name"), name);
    QString id;
    if (query.exec() && query.next()) {
        id = query.value(0).toString();
    }
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }

    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO compare_projects (id, session_id, name, local_path, remote_path, last_used) "
        "VALUES (:id, :session_id, :name, :local_path, :remote_path, :last_used)"));
    query.bindValue(QStringLiteral(":id"), id);
    query.bindValue(QStringLiteral(":session_id"), sessionId);
    query.bindValue(QStringLiteral(":name"), name);
    query.bindValue(QStringLiteral(":local_path"), localPath);
    query.bindValue(QStringLiteral(":remote_path"), remotePath);
    query.bindValue(QStringLiteral(":last_used"), now);
    if (!query.exec()) {
        d->lastError = query.lastError().text();
        return {};
    }
    return id;
}

bool SessionRepository::removeCompareProject(const QString &id)
{
    if (!isInitialized() && !initialize()) {
        return false;
    }

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral("DELETE FROM compare_projects WHERE id = :id"));
    query.bindValue(QStringLiteral(":id"), id);
    if (!query.exec()) {
        d->lastError = query.lastError().text();
        return false;
    }
    return true;
}

void SessionRepository::touchCompareProject(const QString &id)
{
    if (!isInitialized() && !initialize()) {
        return;
    }

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral("UPDATE compare_projects SET last_used = :now WHERE id = :id"));
    query.bindValue(QStringLiteral(":now"), QDateTime::currentSecsSinceEpoch());
    query.bindValue(QStringLiteral(":id"), id);
    query.exec();
}

} // namespace hssh

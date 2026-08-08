#include "SessionRepository.h"

#include "utils/Database.h"

#include "utils/Crypto.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QUuid>

namespace hssh {

class SessionRepository::Impl {
public:
    explicit Impl(const QString &connectionName)
        : database(connectionName)
    {
    }

    Database database;
    QString lastError;
};

SessionRepository::SessionRepository(QObject *parent, const QString &connectionName)
    : QObject(parent)
    , d(std::make_unique<Impl>(connectionName))
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

    // Transparent migration: secrets written by pre-encryption builds are
    // stored as plaintext; re-encrypt them in place with the active key.
    migrateSecrets();
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

    // Collect the folder and all descendants so their sessions are removed
    // too (SQLite does not cascade).
    QSet<QString> ids{id};
    bool changed = true;
    while (changed) {
        changed = false;
        QSqlQuery query(d->database.db());
        query.prepare(QStringLiteral("SELECT id FROM folders WHERE parent_id = :id"));
        for (const QString &folderId : ids) {
            query.bindValue(QStringLiteral(":id"), folderId);
            if (query.exec()) {
                while (query.next()) {
                    const QString childId = query.value(0).toString();
                    if (!ids.contains(childId)) {
                        ids.insert(childId);
                        changed = true;
                    }
                }
            }
        }
    }

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral("DELETE FROM sessions WHERE folder_id = :id"));
    for (const QString &folderId : ids) {
        query.bindValue(QStringLiteral(":id"), folderId);
        if (!query.exec()) {
            d->lastError = query.lastError().text();
            return false;
        }
    }

    query.prepare(QStringLiteral("DELETE FROM folders WHERE id = :id"));
    for (const QString &folderId : ids) {
        query.bindValue(QStringLiteral(":id"), folderId);
        if (!query.exec()) {
            d->lastError = query.lastError().text();
            return false;
        }
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
        "  password_encrypted, private_key_path, key_passphrase_encrypted, post_login_commands,"
        "  keep_alive_seconds, auto_reconnect"
        ") VALUES ("
        "  :id, :folder_id, :name, :host, :port, :username, :auth_method,"
        "  :password_encrypted, :private_key_path, :key_passphrase_encrypted, :post_login_commands,"
        "  :keep_alive_seconds, :auto_reconnect"
        ")"));

    query.bindValue(QStringLiteral(":id"), config.id());
    query.bindValue(QStringLiteral(":folder_id"), config.group().isEmpty() ? QVariant() : QVariant(config.group()));
    query.bindValue(QStringLiteral(":name"), config.name());
    query.bindValue(QStringLiteral(":host"), config.host());
    query.bindValue(QStringLiteral(":port"), config.port());
    query.bindValue(QStringLiteral(":username"), config.username());
    query.bindValue(QStringLiteral(":auth_method"), static_cast<int>(config.authMethod()));
    query.bindValue(QStringLiteral(":password_encrypted"), Crypto::encryptData(config.password().toByteArray()));
    query.bindValue(QStringLiteral(":private_key_path"), config.privateKeyPath());
    query.bindValue(QStringLiteral(":key_passphrase_encrypted"), Crypto::encryptData(config.keyPassphrase().toByteArray()));
    query.bindValue(QStringLiteral(":post_login_commands"), config.postLoginCommands().join('\n'));
    query.bindValue(QStringLiteral(":keep_alive_seconds"), config.keepAliveSeconds());
    query.bindValue(QStringLiteral(":auto_reconnect"), config.autoReconnect() ? 1 : 0);

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

    bool decryptOk = true;
    const QByteArray passwordPlain = Crypto::decryptData(
        query.value(QStringLiteral("password_encrypted")).toByteArray(), &decryptOk);
    if (!decryptOk) {
        d->lastError = tr("Failed to decrypt the stored password (locked or wrong key)");
        return SessionConfig(QString());
    }
    config.setPassword(SecureString(passwordPlain));

    config.setPrivateKeyPath(query.value(QStringLiteral("private_key_path")).toString());

    const QByteArray passphrasePlain = Crypto::decryptData(
        query.value(QStringLiteral("key_passphrase_encrypted")).toByteArray(), &decryptOk);
    if (!decryptOk) {
        d->lastError = tr("Failed to decrypt the stored key passphrase (locked or wrong key)");
        return SessionConfig(QString());
    }
    config.setKeyPassphrase(SecureString(passphrasePlain));
    config.setPostLoginCommands(query.value(QStringLiteral("post_login_commands")).toString().split('\n', Qt::SkipEmptyParts));
    config.setKeepAliveSeconds(query.value(QStringLiteral("keep_alive_seconds")).toInt());
    config.setAutoReconnect(query.value(QStringLiteral("auto_reconnect")).toBool());

    return config;
}

void SessionRepository::migrateSecrets()
{
    if (!Crypto::isAvailable() || !Crypto::isUnlocked()) {
        return;
    }

    const QByteArray magic("HSE1");

    // Repairs double encryption (an earlier migration re-encrypted blobs
    // whose sibling field was empty): unwrap layers until the plaintext is
    // no longer a blob itself.
    const auto unwrap = [&magic](QByteArray value) {
        while (value.startsWith(magic) && value.size() >= 36) {
            bool ok = false;
            const QByteArray inner = Crypto::decryptData(value, &ok);
            if (!ok || !inner.startsWith(magic)) {
                break;
            }
            value = inner;
        }
        return value;
    };

    QSqlQuery query(d->database.db());
    if (!query.exec(QStringLiteral(
            "SELECT id, password_encrypted, key_passphrase_encrypted FROM sessions"))) {
        return;
    }

    QList<QVariantList> updates;
    while (query.next()) {
        const QByteArray password = unwrap(query.value(1).toByteArray());
        const QByteArray passphrase = unwrap(query.value(2).toByteArray());
        const bool changed = password != query.value(1).toByteArray()
            || passphrase != query.value(2).toByteArray();
        const bool pwDone = password.isEmpty() || password.startsWith(magic);
        const bool ppDone = passphrase.isEmpty() || passphrase.startsWith(magic);
        if (!changed && pwDone && ppDone) {
            continue;
        }
        updates.append({query.value(0),
                        pwDone ? password : Crypto::encryptData(password),
                        ppDone ? passphrase : Crypto::encryptData(passphrase)});
    }

    for (const QVariantList &row : updates) {
        QSqlQuery update(d->database.db());
        update.prepare(QStringLiteral(
            "UPDATE sessions SET password_encrypted = :pw, key_passphrase_encrypted = :pp "
            "WHERE id = :id"));
        update.bindValue(QStringLiteral(":pw"), row.at(1));
        update.bindValue(QStringLiteral(":pp"), row.at(2));
        update.bindValue(QStringLiteral(":id"), row.at(0));
        update.exec();
    }
}

bool SessionRepository::reEncryptAllSecrets()
{
    if (!isInitialized() && !initialize()) {
        return false;
    }
    if (!Crypto::isAvailable()) {
        return true; // Nothing to do when crypto is compiled out.
    }

    const QList<SessionConfig> sessions = loadAllSessions();
    // loadSession returns an empty-id config when decryption fails (e.g.
    // locked); refuse to continue rather than destroying the secrets.
    for (const SessionConfig &config : sessions) {
        if (config.id().isEmpty()) {
            d->lastError = tr("Secrets are locked; unlock before rotating the key");
            return false;
        }
        if (!saveSession(config)) {
            return false;
        }
    }
    return true;
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

bool SessionRepository::recordSessionUse(const QString &sessionId)
{
    if (!isInitialized() && !initialize()) {
        return false;
    }

    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO session_history (session_id, last_used) "
        "VALUES (:session_id, :last_used)"));
    query.bindValue(QStringLiteral(":session_id"), sessionId);
    query.bindValue(QStringLiteral(":last_used"), QDateTime::currentSecsSinceEpoch());
    if (!query.exec()) {
        d->lastError = query.lastError().text();
        return false;
    }
    return true;
}

QList<SessionConfig> SessionRepository::recentSessions(int limit) const
{
    QList<SessionConfig> result;
    if (!isInitialized()) {
        return result;
    }

    // Sessions deleted since the history entry was written are skipped.
    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral(
        "SELECT session_id FROM session_history h "
        "WHERE EXISTS (SELECT 1 FROM sessions s WHERE s.id = h.session_id) "
        "ORDER BY h.last_used DESC LIMIT :limit"));
    query.bindValue(QStringLiteral(":limit"), limit);
    if (!query.exec()) {
        return result;
    }

    while (query.next()) {
        result.append(loadSession(query.value(0).toString()));
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

bool SessionRepository::exportSessionsToJson(const QString &filePath) const
{
    if (!isInitialized()) {
        d->lastError = tr("Database is not initialized");
        return false;
    }

    QJsonObject root;
    root[QStringLiteral("format")] = QStringLiteral("hssh-sessions");
    root[QStringLiteral("version")] = 1;

    QJsonArray foldersArray;
    QSqlQuery query(d->database.db());
    query.prepare(QStringLiteral("SELECT id, parent_id, name FROM folders ORDER BY sort_index, id"));
    if (!query.exec()) {
        d->lastError = query.lastError().text();
        return false;
    }
    while (query.next()) {
        QJsonObject folder;
        folder[QStringLiteral("id")] = query.value(0).toString();
        folder[QStringLiteral("parentId")] = query.value(1).toString();
        folder[QStringLiteral("name")] = query.value(2).toString();
        foldersArray.append(folder);
    }
    root[QStringLiteral("folders")] = foldersArray;

    QJsonArray sessionsArray;
    const QList<SessionConfig> sessions = loadAllSessions();
    for (const SessionConfig &config : sessions) {
        sessionsArray.append(QJsonObject::fromVariantMap(config.toMap()));
    }
    root[QStringLiteral("sessions")] = sessionsArray;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        d->lastError = file.errorString();
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

bool SessionRepository::importSessionsFromJson(const QString &filePath)
{
    if (!isInitialized() && !initialize()) {
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        d->lastError = file.errorString();
        return false;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        d->lastError = tr("Invalid JSON file");
        return false;
    }

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("hssh-sessions")) {
        d->lastError = tr("Not an hssh session export file");
        return false;
    }

    if (!d->database.db().transaction()) {
        d->lastError = d->database.db().lastError().text();
        return false;
    }

    // Track which folder ids already exist so sessions keep their group
    // even when a folder was skipped.
    const QList<QString> existingFolderIds = folderPaths().keys();
    QSet<QString> existingFolders(existingFolderIds.begin(), existingFolderIds.end());
    QSet<QString> existingSessions;
    {
        QSqlQuery query(d->database.db());
        if (query.exec(QStringLiteral("SELECT id FROM sessions"))) {
            while (query.next()) {
                existingSessions.insert(query.value(0).toString());
            }
        }
    }

    const QJsonArray foldersArray = root.value(QStringLiteral("folders")).toArray();
    for (const QJsonValue &value : foldersArray) {
        const QJsonObject folder = value.toObject();
        const QString id = folder.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || existingFolders.contains(id)) {
            continue;
        }
        QSqlQuery query(d->database.db());
        query.prepare(QStringLiteral("INSERT INTO folders (id, parent_id, name) VALUES (:id, :parent_id, :name)"));
        query.bindValue(QStringLiteral(":id"), id);
        query.bindValue(QStringLiteral(":parent_id"), folder.value(QStringLiteral("parentId")).toString());
        query.bindValue(QStringLiteral(":name"), folder.value(QStringLiteral("name")).toString());
        if (!query.exec()) {
            d->lastError = query.lastError().text();
            d->database.db().rollback();
            return false;
        }
        existingFolders.insert(id);
    }

    const QJsonArray sessionsArray = root.value(QStringLiteral("sessions")).toArray();
    for (const QJsonValue &value : sessionsArray) {
        const QJsonObject session = value.toObject();
        const QString id = session.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || existingSessions.contains(id)) {
            continue;
        }
        const SessionConfig config = SessionConfig::fromMap(session.toVariantMap());
        if (!config.isValid()) {
            continue;
        }
        if (!saveSession(config)) {
            d->database.db().rollback();
            return false;
        }
        existingSessions.insert(id);
    }

    if (!d->database.db().commit()) {
        d->lastError = d->database.db().lastError().text();
        return false;
    }

    emit sessionsChanged();
    return true;
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

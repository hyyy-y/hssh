#ifndef HSSH_CORE_SESSIONREPOSITORY_H
#define HSSH_CORE_SESSIONREPOSITORY_H

#include "SessionConfig.h"

#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <memory>

namespace hssh {

class SessionRepository : public QObject {
    Q_OBJECT

public:
    // connectionName: distinct SQLite connection name so that a second
    // repository (e.g. the agent's) does not steal the default connection.
    explicit SessionRepository(QObject *parent = nullptr,
                               const QString &connectionName = QStringLiteral("hssh_default"));
    ~SessionRepository() override;

    bool initialize();
    [[nodiscard]] bool isInitialized() const;
    [[nodiscard]] QString lastError() const;

    // Folder operations
    [[nodiscard]] QString addFolder(const QString &name, const QString &parentId = QString());
    bool updateFolder(const QString &id, const QString &name);
    bool removeFolder(const QString &id);
    [[nodiscard]] QMap<QString, QString> folderPaths() const; // id -> parent_id
    [[nodiscard]] QMap<QString, QString> folderNames() const; // id -> name

    // Session operations
    bool saveSession(const SessionConfig &config);
    bool removeSession(const QString &id);
    [[nodiscard]] SessionConfig loadSession(const QString &id) const;
    [[nodiscard]] QList<SessionConfig> loadAllSessions() const;

    // Decrypts every stored secret with the current key and re-encrypts
    // them with the (possibly new) active key. Used when enabling/disabling
    // or changing the master password. The repository must be unlocked.
    bool reEncryptAllSecrets();

    // Connection history (File > Recent Sessions).
    // Marks a session as recently used; returns false on db error.
    bool recordSessionUse(const QString &sessionId);
    // Most recently used sessions (still existing in the database), newest
    // first.
    [[nodiscard]] QList<SessionConfig> recentSessions(int limit = 10) const;

    // Convenience: load the whole tree
    struct TreeData {
        QList<SessionConfig> sessions;
        QMap<QString, QString> folderPaths; // id -> parent_id
        QMap<QString, QString> folderNames; // id -> name
    };
    [[nodiscard]] TreeData loadTree() const;

    // Session import/export (JSON, includes folders). Import skips items
    // whose id already exists, so a file can be imported repeatedly.
    bool exportSessionsToJson(const QString &filePath) const;
    bool importSessionsFromJson(const QString &filePath);

    // Compare projects: saved local/remote folder pairs for the compare tab,
    // rooted at the remote machine (session id).
    struct CompareProject {
        QString id;
        QString sessionId;
        QString name;
        QString localPath;
        QString remotePath;
        qint64 lastUsed = 0; // seconds since epoch
    };
    // Newest first (by last_used).
    [[nodiscard]] QList<CompareProject> compareProjects(const QString &sessionId) const;
    // Upsert by (session_id, name); returns the project id (empty on error).
    QString saveCompareProject(const QString &sessionId, const QString &name,
                               const QString &localPath, const QString &remotePath);
    bool removeCompareProject(const QString &id);
    void touchCompareProject(const QString &id);

signals:
    void sessionsChanged();

private:
    // Re-encrypts legacy plaintext secrets with the active key.
    void migrateSecrets();

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace hssh

#endif // HSSH_CORE_SESSIONREPOSITORY_H

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
    explicit SessionRepository(QObject *parent = nullptr);
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

    // Convenience: load the whole tree
    struct TreeData {
        QList<SessionConfig> sessions;
        QMap<QString, QString> folderPaths; // id -> parent_id
        QMap<QString, QString> folderNames; // id -> name
    };
    [[nodiscard]] TreeData loadTree() const;

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
    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace hssh

#endif // HSSH_CORE_SESSIONREPOSITORY_H

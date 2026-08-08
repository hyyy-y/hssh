#ifndef HSSH_UTILS_DATABASE_H
#define HSSH_UTILS_DATABASE_H

#include <QSqlDatabase>
#include <QSqlError>
#include <QString>
#include <memory>

namespace hssh {

class Database {
public:
    explicit Database(const QString &connectionName = QStringLiteral("hssh_default"));
    ~Database();

    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool open();
    void close();
    bool isOpen() const;

    bool runMigrations();

    QSqlDatabase db() const;
    QString lastError() const;

    static QString defaultDatabasePath();
    static void setDefaultDatabasePath(const QString &path);

private:
    bool createSchema();
    bool migrateSchema();

    QString m_connectionName;
    QSqlDatabase m_db;
    bool m_open = false;
    QString m_lastError;
    static QString s_defaultDatabasePath;
};

} // namespace hssh

#endif // HSSH_UTILS_DATABASE_H

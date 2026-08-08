#include "Database.h"

#include <QCoreApplication>
#include <QDir>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>

namespace hssh {

QString Database::s_defaultDatabasePath;

Database::Database(const QString &connectionName)
    : m_connectionName(connectionName)
{
}

Database::~Database()
{
    close();
}

bool Database::open()
{
    if (m_open) {
        return true;
    }

    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    m_db.setDatabaseName(defaultDatabasePath());

    if (!m_db.open()) {
        m_lastError = m_db.lastError().text();
        return false;
    }

    m_open = true;
    return runMigrations();
}

void Database::close()
{
    if (!m_open) {
        return;
    }

    if (m_db.isOpen()) {
        m_db.close();
    }
    m_db = QSqlDatabase();
    QSqlDatabase::removeDatabase(m_connectionName);
    m_open = false;
}

bool Database::isOpen() const
{
    return m_open;
}

QSqlDatabase Database::db() const
{
    return m_db;
}

QString Database::lastError() const
{
    return m_lastError;
}

QString Database::defaultDatabasePath()
{
    if (!s_defaultDatabasePath.isEmpty()) {
        return s_defaultDatabasePath;
    }
    const QString dataLocation = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataLocation);
    return dataLocation + QDir::separator() + QStringLiteral("hssh.db");
}

void Database::setDefaultDatabasePath(const QString &path)
{
    s_defaultDatabasePath = path;
}

bool Database::runMigrations()
{
    QSqlQuery query(db());

    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS schema_version ("
            "    version INTEGER PRIMARY KEY"
            ")"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    if (!query.exec(QStringLiteral(
            "INSERT OR IGNORE INTO schema_version (version) VALUES (0)"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    if (!createSchema()) {
        return false;
    }

    return migrateSchema();
}

// Adds columns that older databases lack. New databases get them from
// createSchema(); existing ones need ALTER TABLE.
bool Database::migrateSchema()
{
    const QList<QStringList> migrations = {
        {QStringLiteral("sessions"), QStringLiteral("keep_alive_seconds"),
         QStringLiteral("ALTER TABLE sessions ADD COLUMN keep_alive_seconds INTEGER DEFAULT 30")},
        {QStringLiteral("sessions"), QStringLiteral("auto_reconnect"),
         QStringLiteral("ALTER TABLE sessions ADD COLUMN auto_reconnect INTEGER DEFAULT 0")},
    };

    for (const QStringList &migration : migrations) {
        QSqlQuery query(db());
        query.prepare(QStringLiteral("PRAGMA table_info(%1)").arg(migration.at(0)));
        if (!query.exec()) {
            m_lastError = query.lastError().text();
            return false;
        }

        bool hasColumn = false;
        while (query.next()) {
            if (query.value(1).toString() == migration.at(1)) {
                hasColumn = true;
                break;
            }
        }

        if (hasColumn) {
            continue;
        }

        if (!query.exec(migration.at(2))) {
            m_lastError = query.lastError().text();
            return false;
        }
    }

    return true;
}

bool Database::createSchema()
{
    QSqlQuery query(db());

    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS folders ("
            "    id TEXT PRIMARY KEY,"
            "    parent_id TEXT,"
            "    name TEXT NOT NULL,"
            "    sort_index INTEGER DEFAULT 0"
            ")"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS sessions ("
            "    id TEXT PRIMARY KEY,"
            "    folder_id TEXT,"
            "    name TEXT NOT NULL,"
            "    host TEXT NOT NULL,"
            "    port INTEGER DEFAULT 22,"
            "    username TEXT,"
            "    auth_method TEXT DEFAULT 'password',"
            "    password_encrypted BLOB,"
            "    private_key_path TEXT,"
            "    key_passphrase_encrypted BLOB,"
            "    post_login_commands TEXT,"
            "    keep_alive_seconds INTEGER DEFAULT 30,"
            "    auto_reconnect INTEGER DEFAULT 0,"
            "    sort_index INTEGER DEFAULT 0,"
            "    FOREIGN KEY(folder_id) REFERENCES folders(id)"
            ")"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS host_keys ("
            "    host TEXT PRIMARY KEY,"
            "    port INTEGER DEFAULT 22,"
            "    key_type TEXT NOT NULL,"
            "    fingerprint TEXT NOT NULL,"
            "    trusted INTEGER DEFAULT 0"
            ")"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    // Saved folder-compare pairs, rooted at the remote machine (session).
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS compare_projects ("
            "    id TEXT PRIMARY KEY,"
            "    session_id TEXT NOT NULL,"
            "    name TEXT NOT NULL,"
            "    local_path TEXT NOT NULL,"
            "    remote_path TEXT NOT NULL,"
            "    last_used INTEGER DEFAULT 0,"
            "    UNIQUE(session_id, name)"
            ")"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    // Most-recently-used sessions (File > Recent Sessions).
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS session_history ("
            "    session_id TEXT PRIMARY KEY,"
            "    last_used INTEGER NOT NULL"
            ")"))) {
        m_lastError = query.lastError().text();
        return false;
    }

    return true;
}

} // namespace hssh

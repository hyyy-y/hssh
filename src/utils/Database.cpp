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

    return createSchema();
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

    return true;
}

} // namespace hssh

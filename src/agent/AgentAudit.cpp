#include "AgentAudit.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

namespace hssh {

namespace {
QMutex g_mutex;
constexpr qint64 kMaxLogSize = 5 * 1024 * 1024; // 5 MB
}

QString AgentAudit::sourceName(Source source)
{
    switch (source) {
    case Source::Rest:
        return QStringLiteral("REST");
    case Source::Mcp:
        return QStringLiteral("MCP");
    case Source::Cli:
        return QStringLiteral("CLI");
    }
    return QStringLiteral("?");
}

QString AgentAudit::logFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + QDir::separator() + QStringLiteral("logs");
    QDir().mkpath(dir);
    return dir + QDir::separator() + QStringLiteral("agent_audit.log");
}

void AgentAudit::log(Source source, const QString &action,
                     const QString &detail, const QString &result)
{
    QMutexLocker locker(&g_mutex);

    const QString path = logFilePath();
    QFile file(path);
    // Simple rotation: keep one previous generation.
    if (QFile::exists(path) && QFile(path).size() > kMaxLogSize) {
        QFile::remove(path + QStringLiteral(".1"));
        QFile::rename(path, path + QStringLiteral(".1"));
    }
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    const QString line = QStringLiteral("%1 [%2] %3 | %4 | %5\n")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                                  sourceName(source),
                                  action,
                                  detail,
                                  result);
    file.write(line.toUtf8());
}

} // namespace hssh

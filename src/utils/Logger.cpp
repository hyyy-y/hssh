#include "Logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

namespace hssh {

class Logger::Impl {
public:
    mutable QMutex mutex;
    LogLevel level = LogLevel::Info;
    QString logFilePath;
    QFile logFile;
    QTextStream stream;
};

Logger::Logger()
    : d(std::make_unique<Impl>())
{
    const QString dir = logDirectory();
    QDir().mkpath(dir);

    const QString fileName = QStringLiteral("hssh_%1.log")
                                 .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd")));
    d->logFilePath = dir + QDir::separator() + fileName;

    d->logFile.setFileName(d->logFilePath);
    if (!d->logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        qWarning() << "Failed to open log file:" << d->logFilePath;
    }
    d->stream.setDevice(&d->logFile);
}

Logger::~Logger() = default;

Logger &Logger::instance()
{
    static Logger logger;
    return logger;
}

void Logger::setLevel(LogLevel level)
{
    QMutexLocker locker(&d->mutex);
    d->level = level;
}

LogLevel Logger::level() const
{
    QMutexLocker locker(&d->mutex);
    return d->level;
}

QString Logger::logFilePath() const
{
    QMutexLocker locker(&d->mutex);
    return d->logFilePath;
}

void Logger::debug(const QString &category, const QString &message)
{
    log(LogLevel::Debug, category, message);
}

void Logger::info(const QString &category, const QString &message)
{
    log(LogLevel::Info, category, message);
}

void Logger::warning(const QString &category, const QString &message)
{
    log(LogLevel::Warning, category, message);
}

void Logger::error(const QString &category, const QString &message)
{
    log(LogLevel::Error, category, message);
}

void Logger::log(LogLevel level, const QString &category, const QString &message)
{
    {
        QMutexLocker locker(&d->mutex);
        if (level < d->level) {
            return;
        }
    }

    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"));
    const QString line = QStringLiteral("[%1] [%2] [%3] %4")
                             .arg(timestamp, levelString(level), category, message);

    {
        QMutexLocker locker(&d->mutex);
        if (d->logFile.isOpen()) {
            d->stream << line << QLatin1Char('\n');
            d->stream.flush();
        }
    }

    switch (level) {
    case LogLevel::Debug:
        qDebug().noquote() << line;
        break;
    case LogLevel::Info:
        qInfo().noquote() << line;
        break;
    case LogLevel::Warning:
        qWarning().noquote() << line;
        break;
    case LogLevel::Error:
        qCritical().noquote() << line;
        break;
    }
}

QString Logger::levelString(LogLevel level) const
{
    switch (level) {
    case LogLevel::Debug:
        return QStringLiteral("DEBUG");
    case LogLevel::Info:
        return QStringLiteral("INFO");
    case LogLevel::Warning:
        return QStringLiteral("WARN");
    case LogLevel::Error:
        return QStringLiteral("ERROR");
    }
    return QStringLiteral("UNKNOWN");
}

QString Logger::logDirectory() const
{
    const QString dataLocation = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dataLocation + QDir::separator() + QStringLiteral("logs");
}

} // namespace hssh

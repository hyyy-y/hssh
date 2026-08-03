#ifndef HSSH_UTILS_LOGGER_H
#define HSSH_UTILS_LOGGER_H

#include <QString>
#include <QMutex>
#include <memory>

namespace hssh {

enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

class Logger {
public:
    static Logger &instance();

    void debug(const QString &category, const QString &message);
    void info(const QString &category, const QString &message);
    void warning(const QString &category, const QString &message);
    void error(const QString &category, const QString &message);

    void setLevel(LogLevel level);
    LogLevel level() const;

    QString logFilePath() const;

private:
    Logger();
    ~Logger();

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    void log(LogLevel level, const QString &category, const QString &message);
    QString levelString(LogLevel level) const;
    QString logDirectory() const;

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace hssh

#endif // HSSH_UTILS_LOGGER_H

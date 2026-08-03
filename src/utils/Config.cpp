#include "Config.h"

#include <QCoreApplication>

namespace hssh {

Config::Config()
    : m_settings(QSettings::IniFormat,
                 QSettings::UserScope,
                 QCoreApplication::organizationName(),
                 QCoreApplication::applicationName())
{
}

Config::~Config() = default;

Config &Config::instance()
{
    static Config config;
    return config;
}

QVariant Config::value(const QString &key, const QVariant &defaultValue) const
{
    return m_settings.value(key, defaultValue);
}

void Config::setValue(const QString &key, const QVariant &value)
{
    m_settings.setValue(key, value);
}

void Config::remove(const QString &key)
{
    m_settings.remove(key);
}

bool Config::contains(const QString &key) const
{
    return m_settings.contains(key);
}

QString Config::stringValue(const QString &key, const QString &defaultValue) const
{
    return value(key, defaultValue).toString();
}

int Config::intValue(const QString &key, int defaultValue) const
{
    return value(key, defaultValue).toInt();
}

bool Config::boolValue(const QString &key, bool defaultValue) const
{
    return value(key, defaultValue).toBool();
}

void Config::sync()
{
    m_settings.sync();
}

} // namespace hssh

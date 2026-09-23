#include "Config.h"

#include <QCoreApplication>

namespace hssh {

namespace {

QVector<ConfigKey> &registry()
{
    static QVector<ConfigKey> keys;
    return keys;
}

bool &registrySeeded()
{
    static bool seeded = false;
    return seeded;
}

void seedDefaultKeys()
{
    if (registrySeeded()) {
        return;
    }
    registrySeeded() = true;

    QVector<ConfigKey> &keys = registry();
    keys = {
        // Appearance
        {QStringLiteral("theme/name"), ConfigKeyType::Enum, QStringLiteral("dark"),
         QStringLiteral("Appearance"), QStringLiteral("Theme"),
         {QStringLiteral("dark"), QStringLiteral("light")},
         QStringLiteral("Color theme applied to the whole application.")},
        {QStringLiteral("terminal/fontFamily"), ConfigKeyType::Font, QString(),
         QStringLiteral("Appearance"), QStringLiteral("Terminal font"),
         {}, QStringLiteral("Empty keeps the current built-in font.")},
        {QStringLiteral("terminal/fontSize"), ConfigKeyType::Int, 12,
         QStringLiteral("Appearance"), QStringLiteral("Terminal font size"),
         {}, QStringLiteral("Point size of the terminal font.")},
        {QStringLiteral("terminal/backgroundOpacity"), ConfigKeyType::Int, 100,
         QStringLiteral("Appearance"), QStringLiteral("Terminal background opacity (%)"),
         {}, QStringLiteral("100 is fully opaque; lower values show the window background.")},

        // Terminal behavior
        {QStringLiteral("terminal/mouseProtocol"), ConfigKeyType::Bool, true,
         QStringLiteral("Terminal"), QStringLiteral("Mouse protocol (vim/tmux)"),
         {}, QStringLiteral("Forward mouse events to applications that request them.")},
        {QStringLiteral("terminal/showTimestamps"), ConfigKeyType::Bool, false,
         QStringLiteral("Terminal"), QStringLiteral("Show timestamps"),
         {}, QStringLiteral("Prefix each output line with its arrival time.")},
        {QStringLiteral("terminal/osc52Mode"), ConfigKeyType::Enum, QStringLiteral("prompt"),
         QStringLiteral("Terminal"), QStringLiteral("OSC 52 clipboard sync"),
         {QStringLiteral("prompt"), QStringLiteral("allow"), QStringLiteral("deny")},
         QStringLiteral("Whether remote applications may read/write the local clipboard.")},

        // Security
        {QStringLiteral("security/hostKeyPolicy"), ConfigKeyType::Enum,
         QStringLiteral("accept-new"), QStringLiteral("Security"),
         QStringLiteral("Host key policy"),
         {QStringLiteral("accept-new"), QStringLiteral("ask"), QStringLiteral("accept-all")},
         QStringLiteral("accept-new trusts first use and rejects changed keys; ask prompts "
                        "in the GUI; accept-all never verifies.")},

        // Network
        {QStringLiteral("net/proxyType"), ConfigKeyType::Enum, QStringLiteral("none"),
         QStringLiteral("Network"), QStringLiteral("Outbound proxy"),
         {QStringLiteral("none"), QStringLiteral("http"), QStringLiteral("socks5")},
         QStringLiteral("Proxy used when opening SSH connections.")},
        {QStringLiteral("net/proxyHost"), ConfigKeyType::String, QString(),
         QStringLiteral("Network"), QStringLiteral("Proxy host"), {},
         QStringLiteral("Host name or address of the proxy.")},
        {QStringLiteral("net/proxyPort"), ConfigKeyType::Int, 8080,
         QStringLiteral("Network"), QStringLiteral("Proxy port"), {},
         QStringLiteral("TCP port of the proxy.")},
        {QStringLiteral("net/proxyUser"), ConfigKeyType::String, QString(),
         QStringLiteral("Network"), QStringLiteral("Proxy user"), {},
         QStringLiteral("Optional proxy authentication user.")},
        {QStringLiteral("net/proxyPass"), ConfigKeyType::Password, QString(),
         QStringLiteral("Network"), QStringLiteral("Proxy password"), {},
         QStringLiteral("Optional proxy authentication password.")},

        // Session
        {QStringLiteral("session/logging"), ConfigKeyType::Bool, true,
         QStringLiteral("Session"), QStringLiteral("Automatic session logging"),
         {}, QStringLiteral("Write session output to AppData/logs.")},
        {QStringLiteral("session/confirmCloseTabs"), ConfigKeyType::Bool, false,
         QStringLiteral("Session"), QStringLiteral("Confirm closing terminal tabs"),
         {}, QStringLiteral("Ask before closing a tab with an active connection.")},
        {QStringLiteral("session/restoreTabs"), ConfigKeyType::Bool, true,
         QStringLiteral("Session"), QStringLiteral("Restore previous tabs on startup"),
         {}, QStringLiteral("Ask to reopen the terminal tabs from the last run.")},

        // Agent
        {QStringLiteral("agent/autostart"), ConfigKeyType::Bool, false,
         QStringLiteral("Agent"), QStringLiteral("Start agent with the GUI"),
         {}, QStringLiteral("Expose the local REST API (127.0.0.1:8222) at startup.")},
        {QStringLiteral("agent/token"), ConfigKeyType::Password, QString(),
         QStringLiteral("Agent"), QStringLiteral("Agent access token"),
         {}, QStringLiteral("Bearer token required by the REST API.")},
    };
}

} // namespace

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
    const bool changed = !m_settings.contains(key) || m_settings.value(key) != value;
    m_settings.setValue(key, value);
    if (changed) {
        emit valueChanged(key, value);
    }
}

void Config::remove(const QString &key)
{
    m_settings.remove(key);
    emit valueChanged(key, QVariant());
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

void Config::registerKey(const ConfigKey &key)
{
    seedDefaultKeys();
    registry().append(key);
}

QVector<ConfigKey> Config::registeredKeys()
{
    seedDefaultKeys();
    return registry();
}

const ConfigKey *Config::findKey(const QString &key)
{
    seedDefaultKeys();
    for (const ConfigKey &k : registry()) {
        if (k.key == key) {
            return &k;
        }
    }
    return nullptr;
}

void Config::sync()
{
    m_settings.sync();
}

} // namespace hssh

#ifndef HSSH_UTILS_CONFIG_H
#define HSSH_UTILS_CONFIG_H

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

namespace hssh {

// Editor kind used by SettingsDialog to render a config entry.
enum class ConfigKeyType {
    Bool,
    Int,
    String,
    Password,
    Enum,
    Font,
    Color
};

// One user-facing configuration entry. The SettingsDialog is generated from
// the registry (see Config::registeredKeys()).
struct ConfigKey {
    QString key;
    ConfigKeyType type = ConfigKeyType::String;
    QVariant defaultValue;
    QString group;      // Settings dialog group (e.g. "Terminal").
    QString label;      // User-facing label.
    QStringList enumOptions; // For ConfigKeyType::Enum.
    QString tooltip;
};

class Config : public QObject {
    Q_OBJECT

public:
    static Config &instance();

    QVariant value(const QString &key, const QVariant &defaultValue = QVariant()) const;
    void setValue(const QString &key, const QVariant &value);
    void remove(const QString &key);
    bool contains(const QString &key) const;

    // Convenience accessors.
    QString stringValue(const QString &key, const QString &defaultValue = QString()) const;
    int intValue(const QString &key, int defaultValue = 0) const;
    bool boolValue(const QString &key, bool defaultValue = false) const;

    // Key registry: defaults are seeded lazily on first access; the
    // SettingsDialog renders every registered key.
    static void registerKey(const ConfigKey &key);
    static QVector<ConfigKey> registeredKeys();
    static const ConfigKey *findKey(const QString &key);

    void sync();

signals:
    // Emitted by setValue() after the new value is stored. Consumers apply
    // the change live.
    void valueChanged(const QString &key, const QVariant &value);

private:
    Config();
    ~Config() override;

    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

    QSettings m_settings;
};

} // namespace hssh

#endif // HSSH_UTILS_CONFIG_H

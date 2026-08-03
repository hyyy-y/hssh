#ifndef HSSH_UTILS_CONFIG_H
#define HSSH_UTILS_CONFIG_H

#include <QSettings>
#include <QVariant>
#include <QString>

namespace hssh {

class Config {
public:
    static Config &instance();

    QVariant value(const QString &key, const QVariant &defaultValue = QVariant()) const;
    void setValue(const QString &key, const QVariant &value);
    void remove(const QString &key);
    bool contains(const QString &key) const;

    // Convenience accessors
    QString stringValue(const QString &key, const QString &defaultValue = QString()) const;
    int intValue(const QString &key, int defaultValue = 0) const;
    bool boolValue(const QString &key, bool defaultValue = false) const;

    void sync();

private:
    Config();
    ~Config();

    Config(const Config &) = delete;
    Config &operator=(const Config &) = delete;

    QSettings m_settings;
};

} // namespace hssh

#endif // HSSH_UTILS_CONFIG_H

#ifndef HSSH_APP_DIALOGS_SETTINGSDIALOG_H
#define HSSH_APP_DIALOGS_SETTINGSDIALOG_H

#include "utils/Config.h"

#include <QDialog>
#include <QList>
#include <QVariant>

class QListWidget;
class QStackedWidget;
class QWidget;

namespace hssh {

// Settings dialog auto-generated from the Config key registry. Every
// registered ConfigKey gets an editor widget grouped by ConfigKey::group.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);

private:
    struct Row {
        ConfigKey key;
        QWidget *editor = nullptr;
        QVariant original;
    };

    void buildUi();
    void applyChanges();
    QWidget *createEditor(const ConfigKey &key, QWidget *parent, QVariant *original);
    QVariant readEditor(const Row &row) const;

    QListWidget *m_groupList = nullptr;
    QStackedWidget *m_pages = nullptr;
    QList<Row> m_rows;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_SETTINGSDIALOG_H

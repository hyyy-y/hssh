#ifndef HSSH_APP_DIALOGS_IMPORTSSHCONFIGDIALOG_H
#define HSSH_APP_DIALOGS_IMPORTSSHCONFIGDIALOG_H

#include "core/SessionConfig.h"

#include <QDialog>
#include <QList>

class QCheckBox;
class QTableWidget;

namespace hssh {

// PH1-07: import sessions from an OpenSSH ~/.ssh/config file.
// Parses Host blocks (HostName/Port/User/IdentityFile/ProxyJump), shows a
// checkable preview, and turns the selected entries into SessionConfigs.
class ImportSshConfigDialog : public QDialog {
    Q_OBJECT

public:
    struct Entry {
        QString alias;        // Host pattern (session display name)
        QString hostName;     // HostName (falls back to the alias)
        int port = 22;
        QString user;
        QString identityFile; // empty -> password auth
        QString proxyJump;    // stored for PH1-03 (not yet used at connect)
        bool operator==(const Entry &other) const { return alias == other.alias; }
    };

    explicit ImportSshConfigDialog(QWidget *parent = nullptr);

    // Parses an OpenSSH config text. Wildcard Host patterns (*, ?, !) are
    // skipped: they are patterns, not machines.
    static QList<Entry> parseConfig(const QString &text);

    // SessionConfigs built from the checked entries.
    QList<SessionConfig> selectedConfigs() const;

private:
    void fillTable(const QList<Entry> &entries);

    QTableWidget *m_table = nullptr;
    QList<Entry> m_entries;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_IMPORTSSHCONFIGDIALOG_H

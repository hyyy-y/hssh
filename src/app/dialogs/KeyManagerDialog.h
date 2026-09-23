#ifndef HSSH_APP_DIALOGS_KEYMANAGERDIALOG_H
#define HSSH_APP_DIALOGS_KEYMANAGERDIALOG_H

#include "core/KeyStore.h"

#include <QDialog>

class QPushButton;
class QTableWidget;

namespace hssh {

// PH1-09: list / generate / import / export / delete SSH keys stored in
// KeyStore::keysDir(). The dialog is modal; every action refreshes the list.
class KeyManagerDialog : public QDialog {
    Q_OBJECT

public:
    explicit KeyManagerDialog(QWidget *parent = nullptr);

    // Modal key picker used by the session dialog ("Key Store..." button).
    // Returns the selected key's file path, or an empty string on cancel.
    [[nodiscard]] static QString pickKeyPath(QWidget *parent = nullptr);

private:
    void refresh();
    void generate();
    void importKey();
    void exportSelected();
    void deleteSelected();
    void updateButtonStates();
    // PH2-12: known-hosts panel of the manager.
    void refreshKnownHosts();
    void removeSelectedKnownHost();

    [[nodiscard]] int currentRow() const;

    QTableWidget *m_table = nullptr;
    QPushButton *m_exportButton = nullptr;
    QPushButton *m_deleteButton = nullptr;
    QVector<KeyStore::KeyInfo> m_keys;
    // PH2-12: known-hosts panel.
    QTableWidget *m_knownHostsTable = nullptr;
    QPushButton *m_removeKnownHostButton = nullptr;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_KEYMANAGERDIALOG_H

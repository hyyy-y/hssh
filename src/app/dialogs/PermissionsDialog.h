#ifndef HSSH_APP_DIALOGS_PERMISSIONSDIALOG_H
#define HSSH_APP_DIALOGS_PERMISSIONSDIALOG_H

#include "core/SftpSession.h"

#include <QDialog>

class QCheckBox;
class QLabel;
class QLineEdit;

namespace hssh {

// PH3-14: unix permission editor for one remote entry. Nine r/w/x check
// boxes (owner/group/others) kept in sync with an octal input; directories
// offer "apply recursively". Owner/group are display-only — chown stays
// behind the sudo confirmation (later batch).
class PermissionsDialog : public QDialog {
    Q_OBJECT

public:
    PermissionsDialog(const QString &remotePath, const SftpFileInfo &info,
                      QWidget *parent = nullptr);

    // Parse "644"-style octal (000..777) into mode bits.
    [[nodiscard]] static bool parseOctal(const QString &text, quint32 *mode);
    // "-rw-r--r--"-style rendering without the leading type char.
    [[nodiscard]] static QString symbolicFromMode(quint32 mode);

signals:
    // Fired when the user confirms: mode to apply, plus recursive for
    // directories (apply to everything below remotePath).
    void applied(quint32 mode, bool recursive);

private:
    void syncFromCheckBoxes();
    void syncFromOctalEdit();
    [[nodiscard]] quint32 currentMode() const;
    void setCurrentMode(quint32 mode);

    QString m_remotePath;
    bool m_isDir = false;
    QCheckBox *m_boxes[3][3] = {}; // [who][rwx]
    QLineEdit *m_octalEdit = nullptr;
    QLabel *m_symbolicLabel = nullptr;
    QLabel *m_ownerLabel = nullptr;
    QCheckBox *m_recursiveCheck = nullptr;
    bool m_syncing = false; // guard against checkbox<->octal signal loops
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_PERMISSIONSDIALOG_H

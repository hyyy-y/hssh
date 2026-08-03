#ifndef HSSH_APP_DIALOGS_DIFFDIALOG_H
#define HSSH_APP_DIALOGS_DIFFDIALOG_H

#include <QDialog>

namespace hssh {

// Side-by-side line diff dialog: thin wrapper around DiffView with a Close
// button. Local file on the left, remote on the right.
class DiffDialog : public QDialog {
    Q_OBJECT

public:
    DiffDialog(const QString &leftTitle, const QString &leftText,
               const QString &rightTitle, const QString &rightText,
               QWidget *parent = nullptr);
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_DIFFDIALOG_H

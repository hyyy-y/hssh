#ifndef HSSH_APP_DIALOGS_PORTFORWARDDIALOG_H
#define HSSH_APP_DIALOGS_PORTFORWARDDIALOG_H

#include "core/PortForward.h"

#include <QDialog>
#include <QList>

class QComboBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace hssh {

class SshSession;

// Manages the port forwards of one connected SSH session: local, remote and
// dynamic (SOCKS5) rules with live status.
class PortForwardDialog : public QDialog {
    Q_OBJECT

public:
    explicit PortForwardDialog(SshSession *session, QWidget *parent = nullptr);
    ~PortForwardDialog() override;

private:
    void refreshTable();
    void addForward();
    void removeSelected();
    void setTargetEnabled(bool enabled);

    SshSession *m_session = nullptr;
    QComboBox *m_typeCombo = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_bindAddressEdit = nullptr;
    QSpinBox *m_bindPortSpin = nullptr;
    QLineEdit *m_targetHostEdit = nullptr;
    QSpinBox *m_targetPortSpin = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton *m_removeButton = nullptr;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_PORTFORWARDDIALOG_H

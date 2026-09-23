#ifndef HSSH_APP_DIALOGS_NEWSESSIONDIALOG_H
#define HSSH_APP_DIALOGS_NEWSESSIONDIALOG_H

#include "core/SessionConfig.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;

namespace hssh {

class NewSessionDialog : public QDialog {
    Q_OBJECT

public:
    explicit NewSessionDialog(QWidget *parent = nullptr);
    ~NewSessionDialog() override;

    void setSessionConfig(const SessionConfig &config);
    [[nodiscard]] SessionConfig sessionConfig() const;

private:
    void setupUi();
    void accept() override;

    QLineEdit *m_nameEdit = nullptr;
    QWidget *m_sshFields = nullptr;
    QWidget *m_serialFields = nullptr;
    QComboBox *m_typeEdit = nullptr;
    QComboBox *m_serialPortEdit = nullptr;
    QComboBox *m_serialBaudEdit = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portEdit = nullptr;
    QLineEdit *m_usernameEdit = nullptr;
    QComboBox *m_authMethodEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QLineEdit *m_keyPathEdit = nullptr;
    QLineEdit *m_keyPassphraseEdit = nullptr;
    QSpinBox *m_keepAliveSpin = nullptr;
    QCheckBox *m_autoReconnectCheck = nullptr;
    QCheckBox *m_forwardAgentCheck = nullptr;
    // PH1-02/03: proxy + jump host.
    QComboBox *m_proxyTypeEdit = nullptr;
    QLineEdit *m_proxyHostEdit = nullptr;
    QSpinBox *m_proxyPortEdit = nullptr;
    QLineEdit *m_proxyUsernameEdit = nullptr;
    QLineEdit *m_proxyPasswordEdit = nullptr;
    QLineEdit *m_jumpHostEdit = nullptr;
    QSpinBox *m_jumpPortEdit = nullptr;
    QLineEdit *m_jumpUsernameEdit = nullptr;
    QLineEdit *m_jumpPasswordEdit = nullptr;
    QLineEdit *m_jumpKeyPathEdit = nullptr;

    SessionConfig m_config;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_NEWSESSIONDIALOG_H

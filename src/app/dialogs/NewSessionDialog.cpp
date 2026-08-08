#include "NewSessionDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace hssh {

NewSessionDialog::NewSessionDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi();
    setWindowTitle(tr("New Session"));
    setMinimumWidth(400);
}

NewSessionDialog::~NewSessionDialog() = default;

void NewSessionDialog::setSessionConfig(const SessionConfig &config)
{
    m_config = config;
    m_nameEdit->setText(config.name());
    m_hostEdit->setText(config.host());
    m_portEdit->setValue(config.port());
    m_usernameEdit->setText(config.username());
    m_authMethodEdit->setCurrentIndex(static_cast<int>(config.authMethod()));
    m_passwordEdit->setText(config.password().toString());
    m_keyPathEdit->setText(config.privateKeyPath());
    m_keyPassphraseEdit->setText(config.keyPassphrase().toString());
    m_keepAliveSpin->setValue(config.keepAliveSeconds());
    m_autoReconnectCheck->setChecked(config.autoReconnect());
}

SessionConfig NewSessionDialog::sessionConfig() const
{
    return m_config;
}

void NewSessionDialog::setupUi()
{
    auto *layout = new QVBoxLayout(this);

    auto *formLayout = new QFormLayout();
    formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(tr("My Server"));
    formLayout->addRow(tr("Name:"), m_nameEdit);

    m_hostEdit = new QLineEdit(this);
    m_hostEdit->setPlaceholderText(tr("example.com or 192.168.1.1"));
    formLayout->addRow(tr("Host:"), m_hostEdit);

    m_portEdit = new QSpinBox(this);
    m_portEdit->setRange(1, 65535);
    m_portEdit->setValue(22);
    formLayout->addRow(tr("Port:"), m_portEdit);

    m_usernameEdit = new QLineEdit(this);
    m_usernameEdit->setPlaceholderText(tr("root"));
    formLayout->addRow(tr("Username:"), m_usernameEdit);

    m_authMethodEdit = new QComboBox(this);
    m_authMethodEdit->addItem(tr("Password"), static_cast<int>(AuthMethod::Password));
    m_authMethodEdit->addItem(tr("Public Key"), static_cast<int>(AuthMethod::PublicKey));
    m_authMethodEdit->addItem(tr("Keyboard Interactive"), static_cast<int>(AuthMethod::KeyboardInteractive));
    m_authMethodEdit->addItem(tr("SSH Agent"), static_cast<int>(AuthMethod::Agent));
    formLayout->addRow(tr("Auth Method:"), m_authMethodEdit);

    m_passwordEdit = new QLineEdit(this);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    formLayout->addRow(tr("Password:"), m_passwordEdit);

    m_keyPathEdit = new QLineEdit(this);
    formLayout->addRow(tr("Private Key Path:"), m_keyPathEdit);

    m_keyPassphraseEdit = new QLineEdit(this);
    m_keyPassphraseEdit->setEchoMode(QLineEdit::Password);
    formLayout->addRow(tr("Key Passphrase:"), m_keyPassphraseEdit);

    layout->addLayout(formLayout);

    auto *connectionGroup = new QGroupBox(tr("Connection"), this);
    auto *connectionLayout = new QFormLayout(connectionGroup);

    m_keepAliveSpin = new QSpinBox(connectionGroup);
    m_keepAliveSpin->setRange(0, 86400);
    m_keepAliveSpin->setValue(30);
    m_keepAliveSpin->setSuffix(tr(" s"));
    m_keepAliveSpin->setSpecialValueText(tr("Disabled"));
    connectionLayout->addRow(tr("Keep-alive interval:"), m_keepAliveSpin);

    m_autoReconnectCheck = new QCheckBox(tr("Reconnect automatically after a connection drop"), connectionGroup);
    connectionLayout->addRow(QString(), m_autoReconnectCheck);

    layout->addWidget(connectionGroup);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &NewSessionDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &NewSessionDialog::reject);
    layout->addWidget(buttons);
}

void NewSessionDialog::accept()
{
    if (m_hostEdit->text().trimmed().isEmpty()) {
        m_hostEdit->setFocus();
        return;
    }

    m_config.setName(m_nameEdit->text().trimmed());
    m_config.setHost(m_hostEdit->text().trimmed());
    m_config.setPort(m_portEdit->value());
    m_config.setUsername(m_usernameEdit->text().trimmed());
    m_config.setAuthMethod(static_cast<AuthMethod>(m_authMethodEdit->currentData().toInt()));
    m_config.setPassword(SecureString(m_passwordEdit->text()));
    m_config.setPrivateKeyPath(m_keyPathEdit->text().trimmed());
    m_config.setKeyPassphrase(SecureString(m_keyPassphraseEdit->text()));
    m_config.setKeepAliveSeconds(m_keepAliveSpin->value());
    m_config.setAutoReconnect(m_autoReconnectCheck->isChecked());

    QDialog::accept();
}

} // namespace hssh

#include "NewSessionDialog.h"

#include "KeyManagerDialog.h"
#include "terminal/SerialShellProcess.h"

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
    m_forwardAgentCheck->setChecked(config.forwardAgent());
    const int proxyIndex = m_proxyTypeEdit->findData(
        config.proxyType().isEmpty() ? QString() : config.proxyType());
    m_proxyTypeEdit->setCurrentIndex(proxyIndex < 0 ? 0 : proxyIndex);
    m_proxyHostEdit->setText(config.proxyHost());
    m_proxyPortEdit->setValue(config.proxyPort() > 0 ? config.proxyPort() : 1080);
    m_proxyUsernameEdit->setText(config.proxyUsername());
    m_proxyPasswordEdit->setText(config.proxyPassword().toString());
    m_jumpHostEdit->setText(config.jumpHost());
    m_jumpPortEdit->setValue(config.jumpPort() > 0 ? config.jumpPort() : 22);
    m_jumpUsernameEdit->setText(config.jumpUsername());
    m_jumpPasswordEdit->setText(config.jumpPassword().toString());
    m_jumpKeyPathEdit->setText(config.jumpPrivateKeyPath());

    // Type + serial fields.
    const int typeIndex = m_typeEdit->findData(static_cast<int>(config.sessionType()));
    m_typeEdit->setCurrentIndex(typeIndex >= 0 ? typeIndex : 0);
    m_serialPortEdit->setCurrentText(config.serialPort());
    if (config.serialBaudRate() > 0) {
        m_serialBaudEdit->setCurrentText(QString::number(config.serialBaudRate()));
    }
    const bool serial = config.sessionType() == SessionType::Serial;
    m_sshFields->setVisible(!serial);
    m_serialFields->setVisible(serial);
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

    m_typeEdit = new QComboBox(this);
    m_typeEdit->addItem(tr("SSH"), static_cast<int>(SessionType::Ssh));
    m_typeEdit->addItem(tr("Serial (COM/tty)"), static_cast<int>(SessionType::Serial));
    formLayout->addRow(tr("Type:"), m_typeEdit);
    connect(m_typeEdit, &QComboBox::currentIndexChanged, this, [this]() {
        const bool serial = static_cast<SessionType>(m_typeEdit->currentData().toInt())
            == SessionType::Serial;
        m_sshFields->setVisible(!serial);
        m_serialFields->setVisible(serial);
    });

    m_sshFields = new QWidget(this);
    auto *sshLayout = new QFormLayout(m_sshFields);
    sshLayout->setContentsMargins(0, 0, 0, 0);
    sshLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_hostEdit = new QLineEdit(m_sshFields);
    m_hostEdit->setPlaceholderText(tr("example.com or 192.168.1.1"));
    sshLayout->addRow(tr("Host:"), m_hostEdit);

    m_portEdit = new QSpinBox(m_sshFields);
    m_portEdit->setRange(1, 65535);
    m_portEdit->setValue(22);
    sshLayout->addRow(tr("Port:"), m_portEdit);

    m_usernameEdit = new QLineEdit(m_sshFields);
    m_usernameEdit->setPlaceholderText(tr("root"));
    sshLayout->addRow(tr("Username:"), m_usernameEdit);

    m_authMethodEdit = new QComboBox(m_sshFields);
    m_authMethodEdit->addItem(tr("Password"), static_cast<int>(AuthMethod::Password));
    m_authMethodEdit->addItem(tr("Public Key"), static_cast<int>(AuthMethod::PublicKey));
    m_authMethodEdit->addItem(tr("Keyboard Interactive"), static_cast<int>(AuthMethod::KeyboardInteractive));
    m_authMethodEdit->addItem(tr("SSH Agent"), static_cast<int>(AuthMethod::Agent));
    sshLayout->addRow(tr("Auth Method:"), m_authMethodEdit);

    m_passwordEdit = new QLineEdit(m_sshFields);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    sshLayout->addRow(tr("Password:"), m_passwordEdit);

    auto *keyPathRow = new QWidget(m_sshFields);
    auto *keyPathLayout = new QHBoxLayout(keyPathRow);
    keyPathLayout->setContentsMargins(0, 0, 0, 0);
    m_keyPathEdit = new QLineEdit(keyPathRow);
    keyPathLayout->addWidget(m_keyPathEdit);
    auto *keyStoreButton = new QPushButton(tr("Key Store..."), keyPathRow);
    keyPathLayout->addWidget(keyStoreButton);
    // PH1-09: pick a managed key from the hssh key store.
    connect(keyStoreButton, &QPushButton::clicked, this, [this]() {
        const QString path = KeyManagerDialog::pickKeyPath(this);
        if (path.isEmpty()) {
            return;
        }
        m_keyPathEdit->setText(path);
        const int index = m_authMethodEdit->findData(static_cast<int>(AuthMethod::PublicKey));
        if (index >= 0) {
            m_authMethodEdit->setCurrentIndex(index);
        }
    });
    sshLayout->addRow(tr("Private Key Path:"), keyPathRow);

    m_keyPassphraseEdit = new QLineEdit(m_sshFields);
    m_keyPassphraseEdit->setEchoMode(QLineEdit::Password);
    sshLayout->addRow(tr("Key Passphrase:"), m_keyPassphraseEdit);

    formLayout->addRow(m_sshFields);

    m_serialFields = new QWidget(this);
    auto *serialLayout = new QFormLayout(m_serialFields);
    serialLayout->setContentsMargins(0, 0, 0, 0);
    m_serialPortEdit = new QComboBox(m_serialFields);
    m_serialPortEdit->setEditable(true);
    const QStringList ports = SerialShellProcess::availablePorts();
    if (!ports.isEmpty()) {
        m_serialPortEdit->addItems(ports);
    } else {
        m_serialPortEdit->setPlaceholderText(tr("COM3 / /dev/ttyUSB0"));
    }
    serialLayout->addRow(tr("Serial Port:"), m_serialPortEdit);
    m_serialBaudEdit = new QComboBox(m_serialFields);
    m_serialBaudEdit->setEditable(true);
    m_serialBaudEdit->addItems({QStringLiteral("115200"), QStringLiteral("57600"),
                                QStringLiteral("38400"), QStringLiteral("19200"),
                                QStringLiteral("9600")});
    m_serialBaudEdit->setCurrentText(QStringLiteral("115200"));
    serialLayout->addRow(tr("Baud Rate:"), m_serialBaudEdit);
    m_serialFields->setVisible(false);
    formLayout->addRow(m_serialFields);

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

    m_forwardAgentCheck = new QCheckBox(tr("Forward local ssh-agent to this host (needs the ssh-agent service)"), connectionGroup);
    connectionLayout->addRow(QString(), m_forwardAgentCheck);

    layout->addWidget(connectionGroup);

    // PH1-02/03: proxy and jump host.
    auto *proxyGroup = new QGroupBox(tr("Proxy && Jump Host"), this);
    auto *proxyLayout = new QFormLayout(proxyGroup);

    m_proxyTypeEdit = new QComboBox(proxyGroup);
    m_proxyTypeEdit->addItem(tr("None (direct)"), QString());
    m_proxyTypeEdit->addItem(QStringLiteral("HTTP"), QStringLiteral("http"));
    m_proxyTypeEdit->addItem(QStringLiteral("SOCKS5"), QStringLiteral("socks5"));
    proxyLayout->addRow(tr("Proxy:"), m_proxyTypeEdit);

    m_proxyHostEdit = new QLineEdit(proxyGroup);
    proxyLayout->addRow(tr("Proxy Host:"), m_proxyHostEdit);

    m_proxyPortEdit = new QSpinBox(proxyGroup);
    m_proxyPortEdit->setRange(1, 65535);
    m_proxyPortEdit->setValue(1080);
    proxyLayout->addRow(tr("Proxy Port:"), m_proxyPortEdit);

    m_proxyUsernameEdit = new QLineEdit(proxyGroup);
    proxyLayout->addRow(tr("Proxy Username:"), m_proxyUsernameEdit);

    m_proxyPasswordEdit = new QLineEdit(proxyGroup);
    m_proxyPasswordEdit->setEchoMode(QLineEdit::Password);
    proxyLayout->addRow(tr("Proxy Password:"), m_proxyPasswordEdit);

    m_jumpHostEdit = new QLineEdit(proxyGroup);
    m_jumpHostEdit->setPlaceholderText(tr("empty = direct connection"));
    proxyLayout->addRow(tr("Jump Host:"), m_jumpHostEdit);

    m_jumpPortEdit = new QSpinBox(proxyGroup);
    m_jumpPortEdit->setRange(1, 65535);
    m_jumpPortEdit->setValue(22);
    proxyLayout->addRow(tr("Jump Port:"), m_jumpPortEdit);

    m_jumpUsernameEdit = new QLineEdit(proxyGroup);
    m_jumpUsernameEdit->setPlaceholderText(tr("empty = same as target username"));
    proxyLayout->addRow(tr("Jump Username:"), m_jumpUsernameEdit);

    m_jumpPasswordEdit = new QLineEdit(proxyGroup);
    m_jumpPasswordEdit->setEchoMode(QLineEdit::Password);
    proxyLayout->addRow(tr("Jump Password:"), m_jumpPasswordEdit);

    m_jumpKeyPathEdit = new QLineEdit(proxyGroup);
    m_jumpKeyPathEdit->setPlaceholderText(tr("empty = password auth for jump"));
    proxyLayout->addRow(tr("Jump Key Path:"), m_jumpKeyPathEdit);

    layout->addWidget(proxyGroup);

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
    m_config.setForwardAgent(m_forwardAgentCheck->isChecked());

    // Proxy + jump host.
    m_config.setProxyType(m_proxyTypeEdit->currentData().toString());
    m_config.setProxyHost(m_proxyHostEdit->text().trimmed());
    m_config.setProxyPort(m_proxyPortEdit->value());
    m_config.setProxyUsername(m_proxyUsernameEdit->text().trimmed());
    m_config.setProxyPassword(SecureString(m_proxyPasswordEdit->text()));
    m_config.setJumpHost(m_jumpHostEdit->text().trimmed());
    m_config.setJumpPort(m_jumpPortEdit->value());
    m_config.setJumpUsername(m_jumpUsernameEdit->text().trimmed());
    m_config.setJumpPassword(SecureString(m_jumpPasswordEdit->text()));
    m_config.setJumpPrivateKeyPath(m_jumpKeyPathEdit->text().trimmed());
    m_config.setSessionType(
        static_cast<SessionType>(m_typeEdit->currentData().toInt()));
    m_config.setSerialPort(m_serialPortEdit->currentText().trimmed());
    m_config.setSerialBaudRate(m_serialBaudEdit->currentText().toInt());

    QDialog::accept();
}

} // namespace hssh

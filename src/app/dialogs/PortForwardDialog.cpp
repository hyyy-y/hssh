#include "PortForwardDialog.h"

#include "core/SshSession.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace hssh {

PortForwardDialog::PortForwardDialog(SshSession *session, QWidget *parent)
    : QDialog(parent)
    , m_session(session)
{
    setWindowTitle(tr("Port Forwarding"));
    resize(720, 420);

    auto *layout = new QVBoxLayout(this);

    auto *addGroup = new QGroupBox(tr("Add Forward"), this);
    auto *form = new QFormLayout(addGroup);

    m_typeCombo = new QComboBox(addGroup);
    m_typeCombo->addItem(tr("Local"), static_cast<int>(ForwardSpec::Type::Local));
    m_typeCombo->addItem(tr("Remote"), static_cast<int>(ForwardSpec::Type::Remote));
    m_typeCombo->addItem(tr("Dynamic (SOCKS5)"), static_cast<int>(ForwardSpec::Type::Dynamic));
    form->addRow(tr("Type:"), m_typeCombo);

    m_nameEdit = new QLineEdit(addGroup);
    m_nameEdit->setPlaceholderText(tr("optional"));
    form->addRow(tr("Name:"), m_nameEdit);

    m_bindAddressEdit = new QLineEdit(addGroup);
    m_bindAddressEdit->setText(QStringLiteral("127.0.0.1"));
    form->addRow(tr("Bind address:"), m_bindAddressEdit);

    m_bindPortSpin = new QSpinBox(addGroup);
    m_bindPortSpin->setRange(1, 65535);
    m_bindPortSpin->setValue(8080);
    form->addRow(tr("Bind port:"), m_bindPortSpin);

    m_targetHostEdit = new QLineEdit(addGroup);
    m_targetHostEdit->setPlaceholderText(tr("localhost or 192.168.1.10"));
    form->addRow(tr("Target host:"), m_targetHostEdit);

    m_targetPortSpin = new QSpinBox(addGroup);
    m_targetPortSpin->setRange(1, 65535);
    m_targetPortSpin->setValue(80);
    form->addRow(tr("Target port:"), m_targetPortSpin);

    auto *addButton = new QPushButton(tr("Add Forward"), addGroup);
    form->addRow(addButton);
    layout->addWidget(addGroup);

    connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) {
                const bool isDynamic = m_typeCombo->currentData().toInt()
                    == static_cast<int>(ForwardSpec::Type::Dynamic);
                m_targetHostEdit->setEnabled(!isDynamic);
                m_targetPortSpin->setEnabled(!isDynamic);
            });
    connect(addButton, &QPushButton::clicked, this, &PortForwardDialog::addForward);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(4);
    m_table->setHorizontalHeaderLabels({tr("Type"), tr("Bind"), tr("Target"), tr("Status")});
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_table);

    auto *buttonRow = new QHBoxLayout();
    m_removeButton = new QPushButton(tr("Remove Selected"), this);
    m_removeButton->setEnabled(false);
    buttonRow->addWidget(m_removeButton);
    buttonRow->addStretch();
    auto *closeButton = new QPushButton(tr("Close"), this);
    buttonRow->addWidget(closeButton);
    layout->addLayout(buttonRow);

    connect(m_removeButton, &QPushButton::clicked, this, &PortForwardDialog::removeSelected);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() {
        m_removeButton->setEnabled(!m_table->selectedItems().isEmpty());
    });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

    if (m_session) {
        connect(m_session->portForwardManager(), &PortForwardManager::forwardsChanged,
                this, &PortForwardDialog::refreshTable);
    }
    refreshTable();
}

PortForwardDialog::~PortForwardDialog() = default;

void PortForwardDialog::refreshTable()
{
    m_table->setRowCount(0);
    if (!m_session) {
        return;
    }
    PortForwardManager *manager = m_session->portForwardManager();
    const QList<ForwardSpec> forwards = manager->forwards();
    m_table->setRowCount(forwards.size());
    for (int row = 0; row < forwards.size(); ++row) {
        const ForwardSpec &spec = forwards.at(row);
        const QString typeText = spec.type == ForwardSpec::Type::Local ? tr("Local")
                                 : spec.type == ForwardSpec::Type::Remote ? tr("Remote")
                                                                          : tr("SOCKS5");
        m_table->setItem(row, 0, new QTableWidgetItem(typeText));
        m_table->setItem(row, 1, new QTableWidgetItem(
            QStringLiteral("%1:%2").arg(spec.bindAddress).arg(spec.bindPort)));
        m_table->setItem(row, 2, new QTableWidgetItem(
            spec.type == ForwardSpec::Type::Dynamic
                ? tr("any (dynamic)")
                : QStringLiteral("%1:%2").arg(spec.targetHost).arg(spec.targetPort)));
        m_table->setItem(row, 3, new QTableWidgetItem(manager->statusText(row)));
        m_table->item(row, 0)->setData(Qt::UserRole, row);
    }
}

void PortForwardDialog::addForward()
{
    if (!m_session) {
        return;
    }
    ForwardSpec spec;
    spec.type = static_cast<ForwardSpec::Type>(m_typeCombo->currentData().toInt());
    spec.name = m_nameEdit->text().trimmed();
    spec.bindAddress = m_bindAddressEdit->text().trimmed();
    if (spec.bindAddress.isEmpty()) {
        spec.bindAddress = QStringLiteral("127.0.0.1");
    }
    spec.bindPort = static_cast<quint16>(m_bindPortSpin->value());
    spec.targetHost = m_targetHostEdit->text().trimmed();
    spec.targetPort = static_cast<quint16>(m_targetPortSpin->value());

    QString error;
    if (!m_session->portForwardManager()->addForward(spec, &error)) {
        QMessageBox::warning(this, tr("Port Forwarding"),
                             tr("Failed to start forward: %1").arg(error));
    }
}

void PortForwardDialog::removeSelected()
{
    if (!m_session || m_table->selectedItems().isEmpty()) {
        return;
    }
    const int row = m_table->currentRow();
    if (row < 0) {
        return;
    }
    m_session->portForwardManager()->removeForward(row);
}

} // namespace hssh

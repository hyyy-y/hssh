#include "EditFolderDialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QVBoxLayout>

namespace hssh {

EditFolderDialog::EditFolderDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi();
    setWindowTitle(tr("Folder"));
    setMinimumWidth(300);
}

EditFolderDialog::~EditFolderDialog() = default;

void EditFolderDialog::setFolderName(const QString &name)
{
    m_nameEdit->setText(name);
}

QString EditFolderDialog::folderName() const
{
    return m_nameEdit->text().trimmed();
}

void EditFolderDialog::setupUi()
{
    auto *layout = new QVBoxLayout(this);

    auto *formLayout = new QFormLayout();
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(tr("Folder Name"));
    formLayout->addRow(tr("Name:"), m_nameEdit);
    layout->addLayout(formLayout);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &EditFolderDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &EditFolderDialog::reject);
    layout->addWidget(buttons);
}

void EditFolderDialog::accept()
{
    if (m_nameEdit->text().trimmed().isEmpty()) {
        m_nameEdit->setFocus();
        return;
    }
    QDialog::accept();
}

} // namespace hssh

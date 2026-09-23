#include "KbdintPromptDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace hssh {

KbdintPromptDialog::KbdintPromptDialog(const QString &name, const QString &instruction,
                                       const QStringList &prompts, const QList<bool> &echo,
                                       QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(name.isEmpty() ? tr("Two-Factor Authentication") : name);
    setMinimumWidth(420);

    auto *layout = new QVBoxLayout(this);
    if (!instruction.isEmpty()) {
        auto *label = new QLabel(instruction, this);
        label->setWordWrap(true);
        layout->addWidget(label);
    }

    auto *form = new QFormLayout();
    for (int i = 0; i < prompts.size(); ++i) {
        auto *edit = new QLineEdit(this);
        const bool masked = (i < echo.size()) ? !echo.at(i) : true;
        if (masked) {
            edit->setEchoMode(QLineEdit::Password);
        }
        m_edits.append(edit);
        form->addRow(prompts.at(i), edit);
    }
    layout->addLayout(form);

    m_remember = new QCheckBox(tr("Remember answers for this session"), this);
    layout->addWidget(m_remember);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(box);

    if (!m_edits.isEmpty()) {
        m_edits.first()->setFocus();
    }
}

QStringList KbdintPromptDialog::answers() const
{
    QStringList result;
    result.reserve(m_edits.size());
    for (const QLineEdit *edit : m_edits) {
        result.append(edit->text());
    }
    return result;
}

bool KbdintPromptDialog::rememberForSession() const
{
    return m_remember->isChecked();
}

} // namespace hssh

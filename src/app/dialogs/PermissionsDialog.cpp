#include "PermissionsDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QVBoxLayout>

namespace hssh {

PermissionsDialog::PermissionsDialog(const QString &remotePath, const SftpFileInfo &info,
                                     QWidget *parent)
    : QDialog(parent)
    , m_remotePath(remotePath)
    , m_isDir(info.isDir)
{
    setWindowTitle(tr("Permissions — %1").arg(info.name));
    setObjectName(QStringLiteral("permissionsDialog"));

    auto *rootLayout = new QVBoxLayout(this);
    auto *form = new QFormLayout;

    auto *pathLabel = new QLabel(m_remotePath, this);
    pathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(tr("Path:"), pathLabel);

    // Owner/group: display only (chown needs root; sudo-gated feature later).
    m_ownerLabel = new QLabel(
        info.owner.isEmpty() ? tr("unknown") : QStringLiteral("%1:%2").arg(info.owner, info.group),
        this);
    form->addRow(tr("Owner:"), m_ownerLabel);
    rootLayout->addLayout(form);

    // r/w/x grid: rows = who, columns = rwx.
    auto *grid = new QGridLayout;
    grid->addWidget(new QLabel(QString(), this), 0, 0);
    grid->addWidget(new QLabel(tr("Read"), this), 0, 1, Qt::AlignCenter);
    grid->addWidget(new QLabel(tr("Write"), this), 0, 2, Qt::AlignCenter);
    grid->addWidget(new QLabel(tr("Execute"), this), 0, 3, Qt::AlignCenter);
    const QStringList who = {tr("Owner"), tr("Group"), tr("Others")};
    for (int w = 0; w < 3; ++w) {
        grid->addWidget(new QLabel(who.at(w), this), w + 1, 0);
        for (int b = 0; b < 3; ++b) {
            m_boxes[w][b] = new QCheckBox(this);
            m_boxes[w][b]->setToolTip(
                QStringLiteral("%1 %2").arg(who.at(w),
                                            b == 0 ? tr("read") : b == 1 ? tr("write") : tr("execute")));
            grid->addWidget(m_boxes[w][b], w + 1, b + 1, Qt::AlignCenter);
            connect(m_boxes[w][b], &QCheckBox::toggled, this,
                    &PermissionsDialog::syncFromCheckBoxes);
        }
    }
    rootLayout->addLayout(grid);

    auto *octalRow = new QFormLayout;
    m_octalEdit = new QLineEdit(this);
    m_octalEdit->setMaxLength(3);
    m_octalEdit->setToolTip(tr("Octal notation, e.g. 644 or 755."));
    octalRow->addRow(tr("Octal:"), m_octalEdit);
    m_symbolicLabel = new QLabel(this);
    octalRow->addRow(tr("Symbolic:"), m_symbolicLabel);
    rootLayout->addLayout(octalRow);

    connect(m_octalEdit, &QLineEdit::textEdited, this, &PermissionsDialog::syncFromOctalEdit);

    m_recursiveCheck = new QCheckBox(tr("Apply recursively to all contents"), this);
    m_recursiveCheck->setVisible(m_isDir);
    rootLayout->addWidget(m_recursiveCheck);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this, buttons]() {
        quint32 mode = 0;
        if (!parseOctal(m_octalEdit->text(), &mode)) {
            return; // incomplete input; the box is validated on every edit
        }
        emit applied(mode, m_isDir && m_recursiveCheck->isChecked());
        buttons->button(QDialogButtonBox::Apply)->setEnabled(false);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    rootLayout->addWidget(buttons);

    setCurrentMode(info.permissions & 0777);
}

bool PermissionsDialog::parseOctal(const QString &text, quint32 *mode)
{
    static const QRegularExpression rx(QStringLiteral("^[0-7]{3}$"));
    if (!rx.match(text.trimmed()).hasMatch()) {
        return false;
    }
    bool ok = false;
    const quint32 value = text.trimmed().toUInt(&ok, 8);
    if (!ok || value > 0777) {
        return false;
    }
    if (mode) {
        *mode = value;
    }
    return true;
}

QString PermissionsDialog::symbolicFromMode(quint32 mode)
{
    QString text;
    for (int shift = 6; shift >= 0; shift -= 3) {
        const quint32 bits = (mode >> shift) & 7;
        text += QLatin1Char((bits & 4) ? 'r' : '-');
        text += QLatin1Char((bits & 2) ? 'w' : '-');
        text += QLatin1Char((bits & 1) ? 'x' : '-');
    }
    return text;
}

quint32 PermissionsDialog::currentMode() const
{
    quint32 mode = 0;
    for (int w = 0; w < 3; ++w) {
        quint32 bits = 0;
        if (m_boxes[w][0] && m_boxes[w][0]->isChecked()) bits |= 4;
        if (m_boxes[w][1] && m_boxes[w][1]->isChecked()) bits |= 2;
        if (m_boxes[w][2] && m_boxes[w][2]->isChecked()) bits |= 1;
        mode |= bits << ((2 - w) * 3);
    }
    return mode;
}

void PermissionsDialog::setCurrentMode(quint32 mode)
{
    m_syncing = true;
    for (int w = 0; w < 3; ++w) {
        const quint32 bits = (mode >> ((2 - w) * 3)) & 7;
        if (m_boxes[w][0]) m_boxes[w][0]->setChecked(bits & 4);
        if (m_boxes[w][1]) m_boxes[w][1]->setChecked(bits & 2);
        if (m_boxes[w][2]) m_boxes[w][2]->setChecked(bits & 1);
    }
    m_octalEdit->setText(QStringLiteral("%1").arg(mode, 3, 8, QLatin1Char('0')));
    m_symbolicLabel->setText(symbolicFromMode(mode));
    m_syncing = false;
}

void PermissionsDialog::syncFromCheckBoxes()
{
    if (m_syncing) {
        return;
    }
    m_syncing = true;
    const quint32 mode = currentMode();
    m_octalEdit->setText(QStringLiteral("%1").arg(mode, 3, 8, QLatin1Char('0')));
    m_symbolicLabel->setText(symbolicFromMode(mode));
    m_syncing = false;
}

void PermissionsDialog::syncFromOctalEdit()
{
    if (m_syncing) {
        return;
    }
    quint32 mode = 0;
    if (!parseOctal(m_octalEdit->text(), &mode)) {
        return; // wait for a complete 3-digit octal before re-syncing
    }
    m_syncing = true;
    for (int w = 0; w < 3; ++w) {
        const quint32 bits = (mode >> ((2 - w) * 3)) & 7;
        if (m_boxes[w][0]) m_boxes[w][0]->setChecked(bits & 4);
        if (m_boxes[w][1]) m_boxes[w][1]->setChecked(bits & 2);
        if (m_boxes[w][2]) m_boxes[w][2]->setChecked(bits & 1);
    }
    m_symbolicLabel->setText(symbolicFromMode(mode));
    m_syncing = false;
}

} // namespace hssh

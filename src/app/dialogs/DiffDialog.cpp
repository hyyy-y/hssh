#include "DiffDialog.h"

#include "app/widgets/DiffView.h"

#include <QDialogButtonBox>
#include <QVBoxLayout>

namespace hssh {

DiffDialog::DiffDialog(const QString &leftTitle, const QString &leftText,
                       const QString &rightTitle, const QString &rightText,
                       QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Compare files"));
    resize(1000, 640);
    setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(this);
    auto *view = new DiffView(this);
    view->setContent(leftTitle, leftText, rightTitle, rightText);
    layout->addWidget(view, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

} // namespace hssh

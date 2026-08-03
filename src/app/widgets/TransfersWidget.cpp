#include "TransfersWidget.h"

#include "app/TransferRegistry.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QStandardItemModel>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace hssh {

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
}

TransfersWidget::TransfersWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    auto *buttonBar = new QHBoxLayout;
    auto *clearButton = new QToolButton(this);
    clearButton->setText(tr("✕"));
    clearButton->setToolTip(tr("Clear finished transfers"));
    buttonBar->addStretch(1);
    buttonBar->addWidget(clearButton);
    layout->addLayout(buttonBar);

    m_model = new QStandardItemModel(0, 5, this);
    m_model->setHorizontalHeaderLabels({tr("Name"), tr("Direction"), tr("Session"), tr("Progress"), tr("Status")});

    m_view = new QTableView(this);
    m_view->setModel(m_model);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setShowGrid(false);
    m_view->verticalHeader()->setVisible(false);
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    layout->addWidget(m_view, 1);

    connect(clearButton, &QToolButton::clicked, this, &TransfersWidget::clearFinished);

    TransferRegistry &registry = TransferRegistry::instance();
    connect(&registry, &TransferRegistry::transferAdded, this, &TransfersWidget::onTransferAdded);
    connect(&registry, &TransferRegistry::transferUpdated, this, &TransfersWidget::onTransferUpdated);
}

TransfersWidget::~TransfersWidget() = default;

int TransfersWidget::rowForId(int id) const
{
    for (int row = 0; row < m_model->rowCount(); ++row) {
        if (m_model->item(row, 0)->data(kIdRole).toInt() == id) {
            return row;
        }
    }
    return -1;
}

void TransfersWidget::onTransferAdded(int id)
{
    const TransferRegistry::Transfer *transfer = TransferRegistry::instance().transfer(id);
    if (!transfer) {
        return;
    }
    auto *nameItem = new QStandardItem(transfer->name);
    nameItem->setData(id, kIdRole);
    auto *directionItem = new QStandardItem(
        transfer->direction == TransferRegistry::Direction::Upload ? tr("Upload") : tr("Download"));
    auto *sessionItem = new QStandardItem(transfer->sessionName);
    auto *progressItem = new QStandardItem;
    auto *statusItem = new QStandardItem(tr("Transferring…"));
    m_model->appendRow({nameItem, directionItem, sessionItem, progressItem, statusItem});
    onTransferUpdated(id);
}

void TransfersWidget::onTransferUpdated(int id)
{
    const TransferRegistry::Transfer *transfer = TransferRegistry::instance().transfer(id);
    const int row = rowForId(id);
    if (!transfer || row < 0) {
        return;
    }

    const QLocale locale;
    QString progressText;
    if (transfer->bytesTotal > 0) {
        progressText = QStringLiteral("%1 / %2 (%3%)")
                           .arg(locale.formattedDataSize(transfer->bytesDone),
                                locale.formattedDataSize(transfer->bytesTotal))
                           .arg(transfer->bytesDone * 100 / transfer->bytesTotal);
    } else {
        progressText = locale.formattedDataSize(transfer->bytesDone);
    }
    m_model->item(row, 3)->setText(progressText);

    if (transfer->finished) {
        if (transfer->ok) {
            m_model->item(row, 4)->setText(tr("Done"));
            m_model->item(row, 4)->setForeground(QColor(0x4e, 0x9a, 0x51));
        } else {
            m_model->item(row, 4)->setText(tr("Failed: %1").arg(transfer->message));
            m_model->item(row, 4)->setForeground(QColor(0xe0, 0x56, 0x56));
        }
    }
}

void TransfersWidget::clearFinished()
{
    for (int row = m_model->rowCount() - 1; row >= 0; --row) {
        const int id = m_model->item(row, 0)->data(kIdRole).toInt();
        const TransferRegistry::Transfer *transfer = TransferRegistry::instance().transfer(id);
        if (transfer && transfer->finished) {
            m_model->removeRow(row);
        }
    }
}

} // namespace hssh

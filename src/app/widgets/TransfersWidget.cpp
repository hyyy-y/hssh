#include "TransfersWidget.h"

#include "app/TransferRegistry.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
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

    // Overall progress across all transfers.
    auto *summaryRow = new QHBoxLayout;
    m_summaryLabel = new QLabel(tr("No transfers"), this);
    m_overallBar = new QProgressBar(this);
    m_overallBar->setRange(0, 100);
    m_overallBar->setValue(0);
    m_overallBar->setMaximumHeight(14);
    summaryRow->addWidget(m_summaryLabel);
    summaryRow->addWidget(m_overallBar, 1);
    layout->addLayout(summaryRow);

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
    return m_rowById.value(id, -1);
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
    m_rowById.insert(id, m_model->rowCount());
    m_model->appendRow({nameItem, directionItem, sessionItem, progressItem, statusItem});

    ++m_activeCount;
    refreshSummary();
    onTransferUpdated(id);
}

void TransfersWidget::onTransferUpdated(int id)
{
    const TransferRegistry::Transfer *transfer = TransferRegistry::instance().transfer(id);
    const int row = rowForId(id);
    if (!transfer || row < 0) {
        return;
    }

    // Incremental byte accounting for the overall bar.
    AggState &st = m_agg[id];
    if (transfer->bytesDone > st.done) {
        m_totalDone += transfer->bytesDone - st.done;
        st.done = transfer->bytesDone;
    }
    if (transfer->bytesTotal > 0 && st.total == 0) {
        st.total = transfer->bytesTotal;
        m_totalBytes += st.total;
    }
    if (transfer->finished && !st.countedFinished) {
        st.countedFinished = true;
        --m_activeCount;
        ++m_finishedCount;
        if (!transfer->ok) {
            ++m_failedCount;
        }
    }
    refreshSummary();

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

void TransfersWidget::refreshSummary()
{
    if (m_activeCount == 0 && m_finishedCount == 0) {
        m_summaryLabel->setText(tr("No transfers"));
        m_overallBar->setValue(0);
        return;
    }
    m_summaryLabel->setText(tr("%1 active · %2 done · %3 failed")
                                .arg(m_activeCount)
                                .arg(m_finishedCount - m_failedCount)
                                .arg(m_failedCount));
    if (m_totalBytes > 0) {
        m_overallBar->setValue(static_cast<int>(m_totalDone * 100 / m_totalBytes));
        m_overallBar->setFormat(QStringLiteral("%1 / %2 (%p%)")
                                    .arg(QLocale().formattedDataSize(m_totalDone),
                                         QLocale().formattedDataSize(m_totalBytes)));
    } else {
        m_overallBar->setValue(0);
        m_overallBar->setFormat(QStringLiteral("%p%"));
    }
}

void TransfersWidget::clearFinished()
{
    for (int row = m_model->rowCount() - 1; row >= 0; --row) {
        const int id = m_model->item(row, 0)->data(kIdRole).toInt();
        const TransferRegistry::Transfer *transfer = TransferRegistry::instance().transfer(id);
        if (transfer && transfer->finished) {
            m_agg.remove(id);
            m_rowById.remove(id);
            m_model->removeRow(row);
        }
    }
    // Rows shifted after removals; rebuild the id map.
    m_rowById.clear();
    for (int row = 0; row < m_model->rowCount(); ++row) {
        m_rowById.insert(m_model->item(row, 0)->data(kIdRole).toInt(), row);
    }
    // Recount aggregates from what remains displayed.
    m_totalDone = 0;
    m_totalBytes = 0;
    m_activeCount = 0;
    m_finishedCount = 0;
    m_failedCount = 0;
    for (auto it = m_agg.cbegin(); it != m_agg.cend(); ++it) {
        m_totalDone += it->done;
        m_totalBytes += it->total;
        if (it->countedFinished) {
            ++m_finishedCount;
        } else {
            ++m_activeCount;
        }
    }
    // Recompute failed count from the registry entries still shown.
    m_failedCount = 0;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const int id = m_model->item(row, 0)->data(kIdRole).toInt();
        const TransferRegistry::Transfer *transfer = TransferRegistry::instance().transfer(id);
        if (transfer && transfer->finished && !transfer->ok) {
            ++m_failedCount;
        }
    }
    refreshSummary();
}

} // namespace hssh

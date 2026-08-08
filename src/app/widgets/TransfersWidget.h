#ifndef HSSH_APP_WIDGETS_TRANSFERSWIDGET_H
#define HSSH_APP_WIDGETS_TRANSFERSWIDGET_H

#include <QHash>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QProgressBar;
class QStandardItemModel;
class QTableView;
class QToolButton;
QT_END_NAMESPACE

namespace hssh {

// Bottom-dock panel listing recent/active file transfers reported to
// TransferRegistry.
class TransfersWidget : public QWidget {
    Q_OBJECT

public:
    explicit TransfersWidget(QWidget *parent = nullptr);
    ~TransfersWidget() override;

private:
    void onTransferAdded(int id);
    void onTransferUpdated(int id);
    void clearFinished();
    int rowForId(int id) const;
    void refreshSummary();

    QTableView *m_view = nullptr;
    QStandardItemModel *m_model = nullptr;
    QHash<int, int> m_rowById; // transfer id -> model row (O(1) lookups)

    // Overall progress across every transfer (incremental aggregates).
    QProgressBar *m_overallBar = nullptr;
    QLabel *m_summaryLabel = nullptr;
    struct AggState {
        qint64 done = 0;
        qint64 total = 0;
        bool countedFinished = false;
    };
    QHash<int, AggState> m_agg; // per-transfer accounting for the summary bar
    qint64 m_totalDone = 0;
    qint64 m_totalBytes = 0;
    int m_activeCount = 0;
    int m_failedCount = 0;
    int m_finishedCount = 0;
};

} // namespace hssh

#endif // HSSH_APP_WIDGETS_TRANSFERSWIDGET_H

#ifndef HSSH_APP_WIDGETS_TRANSFERSWIDGET_H
#define HSSH_APP_WIDGETS_TRANSFERSWIDGET_H

#include <QWidget>

QT_BEGIN_NAMESPACE
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
    [[nodiscard]] int rowForId(int id) const;

    QTableView *m_view = nullptr;
    QStandardItemModel *m_model = nullptr;
};

} // namespace hssh

#endif // HSSH_APP_WIDGETS_TRANSFERSWIDGET_H

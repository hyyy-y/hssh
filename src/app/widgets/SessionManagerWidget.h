#ifndef HSSH_APP_WIDGETS_SESSIONMANAGERWIDGET_H
#define HSSH_APP_WIDGETS_SESSIONMANAGERWIDGET_H

#include "core/SessionConfig.h"
#include "core/SessionModel.h"

#include <QWidget>

class QLineEdit;
class QTreeView;

namespace hssh {

class SessionManagerWidget : public QWidget {
    Q_OBJECT

public:
    explicit SessionManagerWidget(QWidget *parent = nullptr);
    ~SessionManagerWidget() override;

    void setModel(SessionModel *model);
    [[nodiscard]] SessionModel *model() const;

    [[nodiscard]] QModelIndex currentIndex() const;

signals:
    void sessionActivated(const SessionConfig &config);
    void newSessionRequested();
    void newFolderRequested();
    void editRequested(const QModelIndex &index);
    void removeRequested(const QModelIndex &index);

private:
    void onDoubleClicked(const QModelIndex &index);
    void onContextMenu(const QPoint &pos);
    void onSearchTextChanged(const QString &text);

    QTreeView *m_treeView = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    SessionModel *m_model = nullptr;
};

} // namespace hssh

#endif // HSSH_APP_WIDGETS_SESSIONMANAGERWIDGET_H

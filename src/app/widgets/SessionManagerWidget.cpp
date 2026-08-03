#include "SessionManagerWidget.h"

#include <QAction>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTreeView>
#include <QVBoxLayout>

namespace hssh {

SessionManagerWidget::SessionManagerWidget(QWidget *parent)
    : QWidget(parent)
    , m_model(new SessionModel(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Search sessions..."));
    layout->addWidget(m_searchEdit);

    m_treeView = new QTreeView(this);
    m_treeView->setHeaderHidden(true);
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_treeView->setModel(m_model);
    layout->addWidget(m_treeView);

    connect(m_treeView, &QTreeView::doubleClicked, this, &SessionManagerWidget::onDoubleClicked);
    connect(m_treeView, &QTreeView::customContextMenuRequested, this, &SessionManagerWidget::onContextMenu);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &SessionManagerWidget::onSearchTextChanged);
}

SessionManagerWidget::~SessionManagerWidget() = default;

void SessionManagerWidget::setModel(SessionModel *model)
{
    if (m_model == model) {
        return;
    }

    if (m_model && m_model->QObject::parent() == this) {
        m_model->deleteLater();
    }

    m_model = model ? model : new SessionModel(this);
    m_treeView->setModel(m_model);
}

SessionModel *SessionManagerWidget::model() const
{
    return m_model;
}

QModelIndex SessionManagerWidget::currentIndex() const
{
    return m_treeView->currentIndex();
}

void SessionManagerWidget::onDoubleClicked(const QModelIndex &index)
{
    if (!index.isValid() || !m_model) {
        return;
    }

    if (m_model->nodeType(index) == SessionModel::NodeType::Session) {
        emit sessionActivated(m_model->sessionConfig(index));
    }
}

void SessionManagerWidget::onContextMenu(const QPoint &pos)
{
    const QModelIndex index = m_treeView->indexAt(pos);
    QMenu menu(this);

    menu.addAction(tr("New Session"), this, [this]() {
        emit newSessionRequested();
    });
    menu.addAction(tr("New Folder"), this, [this]() {
        emit newFolderRequested();
    });

    if (index.isValid()) {
        menu.addSeparator();
        menu.addAction(tr("Edit"), this, [this, index]() {
            emit editRequested(index);
        });
        menu.addAction(tr("Remove"), this, [this, index]() {
            emit removeRequested(index);
        });
    }

    menu.exec(m_treeView->mapToGlobal(pos));
}

void SessionManagerWidget::onSearchTextChanged(const QString &text)
{
    if (!m_model) {
        return;
    }

    if (text.isEmpty()) {
        for (int i = 0; i < m_model->rowCount({}); ++i) {
            const QModelIndex groupIndex = m_model->index(i, 0, {});
            m_treeView->expand(groupIndex);
        }
        return;
    }

    // TODO: implement real filter proxy model in a later phase
    Q_UNUSED(text)
}

} // namespace hssh

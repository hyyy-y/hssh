#include "SessionManagerWidget.h"

#include <QAction>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QVBoxLayout>

namespace hssh {

namespace {

// Filters the session tree by name/host/username; a folder row stays visible
// when it (or any descendant) matches.
class SessionFilterProxy : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        const QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);
        if (!index.isValid()) {
            return false;
        }

        const QString text = filterRegularExpression().pattern();
        if (text.isEmpty()) {
            return true;
        }

        const QString display = sourceModel()->data(index, Qt::DisplayRole).toString();
        if (display.contains(text, Qt::CaseInsensitive)) {
            return true;
        }

        const QVariant configVariant =
            sourceModel()->data(index, static_cast<int>(SessionModel::Role::SessionConfigRole));
        if (configVariant.isValid()) {
            const SessionConfig config = qvariant_cast<SessionConfig>(configVariant);
            if (config.host().contains(text, Qt::CaseInsensitive)
                || config.username().contains(text, Qt::CaseInsensitive)) {
                return true;
            }
        }

        for (int row = 0; row < sourceModel()->rowCount(index); ++row) {
            if (filterAcceptsRow(row, index)) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

SessionManagerWidget::SessionManagerWidget(QWidget *parent)
    : QWidget(parent)
    , m_model(new SessionModel(this))
    , m_proxy(new SessionFilterProxy(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Search sessions..."));
    m_searchEdit->setClearButtonEnabled(true);
    layout->addWidget(m_searchEdit);

    m_proxy->setSourceModel(m_model);
    m_proxy->setRecursiveFilteringEnabled(false);

    m_treeView = new QTreeView(this);
    m_treeView->setHeaderHidden(true);
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_treeView->setModel(m_proxy);
    layout->addWidget(m_treeView);

    connect(m_treeView, &QTreeView::doubleClicked, this, &SessionManagerWidget::onDoubleClicked);
    connect(m_treeView, &QTreeView::customContextMenuRequested, this, &SessionManagerWidget::onContextMenu);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &SessionManagerWidget::onSearchTextChanged);
    connect(m_proxy, &QSortFilterProxyModel::layoutChanged, this, [this]() {
        m_treeView->expandAll();
    });
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
    m_proxy->setSourceModel(m_model);
}

SessionModel *SessionManagerWidget::model() const
{
    return m_model;
}

QModelIndex SessionManagerWidget::currentIndex() const
{
    const QModelIndex proxyIndex = m_treeView->currentIndex();
    if (!proxyIndex.isValid() || !m_proxy) {
        return {};
    }
    return m_proxy->mapToSource(proxyIndex);
}

void SessionManagerWidget::onDoubleClicked(const QModelIndex &index)
{
    if (!index.isValid() || !m_model) {
        return;
    }

    // The view is driven by the filter proxy; map before touching the model.
    const QModelIndex sourceIndex = m_proxy->mapToSource(index);
    if (m_model->nodeType(sourceIndex) == SessionModel::NodeType::Session) {
        emit sessionActivated(m_model->sessionConfig(sourceIndex));
    }
}

void SessionManagerWidget::onContextMenu(const QPoint &pos)
{
    const QModelIndex sourceIndex = m_proxy->mapToSource(m_treeView->indexAt(pos));
    QMenu menu(this);

    menu.addAction(tr("New Session"), this, [this]() {
        emit newSessionRequested();
    });
    menu.addAction(tr("New Folder"), this, [this]() {
        emit newFolderRequested();
    });

    if (sourceIndex.isValid()) {
        menu.addSeparator();
        menu.addAction(tr("Edit"), this, [this, sourceIndex]() {
            emit editRequested(sourceIndex);
        });
        menu.addAction(tr("Remove"), this, [this, sourceIndex]() {
            emit removeRequested(sourceIndex);
        });
    }

    menu.exec(m_treeView->mapToGlobal(pos));
}

void SessionManagerWidget::onSearchTextChanged(const QString &text)
{
    if (!m_proxy) {
        return;
    }

    m_proxy->setFilterRegularExpression(QRegularExpression::escape(text));
    if (text.isEmpty()) {
        m_treeView->expandAll();
    } else {
        m_treeView->expandAll();
    }
}

} // namespace hssh

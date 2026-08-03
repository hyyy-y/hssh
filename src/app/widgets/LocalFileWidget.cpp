#include "LocalFileWidget.h"

#include <QDir>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QStandardPaths>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace hssh {

// Sorts directories before files and overlays compare-highlight colors on
// the name column (keyed by file name).
class LocalFileProxyModel : public QSortFilterProxyModel {
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setRowColors(const QHash<QString, QColor> &colors)
    {
        m_rowColors = colors;
        if (rowCount() > 0) {
            emit dataChanged(index(0, 0), index(rowCount() - 1, columnCount() - 1),
                             {Qt::ForegroundRole});
        }
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (role == Qt::ForegroundRole && !m_rowColors.isEmpty()) {
            const QModelIndex nameIndex = this->index(index.row(), 0, index.parent());
            const auto it = m_rowColors.constFind(nameIndex.data().toString());
            if (it != m_rowColors.constEnd()) {
                return it.value();
            }
        }
        return QSortFilterProxyModel::data(index, role);
    }

protected:
    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        auto *fsModel = qobject_cast<QFileSystemModel *>(sourceModel());
        if (fsModel) {
            const bool leftDir = fsModel->isDir(left);
            const bool rightDir = fsModel->isDir(right);
            if (leftDir != rightDir) {
                return leftDir;
            }
        }
        return QSortFilterProxyModel::lessThan(left, right);
    }

private:
    QHash<QString, QColor> m_rowColors;
};

LocalFileWidget::LocalFileWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(2);

    // Navigation bar.
    auto *navBar = new QHBoxLayout;
    const auto makeButton = [this](const QString &text, const QString &tooltip) {
        auto *button = new QToolButton(this);
        button->setText(text);
        button->setToolTip(tooltip);
        return button;
    };
    QToolButton *upButton = makeButton(QStringLiteral("↑"), tr("Up one directory"));
    QToolButton *homeButton = makeButton(QStringLiteral("⌂"), tr("Home directory"));
    QToolButton *refreshButton = makeButton(QStringLiteral("⟳"), tr("Refresh"));
    m_pathEdit = new QLineEdit(this);
    m_pathEdit->setPlaceholderText(tr("Local path"));

    navBar->addWidget(upButton);
    navBar->addWidget(homeButton);
    navBar->addWidget(refreshButton);
    navBar->addWidget(m_pathEdit, 1);
    layout->addLayout(navBar);

    connect(upButton, &QToolButton::clicked, this, &LocalFileWidget::goUp);
    connect(homeButton, &QToolButton::clicked, this, &LocalFileWidget::goHome);
    connect(refreshButton, &QToolButton::clicked, this, &LocalFileWidget::refresh);
    connect(m_pathEdit, &QLineEdit::returnPressed, this, [this]() {
        navigateTo(m_pathEdit->text().trimmed());
    });

    // File listing.
    m_fsModel = new QFileSystemModel(this);
    m_fsModel->setReadOnly(true);
    m_fsModel->setFilter(QDir::AllEntries | QDir::NoDot);

    m_proxy = new LocalFileProxyModel(this);
    m_proxy->setSourceModel(m_fsModel);
    m_proxy->setSortCaseSensitivity(Qt::CaseInsensitive);

    m_view = new QTableView(this);
    m_view->setModel(m_proxy);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setShowGrid(false);
    m_view->verticalHeader()->setVisible(false);
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_view->setSortingEnabled(true);
    m_view->sortByColumn(0, Qt::AscendingOrder);
    layout->addWidget(m_view, 1);

    connect(m_view, &QTableView::doubleClicked, this, &LocalFileWidget::onActivated);

    navigateTo(QDir::homePath());
}

LocalFileWidget::~LocalFileWidget() = default;

void LocalFileWidget::navigateTo(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }
    const QFileInfo info(path);
    if (!info.isDir()) {
        return;
    }
    const QString clean = QDir::cleanPath(path);
    const QModelIndex sourceRoot = m_fsModel->setRootPath(clean);
    m_view->setRootIndex(m_proxy->mapFromSource(sourceRoot));
    if (m_currentPath != clean) {
        m_currentPath = clean;
        m_pathEdit->setText(QDir::toNativeSeparators(clean));
        emit pathChanged(m_currentPath);
    } else {
        m_pathEdit->setText(QDir::toNativeSeparators(clean));
    }
}

void LocalFileWidget::refresh()
{
    // QFileSystemModel watches the directory itself; re-set the root to force
    // a reload (e.g. after a download landed here).
    if (!m_currentPath.isEmpty()) {
        const QModelIndex sourceRoot = m_fsModel->setRootPath(m_currentPath);
        m_view->setRootIndex(m_proxy->mapFromSource(sourceRoot));
    }
}

QStringList LocalFileWidget::selectedPaths() const
{
    QStringList result;
    const QModelIndex root = m_view->rootIndex();
    const QModelIndexList rows = m_view->selectionModel()->selectedRows();
    for (const QModelIndex &proxyIndex : rows) {
        const QModelIndex sourceIndex = m_proxy->mapToSource(proxyIndex);
        if (sourceIndex.isValid()) {
            result.append(m_fsModel->filePath(sourceIndex));
        }
    }
    Q_UNUSED(root)
    return result;
}

void LocalFileWidget::setRowColors(const QHash<QString, QColor> &colors)
{
    m_proxy->setRowColors(colors);
}

void LocalFileWidget::clearRowColors()
{
    m_proxy->setRowColors({});
}

void LocalFileWidget::goUp()
{
    if (m_currentPath.isEmpty()) {
        return;
    }
    const QDir dir(m_currentPath);
    if (dir.isRoot()) {
        return;
    }
    QDir parent(dir);
    parent.cdUp();
    navigateTo(parent.absolutePath());
}

void LocalFileWidget::goHome()
{
    navigateTo(QDir::homePath());
}

void LocalFileWidget::onActivated(const QModelIndex &proxyIndex)
{
    const QModelIndex sourceIndex = m_proxy->mapToSource(proxyIndex);
    if (!sourceIndex.isValid()) {
        return;
    }
    if (m_fsModel->isDir(sourceIndex)) {
        navigateTo(m_fsModel->filePath(sourceIndex));
    } else {
        emit fileActivated(m_fsModel->filePath(sourceIndex));
    }
}

} // namespace hssh

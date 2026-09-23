#include "RemoteSearchDialog.h"

#include "app/widgets/SftpWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace hssh {

namespace {

// Guard against pathological trees ("*" over a million files): rendering
// more rows than this buys nothing but minutes of table population.
constexpr int kMaxResultRows = 50000;

} // namespace

RemoteSearchDialog::RemoteSearchDialog(SftpWidget *remote, QWidget *parent)
    : QDialog(parent)
    , m_remote(remote)
{
    setWindowTitle(tr("Search Remote Files"));
    setObjectName(QStringLiteral("remoteSearchDialog"));
    resize(860, 560);

    auto *rootLayout = new QVBoxLayout(this);

    auto *form = new QFormLayout;
    m_rootEdit = new QLineEdit(m_remote->currentPath(), this);
    m_rootEdit->setToolTip(tr("Directory the recursive search starts from."));
    form->addRow(tr("Search in:"), m_rootEdit);

    auto *nameRow = new QHBoxLayout;
    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(tr("*.log, data*.txt, build/src (empty = all files)"));
    m_nameEdit->setToolTip(tr("Wildcards * and ?; case-insensitive. A pattern with '/' "
                              "matches the relative path instead of the file name."));
    nameRow->addWidget(m_nameEdit, 1);
    form->addRow(tr("Name:"), nameRow);

    auto *sizeRow = new QHBoxLayout;
    m_sizeModeBox = new QComboBox(this);
    m_sizeModeBox->addItem(tr("Any size"));
    m_sizeModeBox->addItem(tr("At least"));
    m_sizeModeBox->addItem(tr("At most"));
    m_sizeSpin = new QSpinBox(this);
    m_sizeSpin->setRange(1, 1000000);
    m_sizeUnitBox = new QComboBox(this);
    m_sizeUnitBox->addItem(tr("KB"));
    m_sizeUnitBox->addItem(tr("MB"));
    m_sizeUnitBox->addItem(tr("GB"));
    sizeRow->addWidget(m_sizeModeBox);
    sizeRow->addWidget(m_sizeSpin);
    sizeRow->addWidget(m_sizeUnitBox);
    sizeRow->addStretch(1);
    form->addRow(tr("Size:"), sizeRow);

    auto *timeRow = new QHBoxLayout;
    m_timeCheck = new QCheckBox(tr("Modified within"), this);
    m_daysSpin = new QSpinBox(this);
    m_daysSpin->setRange(1, 3650);
    m_daysSpin->setValue(7);
    timeRow->addWidget(m_timeCheck);
    timeRow->addWidget(m_daysSpin);
    timeRow->addWidget(new QLabel(tr("days"), this));
    timeRow->addStretch(1);
    form->addRow(QString(), timeRow);

    auto *depthRow = new QHBoxLayout;
    m_depthSpin = new QSpinBox(this);
    m_depthSpin->setRange(0, 20);
    m_depthSpin->setValue(3);
    m_depthSpin->setSpecialValueText(tr("Unlimited"));
    m_depthSpin->setToolTip(tr("Maximum directory levels to descend from the search "
                               "root. 0 walks the whole tree."));
    depthRow->addWidget(m_depthSpin);
    depthRow->addStretch(1);
    form->addRow(tr("Max depth:"), depthRow);
    rootLayout->addLayout(form);

    auto *buttonRow = new QHBoxLayout;
    m_searchButton = new QPushButton(tr("Search"), this);
    m_searchButton->setDefault(true);
    m_cancelButton = new QPushButton(tr("Cancel"), this);
    m_cancelButton->setEnabled(false);
    buttonRow->addStretch(1);
    buttonRow->addWidget(m_cancelButton);
    buttonRow->addWidget(m_searchButton);
    rootLayout->addLayout(buttonRow);

    m_results = new QTableWidget(0, 4, this);
    m_results->setHorizontalHeaderLabels({tr("Name"), tr("Folder"), tr("Size"), tr("Modified")});
    m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results->setSortingEnabled(true);
    m_results->horizontalHeader()->setStretchLastSection(true);
    m_results->verticalHeader()->setVisible(false);
    m_results->setAlternatingRowColors(true);
    rootLayout->addWidget(m_results, 1);

    m_statusLabel = new QLabel(this);
    rootLayout->addWidget(m_statusLabel);

    connect(m_searchButton, &QPushButton::clicked, this, &RemoteSearchDialog::startSearch);
    connect(m_cancelButton, &QPushButton::clicked, this, &RemoteSearchDialog::cancelSearch);
    connect(m_results, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int column) { onResultActivated(row, column); });
    // The SFTP widget's signals carry EVERY walk (folder compare listens to
    // the same pair) — only results for the root this dialog asked about,
    // while waiting for them, are ours.
    connect(m_remote, &SftpWidget::dirTreeListed, this, &RemoteSearchDialog::onWalkListed);
    connect(m_remote, &SftpWidget::dirTreeProgress, this, &RemoteSearchDialog::onWalkProgress);
}

QList<RemoteFileEntry> RemoteSearchDialog::filterEntries(
    const QList<RemoteFileEntry> &entries, const Criteria &criteria, qint64 nowSecs)
{
    QRegularExpression nameRx;
    if (!criteria.namePattern.isEmpty()) {
        // Wildcards, case-insensitive. A '/' in the pattern addresses the
        // relative path ("build/src/*.o"), otherwise the plain file name.
        nameRx = QRegularExpression(
            QRegularExpression::wildcardToRegularExpression(criteria.namePattern),
            QRegularExpression::CaseInsensitiveOption);
        if (!nameRx.isValid()) {
            return {};
        }
    }

    const qint64 timeFloor = criteria.timeEnabled
                                 ? nowSecs - qint64(criteria.days) * 86400
                                 : 0;

    QList<RemoteFileEntry> out;
    for (const RemoteFileEntry &entry : entries) {
        if (entry.isDir) {
            // Directories carry no meaningful size; only name/time can match.
            if (criteria.sizeMode != 0) {
                continue;
            }
        } else if (criteria.sizeMode == 1 && entry.size < criteria.sizeBytes) {
            continue;
        } else if (criteria.sizeMode == 2 && entry.size > criteria.sizeBytes) {
            continue;
        }

        if (criteria.timeEnabled && entry.mtime < timeFloor) {
            continue;
        }

        if (!nameRx.pattern().isEmpty()) {
            const QString subject = criteria.namePattern.contains(QLatin1Char('/'))
                                        ? entry.relPath
                                        : entry.relPath.section(QLatin1Char('/'), -1);
            if (!nameRx.match(subject).hasMatch()) {
                continue;
            }
        }

        out.append(entry);
        if (out.size() >= kMaxResultRows) {
            break;
        }
    }
    return out;
}

RemoteSearchDialog::Criteria RemoteSearchDialog::currentCriteria() const
{
    Criteria c;
    c.namePattern = m_nameEdit->text().trimmed();
    c.sizeMode = m_sizeModeBox->currentIndex();
    static const qint64 units[] = {1024, 1024 * 1024, 1024LL * 1024 * 1024};
    c.sizeBytes = qint64(m_sizeSpin->value()) * units[m_sizeUnitBox->currentIndex()];
    c.timeEnabled = m_timeCheck->isChecked();
    c.days = m_daysSpin->value();
    c.maxDepth = m_depthSpin->value();
    return c;
}

void RemoteSearchDialog::startSearch()
{
    const QString root = m_rootEdit->text().trimmed();
    if (root.isEmpty()) {
        m_statusLabel->setText(tr("Enter a directory to search in."));
        return;
    }

    m_active = true;
    m_activeRoot = root;
    m_searchButton->setEnabled(false);
    m_cancelButton->setEnabled(true);
    m_statusLabel->setText(tr("Scanning %1…").arg(root));
    m_results->setRowCount(0);

    m_remote->listDirTree(root, m_depthSpin->value());
}

void RemoteSearchDialog::cancelSearch()
{
    m_remote->cancelTreeWalk();
    m_active = false;
    m_searchButton->setEnabled(true);
    m_cancelButton->setEnabled(false);
    m_statusLabel->setText(tr("Cancelled."));
}

void RemoteSearchDialog::onWalkListed(const QString &path, const QList<RemoteFileEntry> &entries)
{
    if (!m_active || path != m_activeRoot) {
        return; // a walk started by the compare pane or a different root
    }
    m_active = false;
    m_searchButton->setEnabled(true);
    m_cancelButton->setEnabled(false);

    const QList<RemoteFileEntry> hits = filterEntries(
        entries, currentCriteria(), QDateTime::currentSecsSinceEpoch());

    const QLocale locale;
    m_results->setRowCount(static_cast<int>(hits.size()));
    for (int i = 0; i < hits.size(); ++i) {
        const RemoteFileEntry &entry = hits.at(i);
        const QString folder = entry.relPath.section(QLatin1Char('/'), 0, -2);

        auto *nameItem = new QTableWidgetItem(entry.isDir
                                                  ? QStringLiteral("[DIR] ") + entry.relPath.section(QLatin1Char('/'), -1)
                                                  : entry.relPath.section(QLatin1Char('/'), -1));
        nameItem->setData(Qt::UserRole, entry.remotePath); // navigation target
        nameItem->setData(Qt::UserRole + 1, entry.isDir);
        // Numeric sort keys so "9 KB" sorts before "10 MB".
        auto *sizeItem = new QTableWidgetItem;
        if (entry.isDir) {
            sizeItem->setText(QStringLiteral("—"));
        } else {
            sizeItem->setText(locale.formattedDataSize(entry.size));
            sizeItem->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(entry.size));
        }
        auto *timeItem = new QTableWidgetItem(
            entry.mtime > 0
                ? QDateTime::fromSecsSinceEpoch(entry.mtime).toString(QStringLiteral("yyyy-MM-dd HH:mm"))
                : QString());
        timeItem->setData(Qt::UserRole, QVariant::fromValue<qlonglong>(entry.mtime));

        m_results->setItem(i, 0, nameItem);
        m_results->setItem(i, 1, new QTableWidgetItem(folder));
        m_results->setItem(i, 2, sizeItem);
        m_results->setItem(i, 3, timeItem);
    }

    m_statusLabel->setText(tr("%1 match(es) out of %2 scanned entries.")
                               .arg(hits.size())
                               .arg(entries.size()));
}

void RemoteSearchDialog::onWalkProgress(const QString &path, int scanned)
{
    if (m_active && path == m_activeRoot) {
        m_statusLabel->setText(tr("Scanning %1… %2 entries").arg(path).arg(scanned));
    }
}

void RemoteSearchDialog::onResultActivated(int row, int column)
{
    Q_UNUSED(column);
    QTableWidgetItem *item = m_results->item(row, 0);
    if (!item) {
        return;
    }
    // Locate: a directory opens itself, a file opens its parent folder.
    QString target = item->data(Qt::UserRole).toString();
    if (!item->data(Qt::UserRole + 1).toBool()) {
        const int slash = target.lastIndexOf(QLatin1Char('/'));
        target = slash <= 0 ? QStringLiteral("/") : target.left(slash);
    }
    m_remote->navigateTo(target);
}

} // namespace hssh

#include "CommandPalette.h"

#include "utils/Config.h"

#include <QEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <algorithm>

namespace hssh {

namespace {
constexpr int kMaxRecents = 10;
} // namespace

CommandPalette::CommandPalette(QWidget *parent)
    : QDialog(parent)
{
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_ShowWithoutActivating, false);
    setMinimumWidth(520);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(4);

    m_edit = new QLineEdit(this);
    m_edit->setPlaceholderText(tr("Type a command, session or tab..."));
    m_edit->setClearButtonEnabled(true);
    layout->addWidget(m_edit);

    m_view = new QListView(this);
    m_model = new QStandardItemModel(this);
    m_model->setColumnCount(2);
    m_view->setModel(m_model);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setUniformItemSizes(true);
    m_view->setFrameShape(QFrame::StyledPanel);
    layout->addWidget(m_view);
    setMaximumHeight(420);

    connect(m_edit, &QLineEdit::textChanged, this, &CommandPalette::setFilter);
    connect(m_view, &QListView::activated, this, [this](const QModelIndex &index) {
        runResult(index.row());
    });
    // Route Up/Down/Enter from the edit to the list.
    m_edit->installEventFilter(this);
}

void CommandPalette::setItems(const QList<Item> &items)
{
    m_items = items;
    refilter();
}

void CommandPalette::setFilter(const QString &text)
{
    m_query = text;
    refilter();
}

bool CommandPalette::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Down:
        case Qt::Key_Up: {
            const int delta = key->key() == Qt::Key_Down ? 1 : -1;
            const int row = m_view->currentIndex().row();
            const int next = qBound(0, row + delta, qMax(0, m_filtered.size() - 1));
            if (m_filtered.isEmpty()) {
                return true;
            }
            m_view->setCurrentIndex(m_model->index(next, 0));
            return true;
        }
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (!m_filtered.isEmpty()) {
                const int row = m_view->currentIndex().row();
                runResult(row < 0 ? 0 : row);
            }
            return true;
        default:
            break;
        }
    }
    return QDialog::eventFilter(watched, event);
}

int CommandPalette::fuzzyScore(const QString &text, const QString &query)
{
    if (query.isEmpty()) {
        return 1; // show everything, recents first
    }
    const QString t = text.toLower();
    const QString q = query.toLower();
    int score = 0;
    int ti = 0;
    int streak = 0;
    bool matchedAny = false;
    for (int qi = 0; qi < q.size(); ++qi) {
        if (q.at(qi).isSpace()) {
            continue;
        }
        const int found = t.indexOf(q.at(qi), ti);
        if (found < 0) {
            return -1;
        }
        streak = (found == ti) ? streak + 1 : 1;
        score += 1 + streak;
        if (found == 0) {
            score += 5; // word-start bonus
        }
        ti = found + 1;
        matchedAny = true;
    }
    if (!matchedAny) {
        return -1;
    }
    // Tighter matches rank higher.
    score += qMax(0, 20 - (ti - q.size()));
    return score;
}

void CommandPalette::refilter()
{
    const QString query = m_query;
    const QStringList recents = recentIds();

    struct Scored {
        int index;
        int score;
    };
    QList<Scored> scored;
    for (int i = 0; i < m_items.size(); ++i) {
        const Item &item = m_items.at(i);
        int score = fuzzyScore(item.title, query);
        if (score < 0) {
            score = fuzzyScore(item.category + QLatin1Char(' ') + item.title, query);
        }
        if (score < 0) {
            continue;
        }
        const int recentPos = recents.indexOf(item.id);
        if (recentPos >= 0) {
            score += (kMaxRecents - recentPos) * 10;
        }
        if (item.action == nullptr) {
            score -= 1; // separators/inert items sink
        }
        scored.append({i, score});
    }
    std::sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.index < b.index; // stable order for equal scores
    });

    m_filtered.clear();
    m_model->setRowCount(0);
    for (const Scored &s : scored) {
        const Item &item = m_items.at(s.index);
        m_filtered.append(s.index);
        auto *title = new QStandardItem(item.category + QStringLiteral(": ") + item.title);
        auto *shortcut = new QStandardItem(item.shortcut);
        m_model->appendRow({title, shortcut});
    }
    if (!m_filtered.isEmpty()) {
        m_view->setCurrentIndex(m_model->index(0, 0));
    }
}

QString CommandPalette::resultIdAt(int row) const
{
    if (row < 0 || row >= m_filtered.size()) {
        return {};
    }
    return m_items.at(m_filtered.at(row)).id;
}

void CommandPalette::runResult(int row)
{
    if (row < 0 || row >= m_filtered.size()) {
        return;
    }
    const Item item = m_items.at(m_filtered.at(row));
    pushRecent(item.id);
    hide();
    if (item.action) {
        item.action();
    }
    accept();
}

QStringList CommandPalette::recentIds()
{
    const QVariant value = Config::instance().value(QStringLiteral("ui/commandPaletteRecents"));
    return value.toStringList();
}

void CommandPalette::pushRecent(const QString &id)
{
    QStringList recents = recentIds();
    recents.removeAll(id);
    recents.prepend(id);
    while (recents.size() > kMaxRecents) {
        recents.removeLast();
    }
    Config::instance().setValue(QStringLiteral("ui/commandPaletteRecents"), recents);
    Config::instance().sync();
}

} // namespace hssh

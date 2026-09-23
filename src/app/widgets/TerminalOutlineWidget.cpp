#include "TerminalOutlineWidget.h"

#include "terminal/TerminalWidget.h"

#include <QLabel>
#include <QListWidget>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

namespace hssh {

namespace {
constexpr int kMaxEntries = 500;
constexpr int kRefreshMs = 3000;

// user@host:...$ prompts, "make[1]: Entering directory", timestamped logs.
const QRegularExpression kPromptPattern(
    QStringLiteral("^[\\w.-]+@[\\w.-]+[:]"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kMakePattern(
    QStringLiteral("make\\[[0-9]+\\].*(Entering|Leaving) directory"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kTimestampPattern(
    QStringLiteral("^[0-9]{4}-[0-9]{2}-[0-9]{2}[ T][0-9]{2}:[0-9]{2}:[0-9]{2}"));
const QRegularExpression kLogLevelPattern(
    QStringLiteral("\\[(FATAL|ERROR|WARN)\\]"),
    QRegularExpression::CaseInsensitiveOption);
} // namespace

TerminalOutlineWidget::TerminalOutlineWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_list = new QListWidget(this);
    m_list->setWordWrap(false);
    m_list->setUniformItemSizes(true);
    layout->addWidget(m_list);

    auto *hint = new QLabel(tr("Prompts, build steps and log headers of the "
                                "current terminal. Click to jump."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #888; padding: 4px;"));
    layout->addWidget(hint);

    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        TerminalWidget *terminal = m_provider ? m_provider() : nullptr;
        if (!terminal || !item) {
            return;
        }
        terminal->scrollToLogicalRow(item->data(Qt::UserRole).toInt());
        terminal->setFocus();
    });

    m_timer = new QTimer(this);
    m_timer->setInterval(kRefreshMs);
    connect(m_timer, &QTimer::timeout, this, &TerminalOutlineWidget::refresh);
}

void TerminalOutlineWidget::setTerminalProvider(std::function<TerminalWidget *()> provider)
{
    m_provider = std::move(provider);
    refresh();
}

void TerminalOutlineWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    refresh();
}

void TerminalOutlineWidget::refresh()
{
    if (!isVisible()) {
        m_timer->stop(); // nothing to do while hidden
        return;
    }
    m_timer->start(); // (re)arm
    m_list->clear();
    TerminalWidget *terminal = m_provider ? m_provider() : nullptr;
    if (!terminal) {
        return;
    }
    const int rows = terminal->bufferRowCount();
    for (int row = 0; row < rows && m_list->count() < kMaxEntries; ++row) {
        const QString line = terminal->outlineLineAt(row);
        if (line.isEmpty() || !isOutlineLine(line)) {
            continue;
        }
        auto *item = new QListWidgetItem(outlineTitle(line));
        item->setData(Qt::UserRole, row);
        item->setToolTip(QString::number(row));
        m_list->addItem(item);
    }
}

bool TerminalOutlineWidget::isOutlineLine(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.size() < 4) {
        return false;
    }
    return kPromptPattern.match(trimmed).hasMatch()
           || kMakePattern.match(trimmed).hasMatch()
           || kTimestampPattern.match(trimmed).hasMatch()
           || kLogLevelPattern.match(trimmed).hasMatch();
}

QString TerminalOutlineWidget::outlineTitle(const QString &line)
{
    QString title = line;
    while (title.endsWith(QLatin1Char(' '))) {
        title.chop(1);
    }
    if (title.size() > 72) {
        title.truncate(72);
        title += QStringLiteral("...");
    }
    return title;
}

} // namespace hssh

#include "DiffView.h"

#include "utils/TextDiff.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QVBoxLayout>

namespace hssh {

namespace {
const QColor kRemovedBg(0x4a, 0x1f, 0x1f);
const QColor kAddedBg(0x1f, 0x3a, 0x1f);
const QColor kPlaceholderBg(0x24, 0x24, 0x25);

void appendLine(QTextCursor &cursor, const QString &text, const QColor &background, bool firstLine)
{
    QTextBlockFormat blockFormat;
    if (background.isValid()) {
        blockFormat.setBackground(background);
    }
    if (firstLine) {
        // Format the document's initial empty block instead of adding one.
        cursor.setBlockFormat(blockFormat);
    } else {
        cursor.insertBlock(blockFormat);
    }
    cursor.insertText(text);
}
} // namespace

DiffView::DiffView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto *titleBar = new QHBoxLayout;
    m_leftLabel = new QLabel(this);
    m_rightLabel = new QLabel(this);
    titleBar->addWidget(m_leftLabel, 1);
    titleBar->addWidget(m_rightLabel, 1);
    layout->addLayout(titleBar);

    auto *panes = new QHBoxLayout;
    m_left = new QPlainTextEdit(this);
    m_right = new QPlainTextEdit(this);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (QPlainTextEdit *pane : {m_left, m_right}) {
        pane->setReadOnly(true);
        pane->setFont(mono);
        pane->setLineWrapMode(QPlainTextEdit::NoWrap);
        panes->addWidget(pane, 1);
    }
    layout->addLayout(panes, 1);

    // Synchronized scrolling.
    const auto syncScroll = [this](QScrollBar *from, QScrollBar *to) {
        connect(from, &QScrollBar::valueChanged, this, [this, to](int value) {
            if (m_syncingScroll) {
                return;
            }
            m_syncingScroll = true;
            to->setValue(value);
            m_syncingScroll = false;
        });
    };
    syncScroll(m_left->verticalScrollBar(), m_right->verticalScrollBar());
    syncScroll(m_right->verticalScrollBar(), m_left->verticalScrollBar());

    m_statsLabel = new QLabel(this);
    layout->addWidget(m_statsLabel);
}

void DiffView::setHint(const QString &text)
{
    m_leftLabel->clear();
    m_rightLabel->clear();
    m_left->clear();
    m_right->clear();
    m_statsLabel->setText(text);
}

void DiffView::setContent(const QString &leftTitle, const QString &leftText,
                          const QString &rightTitle, const QString &rightText)
{
    m_leftLabel->setText(leftTitle);
    m_rightLabel->setText(rightTitle);
    m_left->clear();
    m_right->clear();

    // Build the aligned documents from the diff: Removed lines leave a blank
    // placeholder on the right, Added lines on the left.
    const QList<DiffLine> diff = diffLines(splitIntoLines(leftText), splitIntoLines(rightText));
    int added = 0;
    int removed = 0;

    QTextCursor leftCursor(m_left->document());
    QTextCursor rightCursor(m_right->document());
    leftCursor.beginEditBlock();
    rightCursor.beginEditBlock();
    bool first = true;
    for (const DiffLine &line : diff) {
        // Both documents advance in lockstep (one output line per diff line).
        switch (line.tag) {
        case DiffLine::Tag::Equal:
            appendLine(leftCursor, line.text, QColor(), first);
            appendLine(rightCursor, line.text, QColor(), first);
            break;
        case DiffLine::Tag::Removed:
            appendLine(leftCursor, line.text, kRemovedBg, first);
            appendLine(rightCursor, QString(), kPlaceholderBg, first);
            ++removed;
            break;
        case DiffLine::Tag::Added:
            appendLine(leftCursor, QString(), kPlaceholderBg, first);
            appendLine(rightCursor, line.text, kAddedBg, first);
            ++added;
            break;
        }
        first = false;
    }
    leftCursor.endEditBlock();
    rightCursor.endEditBlock();

    if (added > 0 || removed > 0) {
        m_statsLabel->setText(tr("+%1 / -%2 lines").arg(added).arg(removed));
    } else {
        m_statsLabel->setText(tr("Identical"));
    }
}

} // namespace hssh

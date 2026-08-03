#ifndef HSSH_APP_WIDGETS_DIFFVIEW_H
#define HSSH_APP_WIDGETS_DIFFVIEW_H

#include <QWidget>

QT_BEGIN_NAMESPACE
class QLabel;
class QPlainTextEdit;
QT_END_NAMESPACE

namespace hssh {

// Side-by-side line diff view: left text on the left, right text on the
// right, aligned line-by-line with added/removed highlighting and
// synchronized scrolling. Embeddable: used by DiffDialog and inline in the
// folder compare page.
class DiffView : public QWidget {
    Q_OBJECT

public:
    explicit DiffView(QWidget *parent = nullptr);

    void setContent(const QString &leftTitle, const QString &leftText,
                    const QString &rightTitle, const QString &rightText);
    // Empty panes plus a hint in the status line (e.g. "double-click a row").
    void setHint(const QString &text);

private:
    QPlainTextEdit *m_left = nullptr;
    QPlainTextEdit *m_right = nullptr;
    QLabel *m_leftLabel = nullptr;
    QLabel *m_rightLabel = nullptr;
    QLabel *m_statsLabel = nullptr;
    bool m_syncingScroll = false;
};

} // namespace hssh

#endif // HSSH_APP_WIDGETS_DIFFVIEW_H

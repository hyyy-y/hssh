#ifndef HSSH_APP_WIDGETS_TERMINALOUTLINEWIDGET_H
#define HSSH_APP_WIDGETS_TERMINALOUTLINEWIDGET_H

#include <QWidget>

#include <functional>

class QListWidget;
class QTimer;

namespace hssh {

class TerminalWidget;

// PH2-02: outline of the CURRENT terminal — prompts, make targets and
// timestamped log headers, click to jump. Refreshes while visible.
class TerminalOutlineWidget : public QWidget {
    Q_OBJECT

public:
    // provider returns the terminal to outline (nullptr = none).
    explicit TerminalOutlineWidget(QWidget *parent = nullptr);
    void setTerminalProvider(std::function<TerminalWidget *()> provider);

    // True when the line looks like an outline marker (prompt, make header,
    // timestamped log line). Public for tests.
    [[nodiscard]] static bool isOutlineLine(const QString &line);
    // Display title for a marker line (trimmed, capped).
    [[nodiscard]] static QString outlineTitle(const QString &line);

public slots:
    void refresh();

protected:
    void showEvent(QShowEvent *event) override;

private:
    std::function<TerminalWidget *()> m_provider;
    QListWidget *m_list = nullptr;
    QTimer *m_timer = nullptr;
};

} // namespace hssh

#endif // HSSH_APP_WIDGETS_TERMINALOUTLINEWIDGET_H

#ifndef HSSH_TERMINAL_TERMINALWIDGET_H
#define HSSH_TERMINAL_TERMINALWIDGET_H

#ifdef HSSH_HAS_LIBVTERM
#include <vterm.h>
#endif

#include <QWidget>

#include <deque>
#include <vector>

QT_BEGIN_NAMESPACE
class QScrollBar;
QT_END_NAMESPACE

namespace hssh {

class TerminalWidget : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget() override;

    void feedData(const QByteArray &data);

    [[nodiscard]] int columns() const { return m_cols; }
    [[nodiscard]] int rows() const { return m_rows; }

signals:
    void dataToSend(const QByteArray &data);
    void titleChanged(const QString &title);
    void sizeChanged(int columns, int rows);

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool focusNextPrevChild(bool next) override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    void initializeTerminal();
    void updateTerminalSize();
    void updateScrollBar();
    void scrollToBottom();
    void sendOutputBuffer();
    void renderToPainter(QPainter *painter, const QRect &rect);

    // Selection is stored in absolute buffer coordinates: scrollback lines
    // occupy [0, scrollbackSize) and live-screen rows [scrollbackSize, ...).
    void copySelectionToClipboard();
    void pasteFromClipboard();
    void clearSelection();
    [[nodiscard]] bool hasSelection() const { return m_hasSelection; }
    [[nodiscard]] QString selectedText() const;
    [[nodiscard]] QPoint cellAtPosition(const QPoint &pos) const;
    [[nodiscard]] int logicalRowAt(int viewRow) const;
    bool selectionRangeForRow(int logicalRow, int &startCol, int &endCol) const;

#ifdef HSSH_HAS_LIBVTERM
    struct ScrollbackCell {
        uint32_t chars[VTERM_MAX_CHARS_PER_CELL];
        char width = 1;
        VTermScreenCellAttrs attrs{};
        VTermColor fg{};
        VTermColor bg{};
    };
    using ScrollbackLine = std::vector<ScrollbackCell>;

    // A style-normalized screen cell ready for run-based painting.
    struct PaintCell {
        QString text;
        int width = 1;
        VTermScreenCellAttrs attrs{};
        QColor fg;
        QColor bg;
    };

    static int screenDamage(VTermRect rect, void *user);
    static int screenMoveRect(VTermRect dest, VTermRect src, void *user);
    static int screenMoveCursor(VTermPos pos, VTermPos oldpos, int visible, void *user);
    static int screenSetTermProp(VTermProp prop, VTermValue *val, void *user);
    static int screenBell(void *user);
    static int screenResize(int rows, int cols, void *user);
    static int screenSbPushLine(int cols, const VTermScreenCell *cells, void *user);
    static int screenSbPopLine(int cols, VTermScreenCell *cells, void *user);
    static int screenSbClear(void *user);

    [[nodiscard]] PaintCell makePaintCell(const VTermScreenCell &cell) const;
    [[nodiscard]] PaintCell makePaintCell(const ScrollbackCell &cell) const;
    void drawCellRun(QPainter *painter, int row, int startCol, int span,
                     const QString &text, const PaintCell &style) const;
    void drawCursor(QPainter *painter);
    template <typename Fetch>
    void paintRowCells(QPainter *painter, int row, int startCol, int endCol,
                       int selStartCol, int selEndCol, Fetch &&fetch);
    QColor colorFromVTerm(VTermColor color, bool isForeground) const;
    [[nodiscard]] QString lineTextRange(int logicalRow, int startCol, int endCol) const;
    void onScrollbackLinePushed(int oldSize);
    void onScrollbackLineDropped();

    VTerm *m_vterm = nullptr;
    VTermScreen *m_screen = nullptr;
    std::deque<ScrollbackLine> m_scrollback;
#endif

    QScrollBar *m_scrollBar = nullptr;
    QFont m_font;
    QColor m_selectionBg = QColor(0x26, 0x4f, 0x78);
    bool m_selecting = false;   // mouse drag in progress
    bool m_hasSelection = false;
    int m_selAnchorRow = 0;     // logical buffer row of drag start
    int m_selAnchorCol = 0;
    int m_selEndRow = 0;        // logical buffer row of drag end (unnormalized)
    int m_selEndCol = 0;
    int m_cellWidth = 8;
    int m_cellHeight = 16;
    int m_cellAscent = 12;
    int m_margin = 6; // content padding on every side
    int m_cols = 80;
    int m_rows = 24;
    int m_scrollOffset = 0; // rows scrolled up from the bottom of the buffer
    bool m_cursorVisible = true;
    QPoint m_cursorPos; // cell coordinates (col, row)
};

} // namespace hssh

#endif // HSSH_TERMINAL_TERMINALWIDGET_H

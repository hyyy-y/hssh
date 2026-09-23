#ifndef HSSH_TERMINAL_TERMINALWIDGET_H
#define HSSH_TERMINAL_TERMINALWIDGET_H

#ifdef HSSH_HAS_LIBVTERM
#include <vterm.h>
#endif

#include <QVector>
#include <QWidget>

#include <deque>
#include <vector>

QT_BEGIN_NAMESPACE
class QLineEdit;
class QScrollBar;
QT_END_NAMESPACE

namespace hssh {

class TerminalWidget : public QWidget {
    Q_OBJECT

public:
    explicit TerminalWidget(QWidget *parent = nullptr);
    ~TerminalWidget() override;

    void feedData(const QByteArray &data);

    // PH1-05: runtime font change — recompute cell metrics, resize the
    // terminal grid and repaint.
    void setTerminalFont(const QFont &font);
    [[nodiscard]] QFont terminalFont() const { return m_font; }

    [[nodiscard]] int columns() const { return m_cols; }
    [[nodiscard]] int rows() const { return m_rows; }

    // Terminal-local search (Ctrl+F). Empty query closes the search bar.
    void showSearchBar();
    void closeSearchBar();
    // Jump to the next/previous match; returns false when nothing found.
    bool searchNext();
    bool searchPrevious();

    // Plain-text dump of the last maxLines buffer rows (scrollback tail +
    // live screen), for session logging and the agent "read tab" API.
    [[nodiscard]] QString bufferText(int maxLines) const;
    // Windowed variant: fromLine is a 0-based index into the content rows
    // (empty leading rows trimmed), maxLines caps the result (0 = all).
    [[nodiscard]] QString bufferTextRange(int fromLine, int maxLines) const;

    // PH2-03: the URL (http/https/file/mailto) under a widget position, or
    // an empty string. Trailing sentence punctuation is excluded.
    [[nodiscard]] QString linkAt(const QPoint &pos) const;

    // PH2-04: parse an OSC 52 payload ("52;Ps;Pb64") into the decoded
    // clipboard bytes. Empty when the sequence is malformed, a clipboard
    // query, aimed at an unsupported clipboard, or larger than 1 MB.
    [[nodiscard]] static QByteArray decodeOsc52(const QByteArray &payload);

    // PH2-02: timestamp gutter ("[HH:MM:SS]" left of the grid). Toggling
    // re-derives the grid geometry (columns shrink by the gutter).
    void setShowTimestamps(bool on);
    [[nodiscard]] bool showTimestamps() const { return m_showTimestamps; }

    // PH2-02: outline access — the outline dock lists "interesting" buffer
    // rows (prompts, build headers, timestamped log lines) and jumps to them.
    [[nodiscard]] int bufferRowCount() const;
    [[nodiscard]] QString outlineLineAt(int logicalRow) const;
    void scrollToLogicalRow(int logicalRow);
    // PH2-01: re-wrap the scrollback to a new column count (merge wrapped
    // physical lines into logical lines, re-chunk column-aware so wide
    // glyphs never straddle a boundary). Selection is dropped; an active
    // search re-runs. The column-change debounce calls this; tests drive it
    // directly.
    void reflowScrollback(int newCols);

signals:
    void dataToSend(const QByteArray &data);
    void titleChanged(const QString &title);
    void sizeChanged(int columns, int rows);

protected:
    bool event(QEvent *event) override;
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
    bool eventFilter(QObject *watched, QEvent *event) override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    void initializeTerminal();
    // Re-derive the glyph grid from m_font as measured on this widget's own
    // paint device; returns true when a cell metric changed.
    bool recomputeCellMetrics();
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

    // Search
    void updateSearch(const QString &text);
    void goToMatch(bool next);
    void jumpToMatch(int matchIndex);
    [[nodiscard]] bool searchBarVisible() const { return m_searchBar != nullptr; }
    // Column-aligned text of one logical buffer row (wide chars occupy
    // their grid columns).
    [[nodiscard]] QString lineText(int logicalRow) const;

#ifdef HSSH_HAS_LIBVTERM
    struct ScrollbackCell {
        uint32_t chars[VTERM_MAX_CHARS_PER_CELL];
        char width = 1;
        VTermScreenCellAttrs attrs{};
        VTermColor fg{};
        VTermColor bg{};
    };
    struct ScrollbackEntry {
        std::vector<ScrollbackCell> cells;
        qint64 arriveMs = 0; // scroll-off time, shown by the timestamp gutter
        // PH2-01: this physical line continued the previous one (its content
        // filled the row exactly); reflow merges on this flag.
        bool wrapped = false;
    };

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
                       int selStartCol, int selEndCol, const QVector<int> &searchMatches,
                       Fetch &&fetch);
    QColor colorFromVTerm(VTermColor color, bool isForeground) const;
    [[nodiscard]] QString lineTextRange(int logicalRow, int startCol, int endCol) const;
    void onScrollbackLinePushed(int oldSize);
    void onScrollbackLineDropped();

    // Coalesced repaint: the high-frequency vterm callbacks (damage, scroll
    // moverect, cursor motion) merge into one partial repaint per display
    // frame instead of a full-widget repaint per event-loop turn.
    void scheduleRepaint(const QRect &rect);
    QRect m_pendingRepaint; // pixel coords; null = nothing pending
    bool m_repaintScheduled = false;

    VTerm *m_vterm = nullptr;
    VTermScreen *m_screen = nullptr;
    std::deque<ScrollbackEntry> m_scrollback;
#endif

    QScrollBar *m_scrollBar = nullptr;
    QFont m_font;
    QColor m_selectionBg = QColor(0x26, 0x4f, 0x78);
    QColor m_searchBg = QColor(0x8a, 0x6d, 0x1a);
    bool m_selecting = false;   // mouse drag in progress
    bool m_hasSelection = false;
    int m_selAnchorRow = 0;     // logical buffer row of drag start
    int m_selAnchorCol = 0;
    int m_selEndRow = 0;        // logical buffer row of drag end (unnormalized)
    int m_selEndCol = 0;
    int m_cellWidth = 8;
    int m_cellHeight = 16;
    int m_cellAscent = 12;
    // True when every glyph of m_font has the same advance, i.e. a whole run
    // of text can be painted as one string and still land on the cell grid.
    bool m_monospaceFont = true;
    int m_margin = 6; // content padding on every side
    // PH2-02: timestamp gutter and per-row arrival stamps.
    bool m_showTimestamps = false;
    int m_gutterWidth = 0; // 0 when off, else 11 cells ("[HH:MM:SS] ")
    std::vector<qint64> m_rowStamps; // live-screen rows, ms epoch
    // PH2-01: scrollback reflow debounce (dragging resizes the window in a
    // storm; only the settled column count reflows).
    QTimer *m_reflowTimer = nullptr;
    int m_reflowTargetCols = -1;
    [[nodiscard]] int contentX() const { return m_margin + m_gutterWidth; }
    void updateGutterWidth();
    int m_cols = 80;
    int m_rows = 24;
    int m_scrollOffset = 0; // rows scrolled up from the bottom of the buffer
    // Remote mouse-reporting mode from VTERM_PROP_MOUSE (0 off, 1 click,
    // 2 drag, 3 move). Non-zero routes mouse events to the remote app.
    int m_mouseMode = 0;
    bool m_cursorVisible = true;
    QPoint m_cursorPos; // cell coordinates (col, row)

    QLineEdit *m_searchBar = nullptr;
    QString m_searchText;
    // PH2-03: URL under the cursor while hovering (empty = none).
    QString m_hoverLink;

    // PH2-04: OSC 52 (clipboard set) sniffer state machine. libvterm does
    // not surface OSC 52, so the raw stream is sniffed alongside it.
    enum class Osc52State { Ground, Esc, Body, BodyEsc };
    Osc52State m_osc52State = Osc52State::Ground;
    QByteArray m_osc52Buffer;
    bool m_osc52Handling = false; // reentrancy guard for the prompt dialog
    void scanOsc52(const QByteArray &data);
    void handleOsc52(const QByteArray &payload);
    // Matches on the visible buffer as {row, startCol, endCol} (inclusive).
    QVector<QVector<int>> m_searchRowMatches; // logical row -> flat [start,end,...]
    int m_currentMatch = -1;                  // index into m_searchMatches
    QVector<int> m_searchMatches;             // flat [row,start,end] triplets
};

} // namespace hssh

#endif // HSSH_TERMINAL_TERMINALWIDGET_H

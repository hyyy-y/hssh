#include "TerminalWidget.h"

#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QWheelEvent>

#include "utils/Config.h"

#include <optional>
#include <vector>

namespace hssh {

namespace {
constexpr int defaultColumns = 80;
constexpr int defaultRows = 24;
constexpr int outputBufferSize = 4096;
constexpr int maxScrollbackLines = 10000;

// True when every glyph of the font advances by the same amount, i.e. runs of
// text may be painted as one string and still land on the cell grid.
bool isFixedPitchFont(const QFont &font)
{
    if (QFontInfo(font).fixedPitch()) {
        return true;
    }
    const QFontMetrics fm(font);
    const int advanceM = fm.horizontalAdvance(QLatin1Char('M'));
    return advanceM > 0 && fm.horizontalAdvance(QLatin1Char('i')) == advanceM
           && fm.horizontalAdvance(QLatin1Char('W')) == advanceM
           && fm.horizontalAdvance(QLatin1Char(' ')) == advanceM;
}

// Choose the terminal face once per process. Note that QFont::family() of a
// default-constructed QFont is *not* empty 鈥?it resolves to the application
// font (e.g. Microsoft YaHei UI) 鈥?so an isEmpty() fallback leaves that
// proportional UI font in place and every glyph advance disagrees with the
// cell grid. Pick by verifying the resolved face instead.
const QFont &defaultTerminalFont()
{
    static const QFont font = [] {
        const QStringList preferred = {
            QStringLiteral("Cascadia Mono"), QStringLiteral("Cascadia Code"),
            QStringLiteral("Consolas"), QStringLiteral("Courier New"),
        };
        const QStringList families = QFontDatabase::families();
        for (const QString &family : preferred) {
            if (families.contains(family) && isFixedPitchFont(QFont(family, 10))) {
                return QFont(family, 10);
            }
        }
        // Nothing from the short list: take the first fixed-pitch face the
        // font database knows about (the system fixed font may be a poor
        // terminal face, e.g. NSimSun on zh-CN Windows).
        for (const QString &family : families) {
            if (QFontDatabase::isFixedPitch(family) && isFixedPitchFont(QFont(family, 10))) {
                return QFont(family, 10);
            }
        }
        QFont fallback = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        fallback.setPointSize(10);
        return fallback;
    }();
    return font;
}
} // namespace

TerminalWidget::TerminalWidget(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    // PH2-03: hover tracking for the link hand cursor.
    setMouseTracking(true);

    // Prefer a real terminal font; the system FixedFont on some locales
    // (e.g. NSimSun on zh-CN Windows) renders terminals poorly.
    m_font = defaultTerminalFont();
    m_font.setStyleHint(QFont::TypeWriter);
    m_font.setStyleStrategy(QFont::StyleStrategy(QFont::PreferMatch | QFont::PreferAntialias));
    m_font.setKerning(false);

    // A terminal keeps its own dark color scheme regardless of the app
    // theme; the vterm default fg/bg map to QPalette::Text/Base.
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(0x0c, 0x0c, 0x0c));
    pal.setColor(QPalette::Base, QColor(0x0c, 0x0c, 0x0c));
    pal.setColor(QPalette::Text, QColor(0xcc, 0xcc, 0xcc));
    setPalette(pal);
    setAutoFillBackground(true);

    m_scrollBar = new QScrollBar(Qt::Vertical, this);
    m_scrollBar->setRange(0, 0);
    m_scrollBar->setSingleStep(1);
    m_scrollBar->setPageStep(1);
    m_scrollBar->hide();
    connect(m_scrollBar, &QAbstractSlider::valueChanged, this, [this](int value) {
        const int maxOffset = static_cast<int>(m_scrollback.size());
        m_scrollOffset = maxOffset - value;
        update();
    });

    recomputeCellMetrics();

    // PH2-02: timestamp gutter (Config default off).
    m_showTimestamps = Config::instance().boolValue(QStringLiteral("terminal/showTimestamps"), false);
    updateGutterWidth();
    m_rowStamps.assign(defaultRows, QDateTime::currentMSecsSinceEpoch());

#ifdef HSSH_HAS_LIBVTERM
    initializeTerminal();
#endif
}

TerminalWidget::~TerminalWidget()
{
#ifdef HSSH_HAS_LIBVTERM
    if (m_vterm) {
        vterm_free(m_vterm);
    }
#endif
}

void TerminalWidget::initializeTerminal()
{
#ifdef HSSH_HAS_LIBVTERM
    m_vterm = vterm_new(defaultRows, defaultColumns);
    vterm_set_utf8(m_vterm, 1);
    m_screen = vterm_obtain_screen(m_vterm);

    static const VTermScreenCallbacks callbacks = {
        .damage = &TerminalWidget::screenDamage,
        .moverect = &TerminalWidget::screenMoveRect,
        .movecursor = &TerminalWidget::screenMoveCursor,
        .settermprop = &TerminalWidget::screenSetTermProp,
        .bell = &TerminalWidget::screenBell,
        .resize = &TerminalWidget::screenResize,
        .sb_pushline = &TerminalWidget::screenSbPushLine,
        .sb_popline = &TerminalWidget::screenSbPopLine,
        .sb_clear = &TerminalWidget::screenSbClear,
    };
    vterm_screen_set_callbacks(m_screen, &callbacks, this);
    vterm_screen_reset(m_screen, 1);
#endif
}

void TerminalWidget::setTerminalFont(const QFont &font)
{
    if (font.family() == m_font.family() && font.pointSize() == m_font.pointSize()) {
        return;
    }
    m_font = font;
    m_font.setStyleHint(QFont::TypeWriter);
    m_font.setStyleStrategy(QFont::StyleStrategy(QFont::PreferMatch | QFont::PreferAntialias));
    m_font.setKerning(false);
    recomputeCellMetrics();
    updateTerminalSize();
    update();
}

bool TerminalWidget::recomputeCellMetrics()
{
    // Measure on *this* widget's paint device: QFontMetrics without a device
    // resolves against the primary screen, so a window on a differently
    // scaled monitor would size its cells for the wrong DPI and the painted
    // glyphs would drift off the grid.
    const QFontMetrics fm(m_font, this);
    const int cellWidth = qMax(1, fm.horizontalAdvance(QLatin1Char('M')));
    const int cellHeight = qMax(1, fm.height());
    const int cellAscent = fm.ascent();
    const bool monospace = isFixedPitchFont(m_font);
    const int gutter = m_showTimestamps ? 11 * cellWidth : 0;
    if (cellWidth == m_cellWidth && cellHeight == m_cellHeight
        && cellAscent == m_cellAscent && monospace == m_monospaceFont
        && gutter == m_gutterWidth) {
        return false;
    }
    m_cellWidth = cellWidth;
    m_cellHeight = cellHeight;
    m_cellAscent = cellAscent;
    m_monospaceFont = monospace;
    m_gutterWidth = gutter;
    return true;
}

void TerminalWidget::updateGutterWidth()
{
    m_gutterWidth = m_showTimestamps ? 11 * m_cellWidth : 0;
}

void TerminalWidget::setShowTimestamps(bool on)
{
    if (on == m_showTimestamps) {
        return;
    }
    m_showTimestamps = on;
    Config::instance().setValue(QStringLiteral("terminal/showTimestamps"), on);
    Config::instance().sync();
    updateGutterWidth();
    updateTerminalSize();
    update();
}

int TerminalWidget::bufferRowCount() const
{
#ifdef HSSH_HAS_LIBVTERM
    return static_cast<int>(m_scrollback.size()) + m_rows;
#else
    return 0;
#endif
}

QString TerminalWidget::outlineLineAt(int logicalRow) const
{
#ifdef HSSH_HAS_LIBVTERM
    return lineText(logicalRow);
#else
    Q_UNUSED(logicalRow)
    return {};
#endif
}

void TerminalWidget::scrollToLogicalRow(int logicalRow)
{
#ifdef HSSH_HAS_LIBVTERM
    const int maxOffset = static_cast<int>(m_scrollback.size());
    // Place the requested row three lines below the top edge.
    const int offset = qBound(0, maxOffset - logicalRow + 3, maxOffset);
    if (offset != m_scrollOffset) {
        m_scrollOffset = offset;
        m_scrollBar->setValue(maxOffset - m_scrollOffset);
        update();
    }
#endif
}

// PH2-01: merge wrapped physical lines into logical lines, then re-chunk at
// newCols. Chunking is grid-column aware: a wide glyph never straddles a
// boundary. Selections cannot track the geometry change and are dropped.
void TerminalWidget::reflowScrollback(int newCols)
{
#ifdef HSSH_HAS_LIBVTERM
    if (newCols <= 0 || m_scrollback.empty()) {
        return;
    }
    clearSelection();

    std::deque<ScrollbackEntry> reflowed;
    std::vector<ScrollbackCell> logical; // glyphs of the current logical line
    qint64 logicalStamp = 0;

    const auto flushLogical = [&]() {
        if (logical.empty()) {
            return;
        }
        int col = 0;
        size_t start = 0;
        for (size_t i = 0; i < logical.size(); ++i) {
            const int w = logical[i].width > 0 ? logical[i].width : 1;
            if (col > 0 && col + w > newCols) {
                ScrollbackEntry chunk;
                chunk.cells.assign(logical.begin() + static_cast<long>(start),
                                   logical.begin() + static_cast<long>(i));
                chunk.arriveMs = logicalStamp;
                chunk.wrapped = true;
                reflowed.push_back(std::move(chunk));
                start = i;
                col = 0;
            }
            col += w;
        }
        ScrollbackEntry tail;
        tail.cells.assign(logical.begin() + static_cast<long>(start), logical.end());
        tail.arriveMs = logicalStamp;
        tail.wrapped = false;
        reflowed.push_back(std::move(tail));
        logical.clear();
    };

    for (const ScrollbackEntry &entry : m_scrollback) {
        if (logical.empty()) {
            logicalStamp = entry.arriveMs;
        }
        logical.insert(logical.end(), entry.cells.begin(), entry.cells.end());
        if (!entry.wrapped) {
            flushLogical();
        }
    }
    flushLogical(); // trailing wrapped run without an unwrapped terminator

    while (static_cast<int>(reflowed.size()) > maxScrollbackLines) {
        reflowed.pop_front(); // oldest lines fall out of the buffer
    }
    m_scrollback = std::move(reflowed);

    if (!m_searchText.isEmpty()) {
        updateSearch(m_searchText); // row indices shifted
    }
    updateScrollBar();
    update();
#endif
}

void TerminalWidget::feedData(const QByteArray &data)
{
    // PH2-04: sniff OSC 52 (clipboard set) regardless of libvterm, which
    // does not surface this sequence.
    scanOsc52(data);
#ifdef HSSH_HAS_LIBVTERM
    if (m_vterm) {
        vterm_input_write(m_vterm, data.constData(), static_cast<size_t>(data.size()));
        vterm_screen_flush_damage(m_screen);
        scrollToBottom();
        // Flush terminal-to-remote responses produced while processing this
        // input (cursor-position reports \x1b[6n, device attributes, etc.).
        // readline and other line editors issue such queries when redrawing
        // the prompt; if the reply is never sent (e.g. input driven through
        // the agent API, which bypasses keyPressEvent), their line redraws
        // misplace content and the display shows glued/disordered lines.
        sendOutputBuffer();
        // Matches may point at rows that were just pushed out of the buffer;
        // they are rebuilt on the next search action.
        m_searchRowMatches.clear();
    }
#else
    Q_UNUSED(data)
#endif
}

void TerminalWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), palette().color(QPalette::Base));
    painter.setFont(m_font);
    renderToPainter(&painter, event->rect());
}

QColor TerminalWidget::colorFromVTerm(VTermColor color, bool isForeground) const
{
#ifdef HSSH_HAS_LIBVTERM
    const QPalette pal = palette();
    if (isForeground && VTERM_COLOR_IS_DEFAULT_FG(&color)) {
        return pal.color(QPalette::Text);
    }
    if (!isForeground && VTERM_COLOR_IS_DEFAULT_BG(&color)) {
        return pal.color(QPalette::Base);
    }
    VTermColor c = color;
    vterm_screen_convert_color_to_rgb(m_screen, &c);
    return QColor(c.rgb.red, c.rgb.green, c.rgb.blue);
#else
    Q_UNUSED(color)
    Q_UNUSED(isForeground)
    return QColor();
#endif
}

#ifdef HSSH_HAS_LIBVTERM
namespace {

QString cellText(const uint32_t *chars, bool conceal)
{
    QString text;
    text.reserve(VTERM_MAX_CHARS_PER_CELL);
    for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && chars[i]; ++i) {
        text.append(QString::fromUcs4(reinterpret_cast<const char32_t *>(&chars[i]), 1));
    }
    if (conceal) {
        text.fill(QLatin1Char(' '));
    }
    return text;
}

} // namespace

TerminalWidget::PaintCell TerminalWidget::makePaintCell(const VTermScreenCell &cell) const
{
    PaintCell pc;
    pc.width = cell.width;
    pc.attrs = cell.attrs;
    pc.fg = colorFromVTerm(cell.fg, true);
    pc.bg = colorFromVTerm(cell.bg, false);
    if (pc.attrs.reverse) {
        qSwap(pc.fg, pc.bg);
    }
    pc.text = cellText(cell.chars, pc.attrs.conceal != 0);
    return pc;
}

TerminalWidget::PaintCell TerminalWidget::makePaintCell(const ScrollbackCell &cell) const
{
    PaintCell pc;
    pc.width = cell.width;
    pc.attrs = cell.attrs;
    pc.fg = colorFromVTerm(cell.fg, true);
    pc.bg = colorFromVTerm(cell.bg, false);
    if (pc.attrs.reverse) {
        qSwap(pc.fg, pc.bg);
    }
    pc.text = cellText(cell.chars, pc.attrs.conceal != 0);
    return pc;
}

void TerminalWidget::drawCellRun(QPainter *painter, int row, int startCol, int span,
                                 const QString &text, const PaintCell &style) const
{
    const int x = contentX() + startCol * m_cellWidth;
    const int y = m_margin + row * m_cellHeight;
    painter->fillRect(QRect(x, y, span * m_cellWidth, m_cellHeight), style.bg);

    if (text.isEmpty()) {
        return;
    }

    QFont font = m_font;
    font.setBold(style.attrs.bold != 0);
    font.setItalic(style.attrs.italic != 0);
    font.setUnderline(style.attrs.underline != 0);
    font.setStrikeOut(style.attrs.strike != 0);
    painter->setFont(font);
    painter->setPen(style.fg);
    // Baseline drawing keeps every row on the same line; centering per-cell
    // (AlignVCenter) makes glyphs with different bounding boxes wobble.
    painter->drawText(x, y + m_cellAscent, text);
}

template <typename Fetch>
void TerminalWidget::paintRowCells(QPainter *painter, int row, int startCol, int endCol,
                                   int selStartCol, int selEndCol,
                                   const QVector<int> &searchMatches, Fetch &&fetch)
{
    // fetch(col) returns std::optional<PaintCell>; nullopt = blank grid cell.
    // Cells between selStartCol/selEndCol (inclusive, -1 = no selection)
    // are painted with the selection background; search matches (flat
    // [start,end,...] pairs) get the search background unless selected.
    const auto searched = [&searchMatches](int col) {
        for (int i = 0; i < searchMatches.size(); i += 2) {
            if (col >= searchMatches.at(i) && col <= searchMatches.at(i + 1)) {
                return true;
            }
        }
        return false;
    };
    const auto selected = [this, selStartCol, selEndCol, &searched](std::optional<PaintCell> &pc, int col) {
        if (!pc) {
            return;
        }
        if (selStartCol >= 0 && col >= selStartCol && col <= selEndCol) {
            pc->bg = m_selectionBg;
        } else if (searched(col)) {
            pc->bg = m_searchBg;
        }
    };

    int col = startCol;
    while (col <= endCol) {
        auto first = fetch(col);
        selected(first, col);
        if (!first) {
            if (selStartCol >= 0 && col >= selStartCol && col <= selEndCol) {
                painter->fillRect(QRect(contentX() + col * m_cellWidth, m_margin + row * m_cellHeight,
                                        m_cellWidth, m_cellHeight),
                                  m_selectionBg);
            } else if (searched(col)) {
                painter->fillRect(QRect(contentX() + col * m_cellWidth, m_margin + row * m_cellHeight,
                                        m_cellWidth, m_cellHeight),
                                  m_searchBg);
            }
            ++col;
            continue;
        }

        // Wide glyphs are painted per-cell: their advance comes from a
        // fallback font and would drift off the grid inside a longer run.
        // A proportional face (no fixed-pitch font available) is painted
        // per-cell too, so the glyph advance can never drift away from the
        // cell grid the selection and cursor are drawn on.
        if (first->width != 1 || !m_monospaceFont) {
            drawCellRun(painter, row, col, qMax(1, first->width), first->text, *first);
            col += qMax(1, first->width);
            continue;
        }

        const PaintCell style = *first;
        QString runText = style.text.isEmpty() ? QStringLiteral(" ") : style.text;
        const int runStart = col;
        int span = 1;
        ++col;
        while (col <= endCol) {
            auto next = fetch(col);
            selected(next, col);
            if (!next || next->width != 1
                || next->fg != style.fg || next->bg != style.bg
                || next->attrs.bold != style.attrs.bold
                || next->attrs.italic != style.attrs.italic
                || next->attrs.underline != style.attrs.underline
                || next->attrs.strike != style.attrs.strike) {
                break;
            }
            runText += next->text.isEmpty() ? QStringLiteral(" ") : next->text;
            ++span;
            ++col;
        }
        drawCellRun(painter, row, runStart, span, runText, style);
    }
}

void TerminalWidget::drawCursor(QPainter *painter)
{
    if (!m_cursorVisible || m_scrollOffset != 0
        || m_cursorPos.x() < 0 || m_cursorPos.x() >= m_cols
        || m_cursorPos.y() < 0 || m_cursorPos.y() >= m_rows) {
        return;
    }

    const int x = contentX() + m_cursorPos.x() * m_cellWidth;
    const int y = m_margin + m_cursorPos.y() * m_cellHeight;
    const QRect cellRect(x, y, m_cellWidth, m_cellHeight);
    const QColor cursorColor = palette().color(QPalette::Text);

    if (!hasFocus()) {
        painter->setPen(QPen(cursorColor, 1));
        painter->drawRect(cellRect.adjusted(0, 0, -1, -1));
        return;
    }

    painter->fillRect(cellRect, cursorColor);

    // Redraw the glyph under the cursor in the background color.
    VTermPos pos = {m_cursorPos.y(), m_cursorPos.x()};
    VTermScreenCell cell;
    if (vterm_screen_get_cell(m_screen, pos, &cell) && cell.chars[0]) {
        const PaintCell pc = makePaintCell(cell);
        painter->setPen(pc.bg);
        painter->drawText(x, y + m_cellAscent, pc.text);
    }
}
#endif

void TerminalWidget::renderToPainter(QPainter *painter, const QRect &rect)
{
#ifndef HSSH_HAS_LIBVTERM
    Q_UNUSED(painter)
    Q_UNUSED(rect)
    return;
#else
    if (!m_vterm)
        return;

    const int startRow = qMax(0, (rect.top() - m_margin) / m_cellHeight);
    const int endRow = qMin(m_rows - 1, (rect.bottom() - m_margin) / m_cellHeight);
    const int startCol = qMax(0, (rect.left() - contentX()) / m_cellWidth);
    const int endCol = qMin(m_cols - 1, (rect.right() - m_margin) / m_cellWidth);

    const int scrollbackSize = static_cast<int>(m_scrollback.size());
    const int firstVisibleRow = scrollbackSize - m_scrollOffset;

    // PH2-02: the timestamp gutter, one "[HH:MM:SS]" left of every row.
    if (m_showTimestamps) {
        QFont gutterFont = m_font;
        gutterFont.setBold(false);
        painter->setFont(gutterFont);
        painter->setPen(QColor(0x88, 0x88, 0x88));
        for (int row = startRow; row <= endRow; ++row) {
            const int logicalRow = firstVisibleRow + row;
            qint64 stamp = 0;
            if (logicalRow >= 0 && logicalRow < scrollbackSize) {
                stamp = m_scrollback[static_cast<size_t>(logicalRow)].arriveMs;
            } else {
                const int vtermRow = logicalRow - scrollbackSize;
                if (vtermRow >= 0 && vtermRow < static_cast<int>(m_rowStamps.size())) {
                    stamp = m_rowStamps[static_cast<size_t>(vtermRow)];
                }
            }
            const QDateTime time = QDateTime::fromMSecsSinceEpoch(stamp);
            painter->drawText(m_margin, m_margin + row * m_cellHeight + m_cellAscent,
                              time.toString(QStringLiteral("[HH:MM:SS]")));
        }
    }

    for (int row = startRow; row <= endRow; ++row) {
        const int logicalRow = firstVisibleRow + row;
        int selStart = -1, selEnd = -1;
        selectionRangeForRow(logicalRow, selStart, selEnd);

        static const QVector<int> kNoMatches;
        const QVector<int> &searchMatches =
            (logicalRow >= 0 && logicalRow < m_searchRowMatches.size())
                ? m_searchRowMatches.at(logicalRow)
                : kNoMatches;

        if (logicalRow >= 0 && logicalRow < scrollbackSize) {
            const auto &line = m_scrollback[logicalRow].cells;
            // The line stores one entry per glyph (wide-char continuation
            // cells are omitted), so build a sparse grid-column view first.
            std::vector<const ScrollbackCell *> grid(static_cast<size_t>(m_cols), nullptr);
            int gridCol = 0;
            for (const ScrollbackCell &cell : line) {
                if (gridCol >= m_cols) {
                    break;
                }
                grid[static_cast<size_t>(gridCol)] = &cell;
                gridCol += cell.width > 0 ? cell.width : 1;
            }
            paintRowCells(painter, row, startCol, endCol, selStart, selEnd, searchMatches,
                          [this, &grid](int col) -> std::optional<PaintCell> {
                              const ScrollbackCell *cell = grid[static_cast<size_t>(col)];
                              if (!cell || cell->width == 0) {
                                  return std::nullopt;
                              }
                              return makePaintCell(*cell);
                          });
            continue;
        }

        const int vtermRow = logicalRow - scrollbackSize;
        if (vtermRow < 0 || vtermRow >= m_rows)
            continue;

        paintRowCells(painter, row, startCol, endCol, selStart, selEnd, searchMatches,
                      [this, vtermRow](int col) -> std::optional<PaintCell> {
                          VTermPos pos = {vtermRow, col};
                          VTermScreenCell cell;
                          if (vterm_screen_get_cell(m_screen, pos, &cell) == 0 || cell.width == 0) {
                              return std::nullopt;
                          }
                          return makePaintCell(cell);
                      });
    }

    // Draw the cursor only when looking at the live screen bottom.
    drawCursor(painter);
#endif
}

void TerminalWidget::keyPressEvent(QKeyEvent *event)
{
#ifdef HSSH_HAS_LIBVTERM
    if (!m_vterm) {
        QWidget::keyPressEvent(event);
        return;
    }

    // Terminal-specific scrolling shortcuts.
    if (event->modifiers() == Qt::ShiftModifier) {
        switch (event->key()) {
        case Qt::Key_PageUp:
            m_scrollBar->triggerAction(QAbstractSlider::SliderPageStepSub);
            return;
        case Qt::Key_PageDown:
            m_scrollBar->triggerAction(QAbstractSlider::SliderPageStepAdd);
            return;
        default:
            break;
        }
    }

    // Clipboard shortcuts.
    const Qt::KeyboardModifiers mods = event->modifiers();
    if (mods == Qt::ControlModifier && event->key() == Qt::Key_F) {
        showSearchBar();
        return;
    }
    if (mods == (Qt::ControlModifier | Qt::ShiftModifier)) {
        if (event->key() == Qt::Key_C) {
            copySelectionToClipboard();
            return;
        }
        if (event->key() == Qt::Key_V) {
            pasteFromClipboard();
            return;
        }
    }
    if (mods == Qt::ControlModifier && event->key() == Qt::Key_C && m_hasSelection) {
        // With an active selection Ctrl+C copies instead of sending SIGINT.
        copySelectionToClipboard();
        clearSelection();
        return;
    }
    if (event->key() == Qt::Key_Escape && m_hasSelection) {
        clearSelection();
        return;
    }
    // Typing clears the selection, like other terminals.
    if (m_hasSelection && !event->text().isEmpty()) {
        clearSelection();
    }

    VTermModifier mod = VTERM_MOD_NONE;
    if (event->modifiers() & Qt::ShiftModifier)
        mod = static_cast<VTermModifier>(mod | VTERM_MOD_SHIFT);
    if (event->modifiers() & Qt::ControlModifier)
        mod = static_cast<VTermModifier>(mod | VTERM_MOD_CTRL);
    if (event->modifiers() & Qt::AltModifier)
        mod = static_cast<VTermModifier>(mod | VTERM_MOD_ALT);

    const QString text = event->text();
    // Ctrl+letter arrives as a control character in text() (e.g. 0x03 for
    // Ctrl+C). Feeding that to vterm_keyboard_unichar with VTERM_MOD_CTRL
    // makes libvterm emit a CSI u sequence the remote shell can't parse;
    // route it through the Key_A..Key_Z handler below instead.
    const bool ctrlControlChar = (event->modifiers() & Qt::ControlModifier)
        && !text.isEmpty() && text.at(0).unicode() < 0x20;
    if (!text.isEmpty()
        && !ctrlControlChar
        && event->key() != Qt::Key_Control
        && event->key() != Qt::Key_Shift
        && event->key() != Qt::Key_Alt
        && event->key() != Qt::Key_Meta) {
        for (int i = 0; i < text.size(); ) {
            uint uc = text.at(i).unicode();
            if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
                uc = QChar::surrogateToUcs4(text.at(i), text.at(i + 1));
                ++i;
            }
            vterm_keyboard_unichar(m_vterm, uc, mod);
            ++i;
        }
    } else {
        VTermKey key = VTERM_KEY_NONE;
        switch (event->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            key = VTERM_KEY_ENTER;
            break;
        case Qt::Key_Tab:
            key = VTERM_KEY_TAB;
            break;
        case Qt::Key_Backspace:
            key = VTERM_KEY_BACKSPACE;
            break;
        case Qt::Key_Escape:
            key = VTERM_KEY_ESCAPE;
            break;
        case Qt::Key_Up:
            key = VTERM_KEY_UP;
            break;
        case Qt::Key_Down:
            key = VTERM_KEY_DOWN;
            break;
        case Qt::Key_Left:
            key = VTERM_KEY_LEFT;
            break;
        case Qt::Key_Right:
            key = VTERM_KEY_RIGHT;
            break;
        case Qt::Key_Insert:
            key = VTERM_KEY_INS;
            break;
        case Qt::Key_Delete:
            key = VTERM_KEY_DEL;
            break;
        case Qt::Key_Home:
            key = VTERM_KEY_HOME;
            break;
        case Qt::Key_End:
            key = VTERM_KEY_END;
            break;
        case Qt::Key_PageUp:
            key = VTERM_KEY_PAGEUP;
            break;
        case Qt::Key_PageDown:
            key = VTERM_KEY_PAGEDOWN;
            break;
        default:
            if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F35) {
                key = static_cast<VTermKey>(VTERM_KEY_FUNCTION(event->key() - Qt::Key_F1 + 1));
            }
            break;
        }

        if (key != VTERM_KEY_NONE) {
            vterm_keyboard_key(m_vterm, key, mod);
        } else if (event->modifiers() & Qt::ControlModifier) {
            const int k = event->key();
            if (k >= Qt::Key_A && k <= Qt::Key_Z) {
                vterm_keyboard_unichar(m_vterm, static_cast<uint32_t>('a' + k - Qt::Key_A), VTERM_MOD_CTRL);
            }
        }
    }

    scrollToBottom();
    sendOutputBuffer();
#else
    QWidget::keyPressEvent(event);
#endif
}

void TerminalWidget::sendOutputBuffer()
{
#ifdef HSSH_HAS_LIBVTERM
    if (!m_vterm)
        return;
    char buffer[outputBufferSize];
    size_t len = 0;
    while ((len = vterm_output_read(m_vterm, buffer, sizeof(buffer))) > 0) {
        emit dataToSend(QByteArray(buffer, static_cast<int>(len)));
    }
#endif
}

void TerminalWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    const int scrollBarWidth = style()->pixelMetric(QStyle::PM_ScrollBarExtent);
    m_scrollBar->setGeometry(width() - scrollBarWidth, 0, scrollBarWidth, height());
    if (m_searchBar) {
        m_searchBar->move(qMax(8, width() - m_searchBar->width() - 16), 8);
    }
#ifdef HSSH_HAS_LIBVTERM
    if (recomputeCellMetrics()) {
        update();
    }
    updateTerminalSize();
#endif
}

void TerminalWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
#ifdef HSSH_HAS_LIBVTERM
    if (recomputeCellMetrics()) {
        update();
    }
    updateTerminalSize();
#endif
}

bool TerminalWidget::event(QEvent *event)
{
    const bool handled = QWidget::event(event);
    // Moving the window to a monitor with a different scale changes the
    // device metrics the glyphs are rasterized with.
    if (event->type() == QEvent::ScreenChangeInternal && recomputeCellMetrics()) {
        updateTerminalSize();
        update();
    }
    return handled;
}

void TerminalWidget::updateTerminalSize()
{
#ifdef HSSH_HAS_LIBVTERM
    if (!m_vterm)
        return;

    const int scrollBarWidth = m_scrollBar->isVisible() ? style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0;
    const int newCols = qMax(1, (width() - scrollBarWidth - 2 * m_margin - m_gutterWidth) / m_cellWidth);
    const int newRows = qMax(1, (height() - 2 * m_margin) / m_cellHeight);
    if (newCols != m_cols || newRows != m_rows) {
        const bool colsChanged = (newCols != m_cols);
        m_cols = newCols;
        m_rows = newRows;
        m_rowStamps.assign(static_cast<size_t>(newRows), QDateTime::currentMSecsSinceEpoch());
        vterm_set_size(m_vterm, m_rows, m_cols);
        // A size change may queue a reply (or the remote shell re-queries
        // after SIGWINCH); make sure it reaches the wire promptly.
        sendOutputBuffer();
        updateScrollBar();
        emit sizeChanged(m_cols, m_rows);
        if (colsChanged && !m_scrollback.empty()) {
            // PH2-01: re-wrap the scrollback once the columns settle (drag
            // storms would otherwise reflow dozens of times per gesture).
            m_reflowTargetCols = newCols;
            if (!m_reflowTimer) {
                m_reflowTimer = new QTimer(this);
                m_reflowTimer->setSingleShot(true);
                m_reflowTimer->setInterval(150);
                connect(m_reflowTimer, &QTimer::timeout, this, [this]() {
                    if (m_reflowTargetCols > 0 && m_reflowTargetCols != m_cols) {
                        m_reflowTargetCols = m_cols; // stale request
                    }
                    if (m_reflowTargetCols > 0) {
                        reflowScrollback(m_reflowTargetCols);
                    }
                    m_reflowTargetCols = -1;
                });
            }
            m_reflowTimer->start();
        }
    }
#endif
}

void TerminalWidget::updateScrollBar()
{
#ifdef HSSH_HAS_LIBVTERM
    const int maxOffset = static_cast<int>(m_scrollback.size());
    const bool wasAtBottom = (m_scrollOffset == 0);
    m_scrollBar->setMaximum(maxOffset);
    m_scrollBar->setPageStep(qMax(1, m_rows / 2));
    if (m_scrollback.empty()) {
        m_scrollBar->hide();
    } else {
        m_scrollBar->show();
    }
    m_scrollBar->setValue(maxOffset - m_scrollOffset);
    if (wasAtBottom) {
        m_scrollOffset = 0;
        m_scrollBar->setValue(maxOffset);
    }
#endif
}

void TerminalWidget::scrollToBottom()
{
#ifdef HSSH_HAS_LIBVTERM
    const int maxOffset = static_cast<int>(m_scrollback.size());
    if (m_scrollOffset != 0) {
        m_scrollOffset = 0;
        m_scrollBar->setValue(maxOffset);
        update();
    }
#endif
}

// PH1-04: while the remote application reports mouse events (vim, htop,
// tmux...), clicks/drags/wheel are encoded and forwarded; plain-text
// selection stays available through Shift (xterm convention). The encoding
// itself (SGR/X10) is done by libvterm and leaves via dataToSend.
namespace {
VTermModifier vtermModifiers(Qt::KeyboardModifiers mods)
{
    VTermModifier mod = VTERM_MOD_NONE;
    if (mods & Qt::ShiftModifier) {
        mod = static_cast<VTermModifier>(mod | VTERM_MOD_SHIFT);
    }
    if (mods & Qt::ControlModifier) {
        mod = static_cast<VTermModifier>(mod | VTERM_MOD_CTRL);
    }
    if (mods & Qt::AltModifier) {
        mod = static_cast<VTermModifier>(mod | VTERM_MOD_ALT);
    }
    return mod;
}

int vtermButton(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton:
        return 1;
    case Qt::MiddleButton:
        return 2;
    case Qt::RightButton:
        return 3;
    default:
        return 0;
    }
}
} // namespace

void TerminalWidget::wheelEvent(QWheelEvent *event)
{
#ifdef HSSH_HAS_LIBVTERM
    // Shift bypasses remote reporting (local scrollback), like xterm.
    if (m_mouseMode != 0 && !(event->modifiers() & Qt::ShiftModifier)) {
        const QPoint cell = cellAtPosition(event->position().toPoint());
        vterm_mouse_move(m_vterm, cell.y(), cell.x(), vtermModifiers(event->modifiers()));
        const int steps = qAbs(event->angleDelta().y()) / 120;
        for (int i = 0; i < qMax(1, steps); ++i) {
            vterm_mouse_button(m_vterm, event->angleDelta().y() > 0 ? 4 : 5, true,
                               vtermModifiers(event->modifiers()));
        }
        return;
    }
    if (event->angleDelta().y() > 0) {
        m_scrollBar->triggerAction(QAbstractSlider::SliderSingleStepSub);
    } else if (event->angleDelta().y() < 0) {
        m_scrollBar->triggerAction(QAbstractSlider::SliderSingleStepAdd);
    }
#else
    Q_UNUSED(event)
#endif
}

QPoint TerminalWidget::cellAtPosition(const QPoint &pos) const
{
    const int col = qBound(0, (pos.x() - contentX()) / m_cellWidth, m_cols - 1);
    const int row = qBound(0, (pos.y() - m_margin) / m_cellHeight, m_rows - 1);
    return QPoint(col, row);
}

int TerminalWidget::logicalRowAt(int viewRow) const
{
    return static_cast<int>(m_scrollback.size()) - m_scrollOffset + viewRow;
}

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
#ifdef HSSH_HAS_LIBVTERM
    const int button = vtermButton(event->button());
    if (m_mouseMode != 0 && button != 0 && !(event->modifiers() & Qt::ShiftModifier)) {
        setFocus();
        const QPoint cell = cellAtPosition(event->pos());
        vterm_mouse_move(m_vterm, cell.y(), cell.x(), vtermModifiers(event->modifiers()));
        vterm_mouse_button(m_vterm, button, true, vtermModifiers(event->modifiers()));
        return;
    }
#endif
    if (event->button() == Qt::LeftButton) {
        setFocus();
        // PH2-03: Ctrl+Click opens the link under the cursor.
        if ((event->modifiers() & Qt::ControlModifier) && m_mouseMode == 0) {
            const QString link = linkAt(event->pos());
            if (!link.isEmpty()) {
                QDesktopServices::openUrl(QUrl(link, QUrl::StrictMode));
                return;
            }
        }
        const QPoint cell = cellAtPosition(event->pos());
        m_selecting = true;
        m_selAnchorRow = m_selEndRow = logicalRowAt(cell.y());
        m_selAnchorCol = m_selEndCol = cell.x();
        if (m_hasSelection) {
            m_hasSelection = false;
            update();
        }
        return;
    }
    QWidget::mousePressEvent(event);
}

void TerminalWidget::mouseMoveEvent(QMouseEvent *event)
{
#ifdef HSSH_HAS_LIBVTERM
    const bool anyButton = event->buttons() != Qt::NoButton;
    // Drag reporting (mode 2+) needs a held button; move reporting (3)
    // streams unconditionally. Shift bypasses to local selection.
    if (m_mouseMode >= 2 && !(event->modifiers() & Qt::ShiftModifier)
        && (m_mouseMode >= 3 || anyButton)) {
        const QPoint cell = cellAtPosition(event->pos());
        vterm_mouse_move(m_vterm, cell.y(), cell.x(), vtermModifiers(event->modifiers()));
        return;
    }
#endif
    if (m_selecting && (event->buttons() & Qt::LeftButton)) {
        const QPoint cell = cellAtPosition(event->pos());
        const int row = logicalRowAt(cell.y());
        if (row != m_selEndRow || cell.x() != m_selEndCol) {
            m_selEndRow = row;
            m_selEndCol = cell.x();
            const bool nonEmpty = (m_selAnchorRow != m_selEndRow || m_selAnchorCol != m_selEndCol);
            if (nonEmpty != m_hasSelection) {
                m_hasSelection = nonEmpty;
            }
            update();
        }
        return;
    }
    // PH2-03: hover 鈥?switch to the hand cursor while over a link.
    if (event->buttons() == Qt::NoButton && m_mouseMode == 0) {
        const QString link = linkAt(event->pos());
        if (link != m_hoverLink) {
            m_hoverLink = link;
            setCursor(link.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
            setToolTip(link);
        }
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event)
{
#ifdef HSSH_HAS_LIBVTERM
    const int button = vtermButton(event->button());
    if (m_mouseMode != 0 && button != 0 && !(event->modifiers() & Qt::ShiftModifier)) {
        const QPoint cell = cellAtPosition(event->pos());
        vterm_mouse_move(m_vterm, cell.y(), cell.x(), vtermModifiers(event->modifiers()));
        vterm_mouse_button(m_vterm, button, false, vtermModifiers(event->modifiers()));
        return;
    }
#endif
    if (event->button() == Qt::LeftButton && m_selecting) {
        m_selecting = false;
        // A plain click (no drag) just clears the selection.
        if (m_selAnchorRow == m_selEndRow && m_selAnchorCol == m_selEndCol && m_hasSelection) {
            clearSelection();
        }
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void TerminalWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    // PH2-03: link actions when the menu opens on a URL.
    const QString link = linkAt(event->pos());
    QAction *openLinkAction = nullptr;
    QAction *copyLinkAction = nullptr;
    if (!link.isEmpty()) {
        openLinkAction = menu.addAction(tr("Open Link"));
        copyLinkAction = menu.addAction(tr("Copy Link Address"));
        menu.addSeparator();
    }
    QAction *copyAction = menu.addAction(tr("Copy"));
    copyAction->setEnabled(m_hasSelection);
    QAction *pasteAction = menu.addAction(tr("Paste"));
    pasteAction->setEnabled(!QGuiApplication::clipboard()->text().isEmpty());
    menu.addSeparator();
    QAction *findAction = menu.addAction(tr("Find..."));
    findAction->setShortcut(QKeySequence::Find);
    menu.addSeparator();
    QAction *timestampsAction = menu.addAction(tr("Show Timestamps"));
    timestampsAction->setCheckable(true);
    timestampsAction->setChecked(m_showTimestamps);
    connect(timestampsAction, &QAction::toggled, this,
            &TerminalWidget::setShowTimestamps);

    const QAction *chosen = menu.exec(event->globalPos());
    if (chosen == openLinkAction && openLinkAction) {
        QDesktopServices::openUrl(QUrl(link, QUrl::StrictMode));
    } else if (chosen == copyLinkAction && copyLinkAction) {
        QGuiApplication::clipboard()->setText(link);
    } else if (chosen == copyAction) {
        copySelectionToClipboard();
    } else if (chosen == pasteAction) {
        pasteFromClipboard();
    } else if (chosen == findAction) {
        showSearchBar();
    }
}

bool TerminalWidget::selectionRangeForRow(int logicalRow, int &startCol, int &endCol) const
{
    if (!m_hasSelection) {
        return false;
    }
    int r1 = m_selAnchorRow, c1 = m_selAnchorCol;
    int r2 = m_selEndRow, c2 = m_selEndCol;
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        qSwap(r1, r2);
        qSwap(c1, c2);
    }
    if (logicalRow < r1 || logicalRow > r2) {
        return false;
    }
    startCol = (logicalRow == r1) ? c1 : 0;
    endCol = (logicalRow == r2) ? c2 : m_cols - 1;
    return true;
}

QString TerminalWidget::lineTextRange(int logicalRow, int startCol, int endCol) const
{
#ifdef HSSH_HAS_LIBVTERM
    const int sbSize = static_cast<int>(m_scrollback.size());
    QString text;
    if (logicalRow >= 0 && logicalRow < sbSize) {
        // Entries are sequential from column 0; wide-char continuation
        // cells are omitted, so walk with a running column counter.
        const auto &line = m_scrollback[static_cast<size_t>(logicalRow)].cells;
        int col = 0;
        for (const ScrollbackCell &cell : line) {
            const int w = cell.width > 0 ? cell.width : 1;
            if (col > endCol) {
                break;
            }
            if (col >= startCol) {
                const QString t = cellText(cell.chars, false);
                text += t.isEmpty() ? QString(w, QLatin1Char(' ')) : t;
            }
            col += w;
        }
        return text;
    }

    const int vtermRow = logicalRow - sbSize;
    if (vtermRow < 0 || vtermRow >= m_rows) {
        return {};
    }
    int col = startCol;
    while (col <= endCol && col < m_cols) {
        VTermPos pos = {vtermRow, col};
        VTermScreenCell cell;
        if (vterm_screen_get_cell(m_screen, pos, &cell) == 0) {
            ++col;
            continue;
        }
        const int w = cell.width > 0 ? cell.width : 1;
        const QString t = cellText(cell.chars, false);
        text += t.isEmpty() ? QString(w, QLatin1Char(' ')) : t;
        col += w;
    }
    return text;
#else
    Q_UNUSED(logicalRow)
    Q_UNUSED(startCol)
    Q_UNUSED(endCol)
    return {};
#endif
}

QString TerminalWidget::lineText(int logicalRow) const
{
    return lineTextRange(logicalRow, 0, m_cols - 1);
}

QString TerminalWidget::linkAt(const QPoint &pos) const
{
#ifdef HSSH_HAS_LIBVTERM
    const QPoint cell = cellAtPosition(pos);
    const QString line = lineText(logicalRowAt(cell.y()));
    if (line.isEmpty()) {
        return {};
    }
    const int col = qBound(0, cell.x(), line.size() - 1);
    if (line.at(col).isSpace()) {
        return {};
    }
    // The token is the maximal run of non-space characters around the click.
    int start = col;
    while (start > 0 && !line.at(start - 1).isSpace()) {
        --start;
    }
    int end = col + 1;
    while (end < line.size() && !line.at(end).isSpace()) {
        ++end;
    }
    QString token = line.mid(start, end - start);

    static const QRegularExpression inner(
        QStringLiteral("(https?://|file://|mailto:)[^\\s]+"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = inner.match(token);
    if (!match.hasMatch()) {
        return {};
    }
    // The URL may be glued to prose ("(see https://x/y).") 鈥?take it from the
    // scheme on and drop trailing sentence punctuation.
    QString link = token.mid(match.capturedStart(1));
    while (!link.isEmpty()
           && QStringLiteral(".,;:!?)>]}\"'").contains(link.right(1))) {
        link.chop(1);
    }
    return link;
#else
    Q_UNUSED(pos)
    return {};
#endif
}

void TerminalWidget::scanOsc52(const QByteArray &data)
{
    if (m_osc52Handling) {
        return; // a prompt is up; drop concurrent sequences
    }
    for (char c : data) {
        switch (m_osc52State) {
        case Osc52State::Ground:
            if (c == '\x1b') {
                m_osc52State = Osc52State::Esc;
            }
            break;
        case Osc52State::Esc:
            if (c == ']') {
                m_osc52State = Osc52State::Body;
                m_osc52Buffer.clear();
            } else if (c != '\x1b') {
                m_osc52State = Osc52State::Ground;
            }
            break;
        case Osc52State::Body:
            if (c == '\x07') {
                m_osc52State = Osc52State::Ground;
                handleOsc52(m_osc52Buffer);
                m_osc52Buffer.clear();
            } else if (c == '\x1b') {
                m_osc52State = Osc52State::BodyEsc;
            } else {
                m_osc52Buffer.append(c);
            }
            break;
        case Osc52State::BodyEsc:
            // "\x1b\\" (ST) terminates; anything else aborts the sequence.
            m_osc52State = Osc52State::Ground;
            m_osc52Buffer.clear();
            if (c == '\\') {
                handleOsc52(m_osc52Buffer);
            } else if (c == '\x1b') {
                m_osc52State = Osc52State::Esc;
            }
            break;
        }
        // Runaway OSC (no terminator): drop it instead of growing forever.
        if (m_osc52Buffer.size() > 8 * 1024 * 1024) {
            m_osc52State = Osc52State::Ground;
            m_osc52Buffer.clear();
        }
    }
}

QByteArray TerminalWidget::decodeOsc52(const QByteArray &payload)
{
    // Format: "52;Ps;Pb64" — Ps selects the clipboard(s), Pb64 is the base64
    // payload. An empty Pb64 is a clipboard query; we never answer those.
    if (!payload.startsWith("52;")) {
        return {};
    }
    const QByteArray rest = payload.mid(3);
    const int semi = rest.indexOf(';');
    if (semi < 0) {
        return {};
    }
    const QByteArray selector = rest.left(semi);
    const QByteArray b64 = rest.mid(semi + 1);
    if (b64.isEmpty()) {
        return {};
    }
    bool wanted = selector.isEmpty();
    for (char s : selector) {
        wanted = wanted || s == 'c' || s == 'p' || s == 's';
    }
    if (!wanted) {
        return {};
    }
    const auto decoded = QByteArray::fromBase64Encoding(
        b64, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.size() > 1024 * 1024) {
        return {}; // corrupt or absurdly large
    }
    return decoded.decoded;
}

void TerminalWidget::handleOsc52(const QByteArray &payload)
{
    const QByteArray decoded = decodeOsc52(payload);
    if (decoded.isEmpty()) {
        return;
    }
    const QString mode = Config::instance().stringValue(
        QStringLiteral("terminal/osc52Mode"), QStringLiteral("prompt"));
    if (mode == QLatin1String("deny")) {
        return;
    }
    if (mode != QLatin1String("allow")) {
        m_osc52Handling = true;
        const QString text = tr("The remote host wants to put %n byte(s) into your "
                                "clipboard. Allow?", nullptr, decoded.size());
        const auto answer = QMessageBox::question(
            this, tr("Remote Clipboard Access"), text,
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        m_osc52Handling = false;
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    QGuiApplication::clipboard()->setText(QString::fromUtf8(decoded));
}

QString TerminalWidget::bufferText(int maxLines) const
{
    return bufferTextRange(0, maxLines);
}

QString TerminalWidget::bufferTextRange(int fromLine, int maxLines) const
{
#ifdef HSSH_HAS_LIBVTERM
    const int totalRows = static_cast<int>(m_scrollback.size()) + m_rows;
    if (totalRows <= 0) {
        return {};
    }

    QStringList lines;
    lines.reserve(totalRows);
    for (int row = 0; row < totalRows; ++row) {
        QString line = lineText(row);
        // Trailing blanks are cell padding, not content.
        while (line.endsWith(QLatin1Char(' '))) {
            line.chop(1);
        }
        lines.append(line);
    }
    // Drop fully-empty lines at both ends (unused scrollback/screen rows) so
    // a partially filled screen still returns its content.
    while (!lines.isEmpty() && lines.first().isEmpty()) {
        lines.removeFirst();
    }
    while (!lines.isEmpty() && lines.last().isEmpty()) {
        lines.removeLast();
    }
    if (fromLine > 0) {
        if (fromLine >= lines.size()) {
            return {};
        }
        lines = lines.mid(fromLine);
    }
    if (maxLines > 0 && lines.size() > maxLines) {
        lines = lines.mid(lines.size() - maxLines);
    }
    return lines.join(QLatin1Char('\n'));
#else
    Q_UNUSED(fromLine)
    Q_UNUSED(maxLines)
    return {};
#endif
}

void TerminalWidget::showSearchBar()
{
    if (m_searchBar) {
        m_searchBar->setFocus();
        m_searchBar->selectAll();
        return;
    }

    m_searchBar = new QLineEdit(this);
    m_searchBar->setPlaceholderText(tr("Find... (Enter next, Shift+Enter previous, Esc close)"));
    m_searchBar->setFixedWidth(300);
    m_searchBar->setClearButtonEnabled(true);
    m_searchBar->installEventFilter(this);
    m_searchBar->show();
    m_searchBar->setFocus();
    m_searchBar->move(qMax(8, width() - m_searchBar->width() - 16), 8);

    connect(m_searchBar, &QLineEdit::textChanged, this, &TerminalWidget::updateSearch);
    connect(m_searchBar, &QLineEdit::returnPressed, this, &TerminalWidget::searchNext);
}

void TerminalWidget::closeSearchBar()
{
    if (!m_searchBar) {
        return;
    }
    m_searchBar->removeEventFilter(this);
    m_searchBar->deleteLater();
    m_searchBar = nullptr;
    m_searchText.clear();
    m_searchMatches.clear();
    m_searchRowMatches.clear();
    m_currentMatch = -1;
    update();
    setFocus();
}

void TerminalWidget::updateSearch(const QString &text)
{
    m_searchText = text;
    m_searchMatches.clear();
    m_searchRowMatches.clear();
    m_currentMatch = -1;

    if (text.isEmpty()) {
        update();
        return;
    }

    const int totalRows = static_cast<int>(m_scrollback.size()) + m_rows;
    m_searchRowMatches.resize(totalRows);
    for (int row = 0; row < totalRows; ++row) {
        const QString line = lineText(row);
        int from = 0;
        while ((from = line.indexOf(text, from, Qt::CaseInsensitive)) >= 0) {
            const int end = from + text.size() - 1;
            m_searchMatches << row << from << end;
            m_searchRowMatches[row] << from << end;
            from = end + 2;
        }
    }

    if (!m_searchMatches.isEmpty()) {
        jumpToMatch(0);
    } else {
        update();
    }
}

bool TerminalWidget::searchNext()
{
    if (m_searchMatches.isEmpty()) {
        return false;
    }
    const int count = m_searchMatches.size() / 3;
    jumpToMatch((m_currentMatch + 1) % count);
    return true;
}

bool TerminalWidget::searchPrevious()
{
    if (m_searchMatches.isEmpty()) {
        return false;
    }
    const int count = m_searchMatches.size() / 3;
    jumpToMatch(m_currentMatch <= 0 ? count - 1 : m_currentMatch - 1);
    return true;
}

void TerminalWidget::jumpToMatch(int matchIndex)
{
    if (m_searchMatches.isEmpty()) {
        return;
    }
    m_currentMatch = matchIndex;
    const int row = m_searchMatches.at(matchIndex * 3);

    // Bring the match row into view: scrollbar value equals the logical row
    // when that row sits at the top of the viewport.
    m_scrollBar->setValue(qBound(m_scrollBar->minimum(), row, m_scrollBar->maximum()));
    update();
}

bool TerminalWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_searchBar && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            closeSearchBar();
            return true;
        }
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            if (keyEvent->modifiers() & Qt::ShiftModifier) {
                searchPrevious();
            } else {
                searchNext();
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

QString TerminalWidget::selectedText() const
{
    if (!m_hasSelection) {
        return {};
    }
    int r1 = m_selAnchorRow, c1 = m_selAnchorCol;
    int r2 = m_selEndRow, c2 = m_selEndCol;
    if (r1 > r2 || (r1 == r2 && c1 > c2)) {
        qSwap(r1, r2);
        qSwap(c1, c2);
    }

    QStringList lines;
    for (int row = r1; row <= r2; ++row) {
        const int startCol = (row == r1) ? c1 : 0;
        const int endCol = (row == r2) ? c2 : m_cols - 1;
        QString line = lineTextRange(row, startCol, endCol);
        // Trailing blanks are cell padding, not content.
        while (line.endsWith(QLatin1Char(' '))) {
            line.chop(1);
        }
        lines.append(line);
    }
    return lines.join(QLatin1Char('\n'));
}

void TerminalWidget::copySelectionToClipboard()
{
    const QString text = selectedText();
    if (!text.isEmpty()) {
        QGuiApplication::clipboard()->setText(text);
    }
}

void TerminalWidget::pasteFromClipboard()
{
    QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty()) {
        return;
    }
    // Shells expect carriage return for newlines.
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    text.replace(QLatin1Char('\n'), QLatin1Char('\r'));
    emit dataToSend(text.toUtf8());
}

void TerminalWidget::clearSelection()
{
    m_selecting = false;
    if (m_hasSelection) {
        m_hasSelection = false;
        update();
    }
}

void TerminalWidget::onScrollbackLinePushed(int oldSize)
{
#ifdef HSSH_HAS_LIBVTERM
    // Screen content moved into the scrollback; selections pointing at it
    // shift down by one absolute row.
    if (!m_hasSelection) {
        return;
    }
    if (m_selAnchorRow >= oldSize) {
        ++m_selAnchorRow;
    }
    if (m_selEndRow >= oldSize) {
        ++m_selEndRow;
    }
#else
    Q_UNUSED(oldSize)
#endif
}

void TerminalWidget::onScrollbackLineDropped()
{
    if (!m_hasSelection) {
        return;
    }
    --m_selAnchorRow;
    --m_selEndRow;
    if (m_selAnchorRow < 0 || m_selEndRow < 0) {
        clearSelection();
    }
}

bool TerminalWidget::focusNextPrevChild(bool next)
{
    Q_UNUSED(next)
    // Keep focus inside the terminal so Tab is sent to the shell instead of
    // moving the Qt focus to another widget.
    return false;
}

QSize TerminalWidget::sizeHint() const
{
    return QSize(m_cols * m_cellWidth + 2 * m_margin + m_gutterWidth, m_rows * m_cellHeight + 2 * m_margin);
}

QSize TerminalWidget::minimumSizeHint() const
{
    return QSize(defaultColumns * m_cellWidth / 2, defaultRows * m_cellHeight / 2);
}

void TerminalWidget::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
#ifdef HSSH_HAS_LIBVTERM
    if (m_vterm) {
        VTermState *state = vterm_obtain_state(m_vterm);
        vterm_state_focus_in(state);
    }
#endif
}

void TerminalWidget::focusOutEvent(QFocusEvent *event)
{
    QWidget::focusOutEvent(event);
#ifdef HSSH_HAS_LIBVTERM
    if (m_vterm) {
        VTermState *state = vterm_obtain_state(m_vterm);
        vterm_state_focus_out(state);
    }
#endif
}

#ifdef HSSH_HAS_LIBVTERM
void TerminalWidget::scheduleRepaint(const QRect &rect)
{
#ifdef HSSH_HAS_LIBVTERM
    if (rect.isEmpty()) {
        return;
    }
    m_pendingRepaint = m_pendingRepaint.isNull() ? rect : m_pendingRepaint.united(rect);
    if (m_repaintScheduled) {
        return;
    }
    m_repaintScheduled = true;
    // ~60 fps cap: damage arriving in bursts (streaming output, resize
    // redraws) is merged into one partial repaint per frame instead of a
    // full-widget repaint per event-loop turn.
    QTimer::singleShot(16, this, [this]() {
        m_repaintScheduled = false;
        const QRect rect = m_pendingRepaint;
        m_pendingRepaint = QRect();
        if (!rect.isNull()) {
            update(rect);
        }
    });
#else
    Q_UNUSED(rect)
#endif
}

int TerminalWidget::screenDamage(VTermRect rect, void *user)
{
    auto *widget = static_cast<TerminalWidget *>(user);
    const int left = widget->contentX() + rect.start_col * widget->m_cellWidth;
    const int top = widget->m_margin + rect.start_row * widget->m_cellHeight;
    const int right = widget->m_margin + rect.end_col * widget->m_cellWidth;
    const int bottom = widget->m_margin + rect.end_row * widget->m_cellHeight;
    widget->scheduleRepaint(QRect(left, top, right - left, bottom - top));
    // PH2-02: per-row arrival stamps feed the timestamp gutter.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int row = rect.start_row; row <= rect.end_row && row < static_cast<int>(widget->m_rowStamps.size()); ++row) {
        if (row >= 0) {
            widget->m_rowStamps[static_cast<size_t>(row)] = now;
        }
    }
    return 1;
}

int TerminalWidget::screenMoveRect(VTermRect dest, VTermRect src, void *user)
{
    auto *widget = static_cast<TerminalWidget *>(user);
    // Scroll: only the affected region needs repainting, not the whole widget.
    const auto toPixels = [widget](const VTermRect &r) {
        return QRect(widget->contentX() + r.start_col * widget->m_cellWidth,
                     widget->m_margin + r.start_row * widget->m_cellHeight,
                     (r.end_col - r.start_col) * widget->m_cellWidth,
                     (r.end_row - r.start_row) * widget->m_cellHeight);
    };
    widget->scheduleRepaint(toPixels(dest).united(toPixels(src)));
    return 1;
}

int TerminalWidget::screenMoveCursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    auto *widget = static_cast<TerminalWidget *>(user);
    widget->m_cursorVisible = visible != 0;
    widget->m_cursorPos = QPoint(pos.col, pos.row);
    // Cursor motion (readline redraws one cell per keystroke) only dirties
    // the old and new cursor cells; a full repaint per keystroke made a
    // maximized terminal visibly laggy.
    const auto cellRect = [widget](const VTermPos &p) {
        return QRect(widget->contentX() + p.col * widget->m_cellWidth,
                     widget->m_margin + p.row * widget->m_cellHeight,
                     widget->m_cellWidth, widget->m_cellHeight);
    };
    widget->scheduleRepaint(cellRect(pos).united(cellRect(oldpos)));
    return 1;
}

int TerminalWidget::screenSetTermProp(VTermProp prop, VTermValue *val, void *user)
{
    auto *widget = static_cast<TerminalWidget *>(user);
    if (prop == VTERM_PROP_CURSORVISIBLE) {
        widget->m_cursorVisible = val->boolean != 0;
    } else if (prop == VTERM_PROP_TITLE) {
        if (val->string.str) {
            emit widget->titleChanged(QString::fromUtf8(val->string.str, static_cast<int>(val->string.len)));
        }
    } else if (prop == VTERM_PROP_MOUSE) {
        // The application enabled/disabled mouse reporting (DECSET
        // 1000/1002/1006): 0 off, 1 click, 2 drag, 3 move. While active,
        // mouse events are encoded (SGR) and sent to the remote instead of
        // driving local selection/scrollback.
        widget->m_mouseMode = val->number;
    }
    return 1;
}

int TerminalWidget::screenBell(void *user)
{
    Q_UNUSED(user)
    return 1;
}

int TerminalWidget::screenResize(int rows, int cols, void *user)
{
    auto *widget = static_cast<TerminalWidget *>(user);
    widget->m_rows = rows;
    widget->m_cols = cols;
    widget->updateScrollBar();
    return 1;
}

int TerminalWidget::screenSbPushLine(int cols, const VTermScreenCell *cells, void *user)
{
    auto *widget = static_cast<TerminalWidget *>(user);
    const int oldSize = static_cast<int>(widget->m_scrollback.size());
    ScrollbackEntry entry;
    entry.arriveMs = QDateTime::currentMSecsSinceEpoch();
    entry.cells.reserve(cols);
    for (int i = 0; i < cols; ) {
        ScrollbackCell sc;
        sc.width = cells[i].width;
        sc.attrs = cells[i].attrs;
        sc.fg = cells[i].fg;
        sc.bg = cells[i].bg;
        for (int c = 0; c < VTERM_MAX_CHARS_PER_CELL; ++c) {
            sc.chars[c] = cells[i].chars[c];
        }
        entry.cells.push_back(sc);
        i += cells[i].width > 0 ? cells[i].width : 1;
    }
    // PH2-01 wrap heuristic: the line filled the row exactly (ignoring the
    // blank padding cells), so the next physical line probably continues it.
    int lastContentCol = 0;
    int col = 0;
    for (const ScrollbackCell &cell : entry.cells) {
        const int w = cell.width > 0 ? cell.width : 1;
        if (cell.chars[0] != 0) {
            lastContentCol = col + w;
        }
        col += w;
    }
    entry.wrapped = (lastContentCol >= cols);
    widget->m_scrollback.push_back(std::move(entry));
    widget->onScrollbackLinePushed(oldSize);
    if (static_cast<int>(widget->m_scrollback.size()) > maxScrollbackLines) {
        widget->m_scrollback.pop_front();
        widget->onScrollbackLineDropped();
    }
    widget->updateScrollBar();
    return 1;
}

int TerminalWidget::screenSbPopLine(int cols, VTermScreenCell *cells, void *user)
{
    auto *widget = static_cast<TerminalWidget *>(user);
    if (widget->m_scrollback.empty()) {
        return 0;
    }
    // Scrollback geometry changes in ways a selection can't track; drop it.
    widget->clearSelection();
    const auto &line = widget->m_scrollback.back().cells;
    // The scrollback stores one entry per glyph (wide-char continuation cells
    // are omitted). Restore them at their ORIGINAL grid columns: a wide glyph
    // (width=2) must be followed by a width=0 continuation cell, otherwise
    // every glyph after it shifts left and the line renders glued/disordered.
    int out = 0;
    for (const ScrollbackCell &sc : line) {
        if (out >= cols) {
            break;
        }
        cells[out] = {};
        cells[out].width = sc.width;
        cells[out].attrs = sc.attrs;
        cells[out].fg = sc.fg;
        cells[out].bg = sc.bg;
        for (int c = 0; c < VTERM_MAX_CHARS_PER_CELL; ++c) {
            cells[out].chars[c] = sc.chars[c];
        }
        out += sc.width > 0 ? sc.width : 1;
    }
    for (int i = out; i < cols; ++i) {
        cells[i] = {};
    }
    widget->m_scrollback.pop_back();
    widget->updateScrollBar();
    return 1;
}

int TerminalWidget::screenSbClear(void *user)
{
    auto *widget = static_cast<TerminalWidget *>(user);
    widget->m_scrollback.clear();
    widget->clearSelection();
    widget->updateScrollBar();
    return 1;
}
#endif

} // namespace hssh

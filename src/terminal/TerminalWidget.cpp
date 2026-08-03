#include "TerminalWidget.h"

#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <QStyle>
#include <QWheelEvent>

#include <optional>
#include <vector>

namespace hssh {

namespace {
constexpr int defaultColumns = 80;
constexpr int defaultRows = 24;
constexpr int outputBufferSize = 4096;
constexpr int maxScrollbackLines = 10000;
} // namespace

TerminalWidget::TerminalWidget(QWidget *parent)
    : QWidget(parent)
{
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    setAttribute(Qt::WA_OpaquePaintEvent);

    // Prefer a real terminal font; the system FixedFont on some locales
    // (e.g. NSimSun on zh-CN Windows) renders terminals poorly.
    const QStringList preferred = {
        QStringLiteral("Cascadia Mono"), QStringLiteral("Cascadia Code"),
        QStringLiteral("Consolas"), QStringLiteral("Courier New"),
    };
    const QStringList families = QFontDatabase::families();
    for (const QString &family : preferred) {
        if (families.contains(family)) {
            m_font = QFont(family, 10);
            break;
        }
    }
    if (m_font.family().isEmpty()) {
        m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    }
    m_font.setStyleHint(QFont::TypeWriter);
    m_font.setStyleStrategy(QFont::StyleStrategy(QFont::PreferMatch | QFont::PreferAntialias));
    m_font.setKerning(false);

    const QFontMetrics fm(m_font);
    m_cellWidth = qMax(1, fm.horizontalAdvance(QLatin1Char('M')));
    m_cellHeight = fm.height();
    m_cellAscent = fm.ascent();

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

void TerminalWidget::feedData(const QByteArray &data)
{
#ifdef HSSH_HAS_LIBVTERM
    if (m_vterm) {
        vterm_input_write(m_vterm, data.constData(), static_cast<size_t>(data.size()));
        vterm_screen_flush_damage(m_screen);
        scrollToBottom();
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
    const int x = m_margin + startCol * m_cellWidth;
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
                                   int selStartCol, int selEndCol, Fetch &&fetch)
{
    // fetch(col) returns std::optional<PaintCell>; nullopt = blank grid cell.
    // Cells between selStartCol/selEndCol (inclusive, -1 = no selection)
    // are painted with the selection background.
    const auto selected = [this, selStartCol, selEndCol](std::optional<PaintCell> &pc, int col) {
        if (pc && selStartCol >= 0 && col >= selStartCol && col <= selEndCol) {
            pc->bg = m_selectionBg;
        }
    };

    int col = startCol;
    while (col <= endCol) {
        auto first = fetch(col);
        selected(first, col);
        if (!first) {
            if (selStartCol >= 0 && col >= selStartCol && col <= selEndCol) {
                painter->fillRect(QRect(m_margin + col * m_cellWidth, m_margin + row * m_cellHeight,
                                        m_cellWidth, m_cellHeight),
                                  m_selectionBg);
            }
            ++col;
            continue;
        }

        // Wide glyphs are painted per-cell: their advance comes from a
        // fallback font and would drift off the grid inside a longer run.
        if (first->width != 1) {
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

    const int x = m_margin + m_cursorPos.x() * m_cellWidth;
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
    const int startCol = qMax(0, (rect.left() - m_margin) / m_cellWidth);
    const int endCol = qMin(m_cols - 1, (rect.right() - m_margin) / m_cellWidth);

    const int scrollbackSize = static_cast<int>(m_scrollback.size());
    const int firstVisibleRow = scrollbackSize - m_scrollOffset;

    for (int row = startRow; row <= endRow; ++row) {
        const int logicalRow = firstVisibleRow + row;
        int selStart = -1, selEnd = -1;
        selectionRangeForRow(logicalRow, selStart, selEnd);

        if (logicalRow >= 0 && logicalRow < scrollbackSize) {
            const auto &line = m_scrollback[logicalRow];
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
            paintRowCells(painter, row, startCol, endCol, selStart, selEnd,
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

        paintRowCells(painter, row, startCol, endCol, selStart, selEnd,
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
    if (!text.isEmpty()
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
#ifdef HSSH_HAS_LIBVTERM
    updateTerminalSize();
#endif
}

void TerminalWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
#ifdef HSSH_HAS_LIBVTERM
    updateTerminalSize();
#endif
}

void TerminalWidget::updateTerminalSize()
{
#ifdef HSSH_HAS_LIBVTERM
    if (!m_vterm)
        return;

    const int scrollBarWidth = m_scrollBar->isVisible() ? style()->pixelMetric(QStyle::PM_ScrollBarExtent) : 0;
    const int newCols = qMax(1, (width() - scrollBarWidth - 2 * m_margin) / m_cellWidth);
    const int newRows = qMax(1, (height() - 2 * m_margin) / m_cellHeight);
    if (newCols != m_cols || newRows != m_rows) {
        m_cols = newCols;
        m_rows = newRows;
        vterm_set_size(m_vterm, m_rows, m_cols);
        updateScrollBar();
        emit sizeChanged(m_cols, m_rows);
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

void TerminalWidget::wheelEvent(QWheelEvent *event)
{
#ifdef HSSH_HAS_LIBVTERM
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
    const int col = qBound(0, (pos.x() - m_margin) / m_cellWidth, m_cols - 1);
    const int row = qBound(0, (pos.y() - m_margin) / m_cellHeight, m_rows - 1);
    return QPoint(col, row);
}

int TerminalWidget::logicalRowAt(int viewRow) const
{
    return static_cast<int>(m_scrollback.size()) - m_scrollOffset + viewRow;
}

void TerminalWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        setFocus();
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
    QWidget::mouseMoveEvent(event);
}

void TerminalWidget::mouseReleaseEvent(QMouseEvent *event)
{
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
    QAction *copyAction = menu.addAction(tr("Copy"));
    copyAction->setEnabled(m_hasSelection);
    QAction *pasteAction = menu.addAction(tr("Paste"));
    pasteAction->setEnabled(!QGuiApplication::clipboard()->text().isEmpty());

    const QAction *chosen = menu.exec(event->globalPos());
    if (chosen == copyAction) {
        copySelectionToClipboard();
    } else if (chosen == pasteAction) {
        pasteFromClipboard();
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
        const auto &line = m_scrollback[static_cast<size_t>(logicalRow)];
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
    return QSize(m_cols * m_cellWidth + 2 * m_margin, m_rows * m_cellHeight + 2 * m_margin);
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
int TerminalWidget::screenDamage(VTermRect rect, void *user)
{
    auto *widget = static_cast<TerminalWidget *>(user);
    const int left = widget->m_margin + rect.start_col * widget->m_cellWidth;
    const int top = widget->m_margin + rect.start_row * widget->m_cellHeight;
    const int right = widget->m_margin + rect.end_col * widget->m_cellWidth;
    const int bottom = widget->m_margin + rect.end_row * widget->m_cellHeight;
    widget->update(left, top, right - left, bottom - top);
    return 1;
}

int TerminalWidget::screenMoveRect(VTermRect dest, VTermRect src, void *user)
{
    Q_UNUSED(dest)
    Q_UNUSED(src)
    auto *widget = static_cast<TerminalWidget *>(user);
    widget->update();
    return 1;
}

int TerminalWidget::screenMoveCursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    Q_UNUSED(oldpos)
    auto *widget = static_cast<TerminalWidget *>(user);
    widget->m_cursorVisible = visible != 0;
    widget->m_cursorPos = QPoint(pos.col, pos.row);
    widget->update();
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
    ScrollbackLine line;
    line.reserve(cols);
    for (int i = 0; i < cols; ) {
        ScrollbackCell sc;
        sc.width = cells[i].width;
        sc.attrs = cells[i].attrs;
        sc.fg = cells[i].fg;
        sc.bg = cells[i].bg;
        for (int c = 0; c < VTERM_MAX_CHARS_PER_CELL; ++c) {
            sc.chars[c] = cells[i].chars[c];
        }
        line.push_back(sc);
        i += cells[i].width > 0 ? cells[i].width : 1;
    }
    widget->m_scrollback.push_back(std::move(line));
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
    const auto &line = widget->m_scrollback.back();
    for (int i = 0; i < cols; ++i) {
        if (i < static_cast<int>(line.size())) {
            cells[i] = {};
            cells[i].width = line[i].width;
            cells[i].attrs = line[i].attrs;
            cells[i].fg = line[i].fg;
            cells[i].bg = line[i].bg;
            for (int c = 0; c < VTERM_MAX_CHARS_PER_CELL; ++c) {
                cells[i].chars[c] = line[i].chars[c];
            }
        } else {
            cells[i] = {};
        }
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

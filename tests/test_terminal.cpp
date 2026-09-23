#include "terminal/LocalShellProcess.h"
#include "terminal/TerminalWidget.h"
#include "app/widgets/TerminalOutlineWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QFontInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QThread>
#include <QtTest/QtTest>

#include "utils/Config.h"

using namespace hssh;

class TestTerminal : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qApp->setApplicationName(QStringLiteral("hssh_test_terminal"));
        qApp->setOrganizationName(QStringLiteral("hssh_project_test"));
    }

    void testTerminalWidgetFeedData()
    {
        TerminalWidget widget;
        widget.show();
        QTest::qWaitForWindowExposed(&widget);

        QVERIFY(widget.columns() > 0);
        QVERIFY(widget.rows() > 0);

        // Feeding plain text should not crash and the widget should remain valid.
        widget.feedData("hello world\r\n");
        QTest::qWait(100);
        QVERIFY(widget.columns() > 0);
    }

    void testLocalShellProcessLifecycle()
    {
        const QString shell = defaultShellForPlatform();
        LocalShellProcess process(shell);
        QSignalSpy dataSpy(&process, &LocalShellProcess::dataReceived);
        QSignalSpy errorSpy(&process, &LocalShellProcess::errorOccurred);

        QVERIFY2(process.start(),
                 errorSpy.isEmpty() ? "process.start() failed" : errorSpy.first().first().toString().toUtf8().constData());
        QVERIFY(process.isRunning());

        // Give the shell a moment to print its prompt.
        QTest::qWait(500);

        process.write(echoCommand());

        // Wait for the echoed text to appear in the output.
        QByteArray allOutput;
        bool foundEcho = false;
        for (int i = 0; i < 100 && !foundEcho; ++i) {
            QTest::qWait(100);
            allOutput.clear();
            for (int j = 0; j < dataSpy.size(); ++j) {
                allOutput.append(dataSpy.at(j).at(0).toByteArray());
            }
            foundEcho = allOutput.contains("hssh_test");
        }
        qDebug() << "PTY output:" << allOutput;
        QVERIFY(foundEcho);

        process.close();
        QTRY_VERIFY_WITH_TIMEOUT(!process.isRunning(), 3000);
    }

    void testLocalShellProcessResize()
    {
        const QString shell = defaultShellForPlatform();
        LocalShellProcess process(shell);
        QVERIFY(process.start());
        QVERIFY(process.isRunning());

        // Resize should not crash. The PTY will forward SIGWINCH (Unix) or
        // equivalent (ConPTY) to the child.
        process.resize(120, 40);
        QTest::qWait(100);
        QVERIFY(process.isRunning());

        process.close();
        QTRY_VERIFY_WITH_TIMEOUT(!process.isRunning(), 3000);
    }

    void testLocalShellProcessGracefulExit()
    {
        const QString shell = defaultShellForPlatform();
        LocalShellProcess process(shell);
        QSignalSpy finishedSpy(&process, &LocalShellProcess::finished);

        QVERIFY(process.start());
        process.write(exitCommand());

        QTRY_VERIFY_WITH_TIMEOUT(!finishedSpy.isEmpty(), 5000);
    }

    void testLocalShellProcessCloseKill()
    {
        const QString shell = defaultShellForPlatform();
        LocalShellProcess process(shell);
        QVERIFY(process.start());
        QVERIFY(process.isRunning());

        process.close();
        QTRY_VERIFY_WITH_TIMEOUT(!process.isRunning(), 5000);
    }

    void testAvailableShells()
    {
        const QStringList shells = LocalShellProcess::availableShells();
        QVERIFY(!shells.isEmpty());

        const QString def = LocalShellProcess::defaultShell();
        QVERIFY(!def.isEmpty());
        QVERIFY(!LocalShellProcess::shellExecutable(def).isEmpty());
    }

#ifdef HSSH_HAS_LIBVTERM
    // The terminal grid assumes every glyph advances by the same amount; a
    // proportional face (the old isEmpty() fallback left the application font,
    // e.g. Microsoft YaHei UI, in place) drifts away from it.
    void testDefaultTerminalFontIsFixedPitch()
    {
        TerminalWidget widget;
        const QFont font = widget.terminalFont();
        const QFontMetrics fm(font);
        const int advanceM = fm.horizontalAdvance(QLatin1Char('M'));
        QVERIFY2(advanceM > 0, qPrintable(font.family()));
        QVERIFY2(QFontInfo(font).fixedPitch()
                     || (fm.horizontalAdvance(QLatin1Char('i')) == advanceM
                         && fm.horizontalAdvance(QLatin1Char('W')) == advanceM
                         && fm.horizontalAdvance(QLatin1Char(' ')) == advanceM),
                 qPrintable(QStringLiteral("terminal font '%1' is not fixed pitch")
                                .arg(font.family())));
    }

    // Regression: with a proportional face every cell is painted at its own
    // grid position. Otherwise text advances by the glyph width while the
    // selection/cursor use the cell grid, and selected characters look "far
    // apart" (each one stranded at the left of a wide cell).
    void testProportionalFontGlyphsStayOnGrid()
    {
        TerminalWidget widget;
        widget.show();
        QTest::qWaitForWindowExposed(&widget);

        const QFont proportional = QApplication::font();
        widget.setTerminalFont(proportional);
        widget.feedData(QByteArray("iiiiiiiiiiii\r\n"));
        QTest::qWait(200);

        const QImage image = widget.grab().toImage();
        const QFontMetrics fm(proportional, &widget);
        const int cellWidth = qMax(1, fm.horizontalAdvance(QLatin1Char('M')));
        const int cellHeight = qMax(1, fm.height());
        const int narrowAdvance = qMax(1, fm.horizontalAdvance(QLatin1Char('i')));
        QVERIFY(cellWidth > 0);

        // Topmost ink row starts the first text row; scan that band only (the
        // cursor block sits on the row below).
        int top = -1;
        for (int y = 0; y < image.height() && top < 0; ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (image.pixelColor(x, y).lightness() > 96) {
                    top = y;
                    break;
                }
            }
        }
        QVERIFY2(top >= 0, "no glyph pixels rendered");
        const int bottom = qMin(image.height(), top + cellHeight);

        // Ink columns of the first text row: one cluster per painted cell. A
        // proportional face painted as a single run collapses them together.
        QVector<int> ink;
        for (int x = 0; x < image.width(); ++x) {
            for (int y = top; y < bottom; ++y) {
                if (image.pixelColor(x, y).lightness() > 96) {
                    ink.append(x);
                    break;
                }
            }
        }
        QVERIFY2(!ink.isEmpty(), "no glyph pixels rendered");

        // 12 glyphs in 12 cells must span ~11 cells. Painted as one run they
        // advance by the narrow glyph width instead (a fraction of the span).
        const int span = ink.last() - ink.first();
        QVERIFY2(span >= 6 * cellWidth,
                 qPrintable(QStringLiteral("text advances by the glyph width, not the cell "
                                           "grid: ink span %1px across 12 cells (cell width %2, "
                                           "narrow glyph advance %3)")
                                .arg(span)
                                .arg(cellWidth)
                                .arg(narrowAdvance)));

        int clusters = 1;
        for (int i = 1; i < ink.size(); ++i) {
            if (ink.at(i) - ink.at(i - 1) > 1) {
                ++clusters;
            }
        }
        QVERIFY2(clusters >= 6, qPrintable(QStringLiteral("glyphs collapsed into %1 cluster(s)")
                                               .arg(clusters)));
    }

    // PH2-03: URL detection under a position. The scan is metric-agnostic
    // (no assumptions about margins or cell sizes).
    void testLinkDetection()
    {
        TerminalWidget widget;
        widget.show();
        QTest::qWaitForWindowExposed(&widget);

        widget.feedData(QByteArray("see https://example.com/path?a=1 now "
                                   "and (http://a.b/c). mailto:x@y.z done\r\n"));
        widget.feedData(QByteArray("no links here, just git@host:repo and /usr/bin\r\n"));
        QTest::qWait(100);

        QSet<QString> found;
        for (int y = 0; y < 60; ++y) {
            for (int x = 0; x < widget.width(); x += 2) {
                const QString link = widget.linkAt(QPoint(x, y));
                if (!link.isEmpty()) {
                    found.insert(link);
                }
            }
        }
        QVERIFY2(found.contains(QStringLiteral("https://example.com/path?a=1")),
                 qPrintable(found.values().join(QStringLiteral(", "))));
        QVERIFY2(found.contains(QStringLiteral("http://a.b/c")),
                 "trailing ')' and '.' must be stripped from the URL");
        QVERIFY2(found.contains(QStringLiteral("mailto:x@y.z")),
                 qPrintable(found.values().join(QStringLiteral(", "))));
        // Non-URLs (scp-style path, absolute path) must not be detected.
        for (const QString &link : std::as_const(found)) {
            QVERIFY2(!link.contains(QLatin1String("git@")),
                     qPrintable(QStringLiteral("false positive: %1").arg(link)));
            QVERIFY2(!link.startsWith(QLatin1String("/usr")),
                     qPrintable(QStringLiteral("false positive: %1").arg(link)));
        }
        // The plain-text line stays untouched by the second row.
        QVERIFY(!widget.linkAt(QPoint(widget.width() / 2, 55)).isEmpty()
                || widget.linkAt(QPoint(4, 55)).isEmpty());
    }

    // PH2-04: OSC 52 payload decoding (pure, no clipboard involved).
    void testOsc52Decode()
    {
        QCOMPARE(TerminalWidget::decodeOsc52(QByteArray("52;c;aGVsbG8gd29ybGQ=")),
                 QByteArray("hello world"));
        QCOMPARE(TerminalWidget::decodeOsc52(QByteArray("52;;aGVsbG8=")),
                 QByteArray("hello")); // empty selector = default clipboard
        QVERIFY(TerminalWidget::decodeOsc52(QByteArray("0;window title")).isEmpty());
        QVERIFY(TerminalWidget::decodeOsc52(QByteArray("52;c;")).isEmpty());   // query
        QVERIFY(TerminalWidget::decodeOsc52(QByteArray("52;q;aGVsbG8=")).isEmpty()); // other clipboard
        QVERIFY(TerminalWidget::decodeOsc52(QByteArray("52;c;!!!!")).isEmpty());     // bad base64
        const QByteArray big = QByteArray(1200 * 1024, 'a');
        QVERIFY(TerminalWidget::decodeOsc52(QByteArray("52;c;") + big.toBase64()).isEmpty());
    }

    // PH2-04: OSC 52 clipboard capture (BEL and ST terminators, split
    // chunks, size cap, queries, non-52 OSC passthrough). Needs a working
    // system clipboard — skipped when a clipboard manager hogs it.
    void testOsc52Clipboard()
    {
        auto &config = Config::instance();
        QClipboard *clipboard = QGuiApplication::clipboard();
        config.setValue(QStringLiteral("terminal/osc52Mode"), QStringLiteral("allow"));

        // The desktop clipboard manager occasionally keeps the clipboard
        // open; retry instead of racing it, and skip when it stays busy.
        const auto seed = [&clipboard](const QString &text) {
            for (int i = 0; i < 10; ++i) {
                clipboard->setText(text);
                QThread::msleep(30);
                if (clipboard->text() == text) {
                    return true;
                }
            }
            return false;
        };
        const auto expect = [&clipboard, &seed](const QString &text) {
            for (int i = 0; i < 10 && clipboard->text() != text; ++i) {
                QThread::msleep(30);
            }
            if (clipboard->text() != text && !seed(QStringLiteral("hssh-probe"))) {
                QSKIP("system clipboard unavailable (locked by another process)");
            }
            QCOMPARE(clipboard->text(), text);
        };

        TerminalWidget widget;
        if (!seed(QStringLiteral("sentinel"))) {
            QSKIP("system clipboard unavailable (locked by another process)");
        }

        // BEL terminator.
        widget.feedData(QByteArray("before \x1b]52;c;aGVsbG8gd29ybGQ=\x07 after"));
        expect(QStringLiteral("hello world"));

        // ST terminator, split across two feedData calls.
        if (!seed(QStringLiteral("sentinel"))) {
            QSKIP("system clipboard unavailable (locked by another process)");
        }
        widget.feedData(QByteArray("xx \x1b]52;c;"));
        widget.feedData(QByteArray("c3BsaXQgb2s=\x1b\\yy"));
        expect(QStringLiteral("split ok"));

        // Other OSC sequences (window title) must not touch the clipboard.
        if (!seed(QStringLiteral("sentinel"))) {
            QSKIP("system clipboard unavailable (locked by another process)");
        }
        widget.feedData(QByteArray("\x1b]0;my title\x07"));
        expect(QStringLiteral("sentinel"));

        // deny mode ignores everything.
        config.setValue(QStringLiteral("terminal/osc52Mode"), QStringLiteral("deny"));
        widget.feedData(QByteArray("\x1b]52;c;ZGVueQ==\x07"));
        expect(QStringLiteral("sentinel"));
        clipboard->clear();
    }

    // PH2-02: timestamp gutter geometry + outline markers + buffer access.
    void testTimestampsAndOutline()
    {
        Config::instance().setValue(QStringLiteral("terminal/showTimestamps"), false);
        Config::instance().sync();

        QVERIFY(TerminalOutlineWidget::isOutlineLine(QStringLiteral("user@host:~$ ls")));
        QVERIFY(TerminalOutlineWidget::isOutlineLine(
            QStringLiteral("make[1]: Entering directory '/build'")));
        QVERIFY(TerminalOutlineWidget::isOutlineLine(
            QStringLiteral("2026-09-21 12:00:00 INFO engine start")));
        QVERIFY(TerminalOutlineWidget::isOutlineLine(QStringLiteral("[FATAL] boom")));
        QVERIFY(!TerminalOutlineWidget::isOutlineLine(QStringLiteral("plain output line")));
        QVERIFY(!TerminalOutlineWidget::isOutlineLine(QStringLiteral("ok")));
        QCOMPARE(TerminalOutlineWidget::outlineTitle(QStringLiteral("  hello  ")),
                 QStringLiteral("  hello"));

        TerminalWidget widget;
        widget.show();
        QTest::qWaitForWindowExposed(&widget);

        // Push enough lines to grow the scrollback (and show the scrollbar)
        // BEFORE taking the column baseline.
        for (int i = 0; i < 60; ++i) {
            widget.feedData(QStringLiteral("plain %1\r\n").arg(i).toUtf8());
        }
        widget.feedData(QByteArray("user@host:~$ make all\r\n"));
        QTest::qWait(100);
        QVERIFY(widget.bufferRowCount() > 24);

        // Force a geometry pass so the column count reflects the scrollbar
        // that just appeared (columns() is only recomputed on resizes).
        widget.resize(widget.width() + 1, widget.height());
        QTest::qWait(50);
        const int colsBefore = widget.columns();
        QVERIFY(colsBefore > 40);

        // The marker line must be findable through the outline API.
        int markerRow = -1;
        for (int row = 0; row < widget.bufferRowCount(); ++row) {
            if (TerminalOutlineWidget::isOutlineLine(widget.outlineLineAt(row))) {
                markerRow = row;
                break;
            }
        }
        QVERIFY2(markerRow >= 0, "outline marker not found in the buffer");
        widget.scrollToLogicalRow(markerRow); // smoke: no crash

        // The gutter eats 11 columns when enabled and restores them when off.
        widget.setShowTimestamps(true);
        QCOMPARE(widget.columns(), colsBefore - 11);
        QVERIFY(Config::instance().boolValue(QStringLiteral("terminal/showTimestamps"), false));
        widget.setShowTimestamps(false);
        QCOMPARE(widget.columns(), colsBefore);
        Config::instance().setValue(QStringLiteral("terminal/showTimestamps"), false);
        Config::instance().sync();
    }

    // PH2-01: scrollback reflow — merge wrapped lines into logical lines and
    // re-chunk at the new width (wide glyphs must not straddle a boundary).
    void testScrollbackReflow()
    {
        TerminalWidget widget;
        widget.show();
        QTest::qWaitForWindowExposed(&widget);
        // Default geometry: 80 columns.
        const int cols = widget.columns();

        // 100 A's wrap into 80 + 20 at 80 columns; 30 filler lines push
        // everything into the scrollback.
        QByteArray as(100, 'A');
        as += "\r\n";
        widget.feedData(as);
        for (int i = 0; i < 30; ++i) {
            widget.feedData(QStringLiteral("filler %1\r\n").arg(i).toUtf8());
        }
        QTest::qWait(100);
        QVERIFY(widget.bufferRowCount() > 24);

        // Collect the scrollback rows that contain only A's.
        const auto aRows = [&widget]() {
            QStringList rows;
            for (int r = 0; r < widget.bufferRowCount(); ++r) {
                const QString line = widget.outlineLineAt(r);
                QString trimmed = line;
                while (trimmed.endsWith(QLatin1Char(' '))) {
                    trimmed.chop(1);
                }
                if (!trimmed.isEmpty() && !trimmed.contains(QLatin1Char(' '))) {
                    rows.append(trimmed);
                }
            }
            return rows;
        };
        // At 80 columns the A-run is split 80/20.
        QStringList before = aRows();
        QCOMPARE(before.size(), 2);
        QCOMPARE(before.at(0).size(), 80);
        QCOMPARE(before.at(1).size(), 20);

        // Narrow to half: the 100 A's re-chunk into 3 physical lines
        // (40 + 40 + 20).
        widget.reflowScrollback(cols / 2);
        QStringList narrowed = aRows();
        QCOMPARE(narrowed.size(), 3);
        QCOMPARE(narrowed.at(0).size(), 40);
        QCOMPARE(narrowed.at(1).size(), 40);
        QCOMPARE(narrowed.at(2).size(), 20);
        QCOMPARE(narrowed.join(QString()).size(), 100);

        // Back to the original width: the same logical line is 80/20 again.
        widget.reflowScrollback(cols);
        QStringList restored = aRows();
        QCOMPARE(restored.size(), 2);
        QCOMPARE(restored.at(0).size(), 80);
        QCOMPARE(restored.join(QString()), before.join(QString()));
    }

    // PH2-01: a wide glyph sitting across the new wrap boundary must move to
    // the next chunk whole, not split.
    void testScrollbackReflowWideGlyphs()
    {
        TerminalWidget widget;
        widget.show();
        QTest::qWaitForWindowExposed(&widget);
        const int cols = widget.columns();

        // CJK glyphs render two cells wide: 20 of them = 40 columns; pad with
        // single cells so a wide glyph straddles column 20 when halved.
        QString line;
        for (int i = 0; i < 19; ++i) {
            line += QChar(0x4E00 + (i % 20)); // 一 丁 丂 ...
        }
        line += QStringLiteral("XY");
        line += QStringLiteral("\r\n");
        for (int i = 0; i < 30; ++i) {
            widget.feedData(QStringLiteral("filler %1\r\n").arg(i).toUtf8());
        }
        widget.feedData(line.toUtf8());
        for (int i = 0; i < 30; ++i) {
            widget.feedData(QStringLiteral("tail %1\r\n").arg(i).toUtf8());
        }
        QTest::qWait(100);

        widget.reflowScrollback(20);
        // No physical line may end with a broken surrogate: every outline
        // row must round-trip through QString cleanly and the concatenated
        // glyph count is unchanged (19 wide + 2 narrow = 21 glyphs).
        int wideGlyphLines = 0;
        int totalGlyphs = 0;
        for (int r = 0; r < widget.bufferRowCount(); ++r) {
            QString row = widget.outlineLineAt(r);
            while (row.endsWith(QLatin1Char(' '))) {
                row.chop(1);
            }
            bool hasCJK = false;
            for (const QChar ch : row) {
                if (ch.unicode() >= 0x4E00 && ch.unicode() <= 0x9FFF) {
                    hasCJK = true;
                    break;
                }
            }
            if (hasCJK) {
                ++wideGlyphLines;
                totalGlyphs += row.size();
            }
        }
        QVERIFY(wideGlyphLines >= 2); // the run was re-chunked
        QCOMPARE(totalGlyphs, 21);
    }

    void testDsrReplyFlushedOnFeedData()
    {
        TerminalWidget widget;
        widget.show();
        QTest::qWaitForWindowExposed(&widget);

        QSignalSpy sendSpy(&widget, &TerminalWidget::dataToSend);

        // Cursor position request (readline sends \x1b[6n when redrawing the
        // prompt). The terminal must reply \x1b[<row>;<col>R even when input
        // is driven through the API (no key events), or readline's line
        // redraws misplace content and the display glues lines together.
        widget.feedData(QByteArray("\x1b[6n"));
        QVERIFY(!sendSpy.isEmpty());
        const QByteArray reply = sendSpy.takeFirst().at(0).toByteArray();
        QVERIFY2(reply.startsWith("\x1b["), reply.toHex().constData());
        QVERIFY2(reply.endsWith("R"), reply.toHex().constData());
    }

    void testCtrlKeySequences()
    {
        TerminalWidget widget;
        widget.show();
        QTest::qWaitForWindowExposed(&widget);

        QSignalSpy sendSpy(&widget, &TerminalWidget::dataToSend);

        // Qt delivers Ctrl+letter with the control character in text();
        // the terminal must emit the raw control byte (0x03 / 0x04), not a
        // libvterm CSI u sequence that the remote shell can't parse.
        QKeyEvent ctrlC(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, QString(QChar(0x03)));
        QApplication::sendEvent(&widget, &ctrlC);
        QVERIFY(!sendSpy.isEmpty());
        QCOMPARE(sendSpy.takeFirst().at(0).toByteArray(), QByteArray(1, '\x03'));

        QKeyEvent ctrlD(QEvent::KeyPress, Qt::Key_D, Qt::ControlModifier, QString(QChar(0x04)));
        QApplication::sendEvent(&widget, &ctrlD);
        QVERIFY(!sendSpy.isEmpty());
        QCOMPARE(sendSpy.takeFirst().at(0).toByteArray(), QByteArray(1, '\x04'));
    }
#endif

private:
    [[nodiscard]] static QString defaultShellForPlatform()
    {
#ifdef Q_OS_WIN
        return QStringLiteral("CMD");
#else
        return QStringLiteral("bash");
#endif
    }

    [[nodiscard]] static QByteArray echoCommand()
    {
#ifdef Q_OS_WIN
        return "echo hssh_test\r\n";
#else
        return "echo hssh_test\n";
#endif
    }

    [[nodiscard]] static QByteArray exitCommand()
    {
#ifdef Q_OS_WIN
        return "exit\r\n";
#else
        return "exit\n";
#endif
    }
};

QTEST_MAIN(TestTerminal)
#include "test_terminal.moc"

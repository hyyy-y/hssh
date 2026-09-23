#include "app/dialogs/SessionLogViewer.h"
#include "core/SessionConfig.h"

#include <QtTest/QtTest>

using namespace hssh;

// B4-2 / PH2-15: pure helpers of the session log viewer plus the
// tags/favorite persistence of SessionConfig.
class TestSessionLog : public QObject {
    Q_OBJECT

private slots:
    void testTimestampFromFileName()
    {
        const QDateTime ts = SessionLogViewer::timestampFromFileName(
            QStringLiteral("session_20260923_141530_123.log"));
        QVERIFY(ts.isValid());
        QCOMPARE(ts.date().year(), 2026);
        QCOMPARE(ts.date().month(), 9);
        QCOMPARE(ts.date().day(), 23);
        QCOMPARE(ts.time().hour(), 14);
        QCOMPARE(ts.time().minute(), 15);
        QCOMPARE(ts.time().second(), 30);
        QCOMPARE(ts.time().msec(), 123);

        // Full path input still works (only the file name is inspected).
        const QDateTime ts2 = SessionLogViewer::timestampFromFileName(
            QStringLiteral("C:/some/dir/session_20260102_030405_006.log"));
        QVERIFY(ts2.isValid());
        QCOMPARE(ts2.date().year(), 2026);

        // Malformed names: refuse rather than guess.
        QVERIFY(!SessionLogViewer::timestampFromFileName(QStringLiteral("agent_audit.log")).isValid());
        QVERIFY(!SessionLogViewer::timestampFromFileName(
                     QStringLiteral("session_20260923.log")).isValid());
        QVERIFY(!SessionLogViewer::timestampFromFileName(QString()).isValid());
    }

    void testStripAnsiSequences()
    {
        // SGR color codes are removed, text survives.
        QCOMPARE(SessionLogViewer::stripAnsi("\x1b[32mOK\x1b[0m"), QStringLiteral("OK"));
        // Cursor movement / erase sequences with parameters.
        QCOMPARE(SessionLogViewer::stripAnsi("A\x1b[2K\rB"), QStringLiteral("A\nB"));
        // OSC title with BEL terminator, and with ST terminator.
        QCOMPARE(SessionLogViewer::stripAnsi("\x1b]0;my title\x07$ "),
                 QStringLiteral("$ "));
        QCOMPARE(SessionLogViewer::stripAnsi("\x1b]2;title\x1b\\$ "),
                 QStringLiteral("$ "));
        // Charset designation ESC ( B.
        QCOMPARE(SessionLogViewer::stripAnsi("\x1b(Bhello"), QStringLiteral("hello"));
        // Two-char escapes (ESC 7, ESC =). NB: string-concatenation breaks
        // are REQUIRED here — "\x1b7" alone would greedily parse as 0x1B7.
        QCOMPARE(SessionLogViewer::stripAnsi("\x1b" "7" "\x1b" "=x"), QStringLiteral("x"));
        // CRLF and lone CR both become LF.
        QCOMPARE(SessionLogViewer::stripAnsi("a\r\nb\rc"), QStringLiteral("a\nb\nc"));
        // Bell / backspace / NUL are dropped, tab kept. Same hex-escape
        // greediness trap: "\x07b" would be 0x07B ('{'), not BEL + 'b'.
        QCOMPARE(SessionLogViewer::stripAnsi("a" "\x07" "b" "\x08" "c" "\x00" "d\te"),
                 QStringLiteral("abcde"));
        // UTF-8 content is preserved byte-exact.
        QCOMPARE(SessionLogViewer::stripAnsi("\x1b[1m中文\x1b[m"),
                 QStringLiteral("中文"));
        // Dangling escape at end of buffer does not read past the end.
        QCOMPARE(SessionLogViewer::stripAnsi("end\x1b"), QStringLiteral("end"));
        QCOMPARE(SessionLogViewer::stripAnsi("end\x1b["), QStringLiteral("end"));
    }

    void testTagsAndFavoriteRoundTrip()
    {
        SessionConfig config;
        QVERIFY(config.tags().isEmpty());
        QVERIFY(!config.favorite());

        const QStringList tags{QStringLiteral("prod"), QStringLiteral("rk3588")};
        config.setTags(tags);
        config.setFavorite(true);
        QCOMPARE(config.tags(), tags);
        QVERIFY(config.favorite());

        const SessionConfig restored = SessionConfig::fromMap(config.toMap());
        QCOMPARE(restored.tags(), config.tags());
        QCOMPARE(restored.favorite(), true);

        // Defaults on an empty map.
        const SessionConfig empty = SessionConfig::fromMap(QVariantMap());
        QVERIFY(empty.tags().isEmpty());
        QVERIFY(!empty.favorite());
    }
};

QTEST_MAIN(TestSessionLog)
#include "test_sessionlog.moc"

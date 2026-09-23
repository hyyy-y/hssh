#include "app/dialogs/RemoteSearchDialog.h"

#include <QtTest/QtTest>

using namespace hssh;

using Criteria = RemoteSearchDialog::Criteria;

// PH3-12: the local filter half of the remote search (the tree walk itself
// needs a live SSH server; it is the same collectRemoteFiles the compare
// pane and directory downloads exercise).
class TestRemoteSearch : public QObject {
    Q_OBJECT

private slots:
    void testEmptyPatternMatchesEverything()
    {
        QList<RemoteFileEntry> entries = {
            {QStringLiteral("/srv/a.log"), QStringLiteral("a.log"), 10, 1000, false},
            {QStringLiteral("/srv/sub"), QStringLiteral("sub"), 0, 900, true},
        };
        Criteria c;
        const QList<RemoteFileEntry> hits = RemoteSearchDialog::filterEntries(entries, c, 2000);
        QCOMPARE(hits.size(), 2);
    }

    void testWildcardNameFilter()
    {
        QList<RemoteFileEntry> entries = {
            {QStringLiteral("/srv/app.log"), QStringLiteral("app.log"), 1, 1000, false},
            {QStringLiteral("/srv/APP.LOG.1"), QStringLiteral("APP.LOG.1"), 1, 1000, false},
            {QStringLiteral("/srv/data.txt"), QStringLiteral("data.txt"), 1, 1000, false},
            {QStringLiteral("/srv/build/src/main.o"), QStringLiteral("build/src/main.o"), 1, 1000, false},
        };

        Criteria c;
        c.namePattern = QStringLiteral("*.log");
        QList<RemoteFileEntry> hits = RemoteSearchDialog::filterEntries(entries, c, 2000);
        QCOMPARE(hits.size(), 1); // case-insensitive, ".LOG.1" has a suffix after .log
        QCOMPARE(hits.first().relPath, QStringLiteral("app.log"));

        // '?' single char wildcard.
        c.namePattern = QStringLiteral("dat?.txt");
        hits = RemoteSearchDialog::filterEntries(entries, c, 2000);
        QCOMPARE(hits.size(), 1);

        // A '/' in the pattern addresses the relative path.
        c.namePattern = QStringLiteral("build/src/*.o");
        hits = RemoteSearchDialog::filterEntries(entries, c, 2000);
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits.first().relPath, QStringLiteral("build/src/main.o"));
    }

    void testSizeFilter()
    {
        QList<RemoteFileEntry> entries = {
            {QStringLiteral("/a"), QStringLiteral("small"), 1024, 1000, false},
            {QStringLiteral("/b"), QStringLiteral("big"), 5 * 1024 * 1024, 1000, false},
            {QStringLiteral("/c"), QStringLiteral("dir"), 0, 1000, true},
        };

        Criteria c;
        c.sizeMode = 1; // at least
        c.sizeBytes = 1024 * 1024;
        QCOMPARE(RemoteSearchDialog::filterEntries(entries, c, 2000).size(), 1); // big only

        c.sizeMode = 2; // at most
        QCOMPARE(RemoteSearchDialog::filterEntries(entries, c, 2000).size(), 1); // small only

        // Size filters exclude directories outright.
        c.sizeMode = 2;
        c.sizeBytes = 1024 * 1024 * 1024;
        QCOMPARE(RemoteSearchDialog::filterEntries(entries, c, 2000).size(), 2);
    }

    void testTimeFilter()
    {
        const qint64 now = 1'000'000;
        QList<RemoteFileEntry> entries = {
            {QStringLiteral("/fresh"), QStringLiteral("fresh"), 1, now - 100, false},
            {QStringLiteral("/stale"), QStringLiteral("stale"), 1, now - 90LL * 86400, false},
        };

        Criteria c;
        c.timeEnabled = true;
        c.days = 7;
        const QList<RemoteFileEntry> hits = RemoteSearchDialog::filterEntries(entries, c, now);
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits.first().relPath, QStringLiteral("fresh"));

        // Disabled time filter keeps everything.
        c.timeEnabled = false;
        QCOMPARE(RemoteSearchDialog::filterEntries(entries, c, now).size(), 2);
    }

    void testCombinedFilters()
    {
        const qint64 now = 500'000;
        QList<RemoteFileEntry> entries = {
            {QStringLiteral("/srv/old.log"), QStringLiteral("old.log"), 100, now - 800 * 86400, false},
            {QStringLiteral("/srv/new.log"), QStringLiteral("new.log"), 10 * 1024 * 1024, now - 10, false},
            {QStringLiteral("/srv/new.log.bak"), QStringLiteral("new.log.bak"), 10 * 1024 * 1024, now - 10, false},
        };
        Criteria c;
        c.namePattern = QStringLiteral("*.log");
        c.sizeMode = 1;
        c.sizeBytes = 1024 * 1024;
        c.timeEnabled = true;
        c.days = 30;
        const QList<RemoteFileEntry> hits = RemoteSearchDialog::filterEntries(entries, c, now);
        QCOMPARE(hits.size(), 1);
        QCOMPARE(hits.first().relPath, QStringLiteral("new.log"));
    }
};

QTEST_MAIN(TestRemoteSearch)
#include "test_remotesearch.moc"

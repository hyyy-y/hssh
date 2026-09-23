#include "app/widgets/CommandPalette.h"
#include "utils/Config.h"

#include <QtTest/QtTest>

#include <functional>

using namespace hssh;

class TestCommandPalette : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qApp->setApplicationName(QStringLiteral("hssh_test_palette"));
        qApp->setOrganizationName(QStringLiteral("hssh_project_test"));
        // Recents persist in the Config store; start from a clean state so
        // the ordering assertions are deterministic.
        Config::instance().remove(QStringLiteral("ui/commandPaletteRecents"));
        Config::instance().sync();
    }

    void cleanupTestCase()
    {
        Config::instance().remove(QStringLiteral("ui/commandPaletteRecents"));
        Config::instance().sync();
    }

    void testFuzzyScore()
    {
        QVERIFY(CommandPalette::fuzzyScore(QStringLiteral("Connect"), QString()) > 0);
        QVERIFY(CommandPalette::fuzzyScore(QStringLiteral("Connect"), QStringLiteral("cnt"))
                 > 0); // subsequence
        QVERIFY(CommandPalette::fuzzyScore(QStringLiteral("Connect"), QStringLiteral("xyz")
                  ) < 0);
        // Prefix matches outrank scattered ones.
        QVERIFY(CommandPalette::fuzzyScore(QStringLiteral("Connect"), QStringLiteral("con"))
                 > CommandPalette::fuzzyScore(QStringLiteral("Disconnect"),
                                              QStringLiteral("con")));
    }

    void testFilterAndRecents()
    {
        CommandPalette palette;
        QList<CommandPalette::Item> items;
        for (const QString &name :
             {QStringLiteral("Key Manager..."), QStringLiteral("Appearance..."),
              QStringLiteral("Settings...")}) {
            CommandPalette::Item item;
            item.id = QStringLiteral("menu:") + name;
            item.title = name;
            item.category = QStringLiteral("Command");
            item.action = []() {};
            items.append(item);
        }
        palette.setItems(items);
        QCOMPARE(palette.resultCount(), 3);

        palette.setFilter(QStringLiteral("key"));
        QCOMPARE(palette.resultCount(), 1);
        QCOMPARE(palette.resultIdAt(0), QStringLiteral("menu:Key Manager..."));

        palette.setFilter(QStringLiteral("zzz"));
        QCOMPARE(palette.resultCount(), 0);

        // Recents boost ordering and persist through the Config store.
        // (Pushed BEFORE filtering — refilter() reads the recents list.)
        CommandPalette::pushRecent(QStringLiteral("menu:Settings..."));
        CommandPalette::pushRecent(QStringLiteral("menu:Appearance..."));
        palette.setFilter(QString());
        QCOMPARE(palette.resultIdAt(0), QStringLiteral("menu:Appearance..."));
        QCOMPARE(palette.resultIdAt(1), QStringLiteral("menu:Settings..."));
        QVERIFY(CommandPalette::recentIds().contains(QStringLiteral("menu:Settings...")));

        // runResult executes and records the recent.
        int ran = 0;
        items.first().action = [&ran]() { ++ran; };
        palette.setItems(items);
        palette.setFilter(QStringLiteral("key man"));
        palette.runResult(0);
        QCOMPARE(ran, 1);
        QCOMPARE(CommandPalette::recentIds().first(), QStringLiteral("menu:Key Manager..."));
    }
};

QTEST_MAIN(TestCommandPalette)
#include "test_palette.moc"

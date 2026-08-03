// Unit tests for the line-based diff used by the compare/diff viewer.

#include "utils/TextDiff.h"

#include <QtTest>

using namespace hssh;

class TestTextDiff : public QObject {
    Q_OBJECT

private slots:
    void identical();
    void emptyInputs();
    void insertion();
    void deletion();
    void replacement();
    void mixed();
    void splitLines_data();
    void splitLines();

private:
    static QStringList tags(const QList<DiffLine> &diff)
    {
        QStringList result;
        for (const DiffLine &line : diff) {
            switch (line.tag) {
            case DiffLine::Tag::Equal:   result.append(QStringLiteral("=") + line.text); break;
            case DiffLine::Tag::Removed: result.append(QStringLiteral("-") + line.text); break;
            case DiffLine::Tag::Added:   result.append(QStringLiteral("+") + line.text); break;
            }
        }
        return result;
    }
};

void TestTextDiff::identical()
{
    const QStringList lines{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
    QCOMPARE(diffLines(lines, lines).size(), 3);
    QCOMPARE(tags(diffLines(lines, lines)), (QStringList{QStringLiteral("=a"), QStringLiteral("=b"), QStringLiteral("=c")}));
}

void TestTextDiff::emptyInputs()
{
    QVERIFY(diffLines({}, {}).isEmpty());
    QCOMPARE(tags(diffLines({}, {QStringLiteral("x")})), QStringList{QStringLiteral("+x")});
    QCOMPARE(tags(diffLines({QStringLiteral("x")}, {})), QStringList{QStringLiteral("-x")});
}

void TestTextDiff::insertion()
{
    QCOMPARE(tags(diffLines({QStringLiteral("a"), QStringLiteral("c")},
                            {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")})),
             QStringList({QStringLiteral("=a"), QStringLiteral("+b"), QStringLiteral("=c")}));
}

void TestTextDiff::deletion()
{
    QCOMPARE(tags(diffLines({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")},
                            {QStringLiteral("a"), QStringLiteral("c")})),
             QStringList({QStringLiteral("=a"), QStringLiteral("-b"), QStringLiteral("=c")}));
}

void TestTextDiff::replacement()
{
    // Removed lines must come before their replacement Added lines.
    QCOMPARE(tags(diffLines({QStringLiteral("a"), QStringLiteral("old"), QStringLiteral("b")},
                            {QStringLiteral("a"), QStringLiteral("new"), QStringLiteral("b")})),
             QStringList({QStringLiteral("=a"), QStringLiteral("-old"), QStringLiteral("+new"), QStringLiteral("=b")}));
}

void TestTextDiff::mixed()
{
    const QStringList oldLines{QStringLiteral("one"), QStringLiteral("two"), QStringLiteral("three"),
                               QStringLiteral("four"), QStringLiteral("five")};
    const QStringList newLines{QStringLiteral("one"), QStringLiteral("2"), QStringLiteral("three"),
                               QStringLiteral("five"), QStringLiteral("six")};
    QCOMPARE(tags(diffLines(oldLines, newLines)),
             QStringList({QStringLiteral("=one"), QStringLiteral("-two"), QStringLiteral("+2"),
                          QStringLiteral("=three"), QStringLiteral("-four"), QStringLiteral("=five"),
                          QStringLiteral("+six")}));
}

void TestTextDiff::splitLines_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QStringList>("expected");

    QTest::newRow("lf") << QStringLiteral("a\nb") << QStringList{QStringLiteral("a"), QStringLiteral("b")};
    QTest::newRow("crlf") << QStringLiteral("a\r\nb") << QStringList{QStringLiteral("a"), QStringLiteral("b")};
    QTest::newRow("cr") << QStringLiteral("a\rb") << QStringList{QStringLiteral("a"), QStringLiteral("b")};
    QTest::newRow("mixed") << QStringLiteral("a\r\nb\nc") << QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
}

void TestTextDiff::splitLines()
{
    QFETCH(QString, input);
    QFETCH(QStringList, expected);
    QCOMPARE(splitIntoLines(input), expected);
}

QTEST_GUILESS_MAIN(TestTextDiff)
#include "test_textdiff.moc"

#include "utils/Config.h"

#include <QCoreApplication>
#include <QtTest/QtTest>

using namespace hssh;

class TestConfig : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qApp->setApplicationName(QStringLiteral("hssh_test"));
        qApp->setOrganizationName(QStringLiteral("hssh_project_test"));
    }

    void testStringRoundTrip()
    {
        Config::instance().setValue(QStringLiteral("test/string"), QStringLiteral("hello"));
        QCOMPARE(Config::instance().stringValue(QStringLiteral("test/string")), QStringLiteral("hello"));
    }

    void testIntRoundTrip()
    {
        Config::instance().setValue(QStringLiteral("test/int"), 42);
        QCOMPARE(Config::instance().intValue(QStringLiteral("test/int")), 42);
    }

    void testBoolRoundTrip()
    {
        Config::instance().setValue(QStringLiteral("test/bool"), true);
        QVERIFY(Config::instance().boolValue(QStringLiteral("test/bool")));
    }

    void testDefaultValue()
    {
        Config::instance().remove(QStringLiteral("test/missing"));
        QCOMPARE(Config::instance().stringValue(QStringLiteral("test/missing"), QStringLiteral("fallback")), QStringLiteral("fallback"));
    }
};

QTEST_MAIN(TestConfig)
#include "test_config.moc"

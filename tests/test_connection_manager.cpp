#include "core/ConnectionManager.h"
#include "core/SshSession.h"

#include <QCoreApplication>
#include <QtTest/QtTest>

using namespace hssh;

class TestConnectionManager : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qApp->setApplicationName(QStringLiteral("hssh_test_conn"));
        qApp->setOrganizationName(QStringLiteral("hssh_project_test"));
    }

    void testCreateAndClose()
    {
        SessionConfig config;
        config.setName(QStringLiteral("Test"));
        config.setHost(QStringLiteral("127.0.0.1"));
        config.setPort(22);
        config.setUsername(QStringLiteral("user"));

        const QString id = ConnectionManager::instance().createSession(config);
        QVERIFY(!id.isEmpty());
        QVERIFY(ConnectionManager::instance().sessionIds().contains(id));

        ConnectionManager::instance().closeSession(id);
        QVERIFY(!ConnectionManager::instance().sessionIds().contains(id));
    }

    void testConfigRoundTrip()
    {
        SessionConfig config;
        config.setName(QStringLiteral("Test2"));
        config.setHost(QStringLiteral("192.168.1.2"));
        config.setPort(2222);
        config.setUsername(QStringLiteral("admin"));

        const QString id = ConnectionManager::instance().createSession(config);
        const SessionConfig retrieved = ConnectionManager::instance().config(id);
        QCOMPARE(retrieved.host(), config.host());
        QCOMPARE(retrieved.port(), config.port());
        QCOMPARE(retrieved.username(), config.username());

        ConnectionManager::instance().closeSession(id);
    }
};

QTEST_MAIN(TestConnectionManager)
#include "test_connection_manager.moc"

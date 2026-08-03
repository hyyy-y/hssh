#include "core/SshSession.h"

#include <QCoreApplication>
#include <QtTest/QtTest>

using namespace hssh;

class TestSshSession : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qApp->setApplicationName(QStringLiteral("hssh_test_ssh"));
        qApp->setOrganizationName(QStringLiteral("hssh_project_test"));
    }

    void testDefaultState()
    {
        SshSession session;
        QCOMPARE(session.state(), SshSession::State::Disconnected);
        QVERIFY(!session.isConnected());
    }

    void testSetConfig()
    {
        SshSession session;

        SessionConfig config;
        config.setName(QStringLiteral("Test"));
        config.setHost(QStringLiteral("127.0.0.1"));
        config.setPort(2222);
        config.setUsername(QStringLiteral("admin"));

        session.setSessionConfig(config);
        QCOMPARE(session.sessionConfig().host(), QStringLiteral("127.0.0.1"));
        QCOMPARE(session.sessionConfig().port(), 2222);
        QCOMPARE(session.sessionConfig().username(), QStringLiteral("admin"));
    }

    void testInvalidConnect()
    {
        SshSession session;
        QSignalSpy errorSpy(&session, &SshSession::errorOccurred);

        session.connectToHost(); // no config set

        QVERIFY(!session.isConnected());
        QCOMPARE(errorSpy.count(), 1);
    }
};

QTEST_MAIN(TestSshSession)
#include "test_sshsession.moc"

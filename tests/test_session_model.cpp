#include "core/SessionModel.h"

#include <QCoreApplication>
#include <QtTest/QtTest>

using namespace hssh;

class TestSessionModel : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<SessionConfig>("SessionConfig");
        qApp->setApplicationName(QStringLiteral("hssh_test"));
        qApp->setOrganizationName(QStringLiteral("hssh_project_test"));
    }

    void testEmptyModel()
    {
        SessionModel model;
        QCOMPARE(model.rowCount({}), 0);
    }

    void testAddFolder()
    {
        SessionModel model;
        const QModelIndex folderIndex = model.addFolder(QStringLiteral("My Servers"));
        QVERIFY(folderIndex.isValid());
        QCOMPARE(model.rowCount({}), 1);
        QCOMPARE(model.data(folderIndex).toString(), QStringLiteral("My Servers"));
        QCOMPARE(model.nodeType(folderIndex), SessionModel::NodeType::Folder);
    }

    void testAddSession()
    {
        SessionModel model;
        SessionConfig config;
        config.setName(QStringLiteral("Home Lab"));
        config.setHost(QStringLiteral("192.168.1.100"));
        config.setUsername(QStringLiteral("admin"));

        const QModelIndex sessionIndex = model.addSession(config);
        QVERIFY(sessionIndex.isValid());
        QCOMPARE(model.rowCount({}), 1);
        QCOMPARE(model.nodeType(sessionIndex), SessionModel::NodeType::Session);
        QCOMPARE(model.data(sessionIndex).toString(), QStringLiteral("Home Lab"));

        const SessionConfig retrieved = model.sessionConfig(sessionIndex);
        QCOMPARE(retrieved.host(), QStringLiteral("192.168.1.100"));
        QCOMPARE(retrieved.username(), QStringLiteral("admin"));
    }

    void testNestedSession()
    {
        SessionModel model;
        const QModelIndex folderIndex = model.addFolder(QStringLiteral("Production"));
        QVERIFY(folderIndex.isValid());

        SessionConfig config;
        config.setHost(QStringLiteral("prod.example.com"));
        const QModelIndex sessionIndex = model.addSession(config, folderIndex);
        QVERIFY(sessionIndex.isValid());

        QCOMPARE(model.rowCount({}), 1);
        QCOMPARE(model.rowCount(folderIndex), 1);
        QCOMPARE(model.parent(sessionIndex), folderIndex);
    }

    void testRemoveNode()
    {
        SessionModel model;
        const QModelIndex sessionIndex = model.addSession(SessionConfig());
        QVERIFY(model.removeNode(sessionIndex));
        QCOMPARE(model.rowCount({}), 0);
    }

    void testRenameNode()
    {
        SessionModel model;
        const QModelIndex sessionIndex = model.addSession(SessionConfig());
        QVERIFY(model.setNodeName(sessionIndex, QStringLiteral("Renamed")));
        QCOMPARE(model.data(sessionIndex).toString(), QStringLiteral("Renamed"));
    }

    void testAllSessions()
    {
        SessionModel model;
        SessionConfig config1;
        config1.setHost(QStringLiteral("host1"));
        SessionConfig config2;
        config2.setHost(QStringLiteral("host2"));

        model.addSession(config1);
        model.addSession(config2);

        QCOMPARE(model.allSessions().size(), 2);
    }
};

QTEST_MAIN(TestSessionModel)
#include "test_session_model.moc"

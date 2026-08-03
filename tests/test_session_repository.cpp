#include "core/SessionRepository.h"

#include "utils/Database.h"

#include <QCoreApplication>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryFile>
#include <memory>
#include <QtTest/QtTest>

using namespace hssh;

class TestSessionRepository : public QObject {
    Q_OBJECT

public:
    TestSessionRepository() = default;
    ~TestSessionRepository() override = default;

private slots:
    void initTestCase()
    {
        qRegisterMetaType<SessionConfig>("SessionConfig");
        qApp->setApplicationName(QStringLiteral("hssh_test_repo"));
        qApp->setOrganizationName(QStringLiteral("hssh_project_test"));
    }

    void init()
    {
        m_tempFile = std::make_unique<QTemporaryFile>(QDir::tempPath() + QStringLiteral("/hssh_test_repo_XXXXXX.db"));
        QVERIFY(m_tempFile->open());
        m_tempFile->close();
        Database::setDefaultDatabasePath(m_tempFile->fileName());
    }

    void cleanup()
    {
        Database::setDefaultDatabasePath(QString());
        m_tempFile.reset();
    }

    void testInitialize()
    {
        SessionRepository repo;
        const bool ok = repo.initialize();
        if (!ok) {
            qDebug() << "Initialize failed:" << repo.lastError();
        }
        QVERIFY(ok);
        QVERIFY(repo.isInitialized());
    }

    void testSaveAndLoadSession()
    {
        SessionRepository repo;
        QVERIFY(repo.initialize());

        SessionConfig config;
        config.setName(QStringLiteral("Test Session"));
        config.setHost(QStringLiteral("192.168.1.1"));
        config.setPort(2222);
        config.setUsername(QStringLiteral("admin"));
        config.setAuthMethod(AuthMethod::PublicKey);
        config.setPrivateKeyPath(QStringLiteral("/home/user/.ssh/id_rsa"));

        QVERIFY(repo.saveSession(config));

        const SessionConfig loaded = repo.loadSession(config.id());
        QCOMPARE(loaded.name(), config.name());
        QCOMPARE(loaded.host(), config.host());
        QCOMPARE(loaded.port(), config.port());
        QCOMPARE(loaded.username(), config.username());
        QCOMPARE(loaded.authMethod(), config.authMethod());
        QCOMPARE(loaded.privateKeyPath(), config.privateKeyPath());
    }

    void testRemoveSession()
    {
        SessionRepository repo;
        QVERIFY(repo.initialize());

        SessionConfig config;
        config.setName(QStringLiteral("ToRemove"));
        config.setHost(QStringLiteral("host"));
        QVERIFY(repo.saveSession(config));

        // Verify saved
        SessionConfig loaded = repo.loadSession(config.id());
        QVERIFY(!loaded.id().isEmpty());

        QVERIFY(repo.removeSession(config.id()));

        // Verify removed via repository API
        loaded = repo.loadSession(config.id());
        QVERIFY(loaded.id().isEmpty());

        // Verify removed via raw SQL
        QSqlQuery query(QSqlDatabase::database(QStringLiteral("hssh_default"), false));
        query.prepare(QStringLiteral("SELECT COUNT(*) FROM sessions WHERE id = :id"));
        query.bindValue(QStringLiteral(":id"), config.id());
        QVERIFY(query.exec() && query.next());
        QCOMPARE(query.value(0).toInt(), 0);
    }

    void testFolderOperations()
    {
        SessionRepository repo;
        QVERIFY(repo.initialize());

        const QString folderId = repo.addFolder(QStringLiteral("Servers"));
        QVERIFY(!folderId.isEmpty());

        QMap<QString, QString> names = repo.folderNames();
        QCOMPARE(names.value(folderId), QStringLiteral("Servers"));

        QVERIFY(repo.updateFolder(folderId, QStringLiteral("Production")));
        names = repo.folderNames();
        QCOMPARE(names.value(folderId), QStringLiteral("Production"));
    }

    void testLoadTree()
    {
        SessionRepository repo;
        QVERIFY(repo.initialize());

        const QString folderId = repo.addFolder(QStringLiteral("Lab"));

        SessionConfig config;
        config.setName(QStringLiteral("Lab Server"));
        config.setHost(QStringLiteral("lab.local"));
        config.setGroup(folderId);
        QVERIFY(repo.saveSession(config));

        const SessionRepository::TreeData data = repo.loadTree();
        QCOMPARE(data.sessions.size(), 1);
        QCOMPARE(data.folderNames.size(), 1);
    }

private:
    std::unique_ptr<QTemporaryFile> m_tempFile;
};

QTEST_MAIN(TestSessionRepository)
#include "test_session_repository.moc"

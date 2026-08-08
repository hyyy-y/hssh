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

    void testKeepAliveAndReconnectFields()
    {
        SessionRepository repo;
        QVERIFY(repo.initialize());

        SessionConfig config;
        config.setName(QStringLiteral("Robust"));
        config.setHost(QStringLiteral("host.example"));
        config.setKeepAliveSeconds(60);
        config.setAutoReconnect(true);
        QVERIFY(repo.saveSession(config));

        const SessionConfig loaded = repo.loadSession(config.id());
        QCOMPARE(loaded.keepAliveSeconds(), 60);
        QVERIFY(loaded.autoReconnect());
    }

    void testPasswordEncryptedAtRest()
    {
        SessionRepository repo;
        QVERIFY(repo.initialize());

        SessionConfig config;
        config.setName(QStringLiteral("Secret"));
        config.setHost(QStringLiteral("host.example"));
        config.setPassword(SecureString(QStringLiteral("p@ssw0rd")));
        QVERIFY(repo.saveSession(config));

        // The raw database value must be an encrypted blob, not plaintext.
        QSqlQuery query(QSqlDatabase::database(QStringLiteral("hssh_default"), false));
        query.prepare(QStringLiteral("SELECT password_encrypted FROM sessions WHERE id = :id"));
        query.bindValue(QStringLiteral(":id"), config.id());
        QVERIFY(query.exec() && query.next());
        const QByteArray stored = query.value(0).toByteArray();
        QVERIFY(stored.startsWith("HSE1"));
        QVERIFY(!stored.contains("p@ssw0rd"));

        // ...and the repository decrypts it transparently on load.
        const SessionConfig loaded = repo.loadSession(config.id());
        QCOMPARE(loaded.password().toString(), QStringLiteral("p@ssw0rd"));
    }

    // Regression: a session without a key passphrase must not have its
    // password re-encrypted by repeated initialize() calls.
    void testMigrationIsIdempotent()
    {
        SessionRepository repo;
        QVERIFY(repo.initialize());

        SessionConfig config;
        config.setName(QStringLiteral("NoPassphrase"));
        config.setHost(QStringLiteral("host.example"));
        config.setPassword(SecureString(QStringLiteral("p@ssw0rd")));
        // Empty key passphrase (the case that broke the first migration).
        QVERIFY(repo.saveSession(config));

        const auto blob = [this, &config]() {
            QSqlQuery query(QSqlDatabase::database(QStringLiteral("hssh_default"), false));
            query.prepare(QStringLiteral("SELECT password_encrypted FROM sessions WHERE id = :id"));
            query.bindValue(QStringLiteral(":id"), config.id());
            return query.exec() && query.next() ? query.value(0).toByteArray() : QByteArray();
        };

        const QByteArray first = blob();
        QVERIFY(first.startsWith("HSE1"));

        // A second initialize (simulating an app restart) must not alter
        // the blob.
        SessionRepository repo2;
        QVERIFY(repo2.initialize());
        QCOMPARE(blob(), first);

        const SessionConfig loaded = repo2.loadSession(config.id());
        QCOMPARE(loaded.password().toString(), QStringLiteral("p@ssw0rd"));
    }

    void testExportImport()
    {
        SessionRepository repo;
        QVERIFY(repo.initialize());

        const QString folderId = repo.addFolder(QStringLiteral("Prod"));
        SessionConfig config;
        config.setName(QStringLiteral("Web"));
        config.setHost(QStringLiteral("web.example"));
        config.setUsername(QStringLiteral("deploy"));
        config.setGroup(folderId);
        QVERIFY(repo.saveSession(config));

        const QString exportPath = m_tempFile->fileName() + QStringLiteral(".export.json");
        QVERIFY(repo.exportSessionsToJson(exportPath));

        // Import into a fresh database.
        Database::setDefaultDatabasePath(QString());
        m_tempFile.reset();
        m_tempFile = std::make_unique<QTemporaryFile>(QDir::tempPath() + QStringLiteral("/hssh_test_import_XXXXXX.db"));
        QVERIFY(m_tempFile->open());
        m_tempFile->close();
        Database::setDefaultDatabasePath(m_tempFile->fileName());

        SessionRepository imported;
        QVERIFY(imported.initialize());
        QVERIFY(imported.importSessionsFromJson(exportPath));

        const SessionRepository::TreeData data = imported.loadTree();
        QCOMPARE(data.sessions.size(), 1);
        QCOMPARE(data.sessions.first().name(), QStringLiteral("Web"));
        QCOMPARE(data.sessions.first().host(), QStringLiteral("web.example"));
        QCOMPARE(data.sessions.first().group(), folderId);
        QCOMPARE(data.folderNames.size(), 1);
        QCOMPARE(data.folderNames.value(folderId), QStringLiteral("Prod"));

        QFile::remove(exportPath);
    }

    void testRemoveFolderCascadesSessions()
    {
        SessionRepository repo;
        QVERIFY(repo.initialize());

        const QString folderId = repo.addFolder(QStringLiteral("Temp"));
        SessionConfig config;
        config.setName(QStringLiteral("Inner"));
        config.setHost(QStringLiteral("inner.local"));
        config.setGroup(folderId);
        QVERIFY(repo.saveSession(config));

        QVERIFY(repo.removeFolder(folderId));
        const SessionRepository::TreeData data = repo.loadTree();
        QCOMPARE(data.sessions.size(), 0);
        QCOMPARE(data.folderNames.size(), 0);
    }

private:
    std::unique_ptr<QTemporaryFile> m_tempFile;
};

QTEST_MAIN(TestSessionRepository)
#include "test_session_repository.moc"

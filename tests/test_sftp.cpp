// Integration test for SftpSession. Requires a reachable SSH/SFTP server:
//   HSSH_TEST_SFTP_HOST, HSSH_TEST_SFTP_PORT (default 22),
//   HSSH_TEST_SFTP_USER, HSSH_TEST_SFTP_PASSWORD
// The whole test is skipped when HSSH_TEST_SFTP_HOST is not set.

#include "core/SftpSession.h"

#include <QDir>
#include <QFile>
#include <QObject>
#include <QTemporaryFile>
#include <QtTest>

using namespace hssh;

class TestSftpSession : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void sftpRoundTrip();

private:
    SessionConfig m_config;
    bool m_enabled = false;
};

void TestSftpSession::initTestCase()
{
    const QByteArray host = qgetenv("HSSH_TEST_SFTP_HOST");
    if (host.isEmpty()) {
        QSKIP("HSSH_TEST_SFTP_HOST not set; skipping SFTP integration test");
    }
    m_enabled = true;
    m_config.setHost(QString::fromUtf8(host));
    m_config.setPort(qgetenv("HSSH_TEST_SFTP_PORT").toInt());
    if (m_config.port() <= 0) {
        m_config.setPort(22);
    }
    m_config.setUsername(QString::fromUtf8(qgetenv("HSSH_TEST_SFTP_USER")));
    m_config.setAuthMethod(AuthMethod::Password);
    m_config.setPassword(SecureString(QString::fromUtf8(qgetenv("HSSH_TEST_SFTP_PASSWORD"))));
}

void TestSftpSession::sftpRoundTrip()
{
    QVERIFY(m_enabled);

    SftpSession sftp(m_config);

    QString homePath;
    QString lastError;
    connect(&sftp, &SftpSession::connected, this, [&homePath](const QString &home) {
        homePath = home;
    });
    connect(&sftp, &SftpSession::errorOccurred, this, [&lastError](const QString &message) {
        lastError = message;
    });

    sftp.start();
    QTRY_VERIFY_WITH_TIMEOUT(!homePath.isEmpty() || !lastError.isEmpty(), 20000);
    QVERIFY2(lastError.isEmpty(), qPrintable(lastError));

    // Directory listing of the home directory must succeed.
    QList<SftpFileInfo> entries;
    QString listedPath;
    connect(&sftp, &SftpSession::dirListed, this,
            [&entries, &listedPath](const QString &path, const QList<SftpFileInfo> &e) {
                listedPath = path;
                entries = e;
            });
    sftp.listDir(homePath);
    QTRY_VERIFY_WITH_TIMEOUT(!listedPath.isEmpty() || !lastError.isEmpty(), 10000);
    QVERIFY2(lastError.isEmpty(), qPrintable(lastError));
    QCOMPARE(listedPath, homePath);

    // mkdir a scratch directory.
    const QString testDir = homePath + QStringLiteral("/hssh_sftp_test");
    QList<bool> opResults;
    connect(&sftp, &SftpSession::operationFinished, this,
            [&opResults](const QString &, bool ok, const QString &) {
                opResults.append(ok);
            });
    sftp.makeDir(testDir);
    QTRY_VERIFY_WITH_TIMEOUT(!opResults.isEmpty(), 10000);
    QVERIFY(opResults.takeFirst());

    // Upload a small file with random-ish content.
    QByteArray content;
    for (int i = 0; i < 5000; ++i) {
        content.append(static_cast<char>((i * 31 + 7) & 0xff));
    }
    QTemporaryFile localFile;
    QVERIFY(localFile.open());
    QCOMPARE(localFile.write(content), qint64(content.size()));
    localFile.flush();

    bool uploadDone = false;
    bool uploadOk = false;
    QString uploadError;
    connect(&sftp, &SftpSession::transferFinished, this,
            [&uploadDone, &uploadOk, &uploadError](const QString &, bool ok, const QString &message) {
                uploadDone = true;
                uploadOk = ok;
                uploadError = message;
            });
    const QString remoteFile = testDir + QStringLiteral("/upload.bin");
    sftp.upload(localFile.fileName(), remoteFile);
    QTRY_VERIFY_WITH_TIMEOUT(uploadDone, 20000);
    QVERIFY2(uploadOk, qPrintable(uploadError));

    // The file must appear in the directory listing.
    listedPath.clear();
    sftp.listDir(testDir);
    QTRY_VERIFY_WITH_TIMEOUT(!listedPath.isEmpty(), 10000);
    bool found = false;
    for (const SftpFileInfo &entry : entries) {
        if (entry.name == QStringLiteral("upload.bin")) {
            found = true;
            QCOMPARE(entry.size, qint64(content.size()));
        }
    }
    QVERIFY(found);

    // Download it back and compare contents.
    QTemporaryFile downloadFile;
    QVERIFY(downloadFile.open());
    downloadFile.close();
    uploadDone = false;
    sftp.download(remoteFile, downloadFile.fileName());
    QTRY_VERIFY_WITH_TIMEOUT(uploadDone, 20000);
    QVERIFY(uploadOk);
    QFile check(downloadFile.fileName());
    QVERIFY(check.open(QIODevice::ReadOnly));
    QCOMPARE(check.readAll(), content);

    // Rename, then delete file and directory.
    opResults.clear();
    sftp.renameEntry(remoteFile, testDir + QStringLiteral("/renamed.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(!opResults.isEmpty(), 10000);
    QVERIFY(opResults.takeFirst());

    opResults.clear();
    sftp.removeFile(testDir + QStringLiteral("/renamed.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(!opResults.isEmpty(), 10000);
    QVERIFY(opResults.takeFirst());

    opResults.clear();
    sftp.removeDir(testDir);
    QTRY_VERIFY_WITH_TIMEOUT(!opResults.isEmpty(), 10000);
    QVERIFY(opResults.takeFirst());

    sftp.stop();
}

QTEST_GUILESS_MAIN(TestSftpSession)
#include "test_sftp.moc"

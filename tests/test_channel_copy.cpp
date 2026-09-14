// Integration test for ChannelCopySession (base64-over-exec and scp).
// Requires a reachable SSH server with a POSIX shell and `base64` (and
// `scp` for the scp half):
//   HSSH_TEST_SFTP_HOST, HSSH_TEST_SFTP_PORT (default 22),
//   HSSH_TEST_SFTP_USER, HSSH_TEST_SFTP_PASSWORD
// The whole test is skipped when HSSH_TEST_SFTP_HOST is not set.

#include "core/ChannelCopySession.h"

#include <QDir>
#include <QFile>
#include <QObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace hssh;

class TestChannelCopy : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void base64RoundTrip();
    void scpRoundTrip();

private:
    // Uploads content via `mode`, downloads it back and compares bytes.
    void roundTrip(ChannelCopySession::Mode mode);

    SessionConfig m_config;
    bool m_enabled = false;
    QTemporaryDir m_dir;
};

void TestChannelCopy::initTestCase()
{
    const QByteArray host = qgetenv("HSSH_TEST_SFTP_HOST");
    if (host.isEmpty()) {
        QSKIP("HSSH_TEST_SFTP_HOST not set; skipping channel-copy integration test");
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
    QVERIFY(m_dir.isValid());
}

void TestChannelCopy::roundTrip(ChannelCopySession::Mode mode)
{
    QVERIFY(m_enabled);

    ChannelCopySession worker(m_config, mode);
    QString lastError;
    connect(&worker, &ChannelCopySession::errorOccurred, this,
            [&lastError](const QString &message) { lastError = message; });
    worker.start();
    QTest::qWait(300); // let the async connect get going

    // ~200 KB: big enough to exercise many chunks, small enough for a fast test.
    QByteArray content;
    content.reserve(200 * 1024);
    for (int i = 0; i < 200 * 1024; ++i) {
        content.append(static_cast<char>((i * 31 + 7) & 0xff));
    }
    const QString localUp = m_dir.filePath(QStringLiteral("up.bin"));
    QFile up(localUp);
    QVERIFY(up.open(QIODevice::WriteOnly));
    QCOMPARE(up.write(content), qint64(content.size()));
    up.close();

    const QString remoteFile = QStringLiteral("/tmp/hssh_channelcopy_test.bin");

    bool done = false;
    bool ok = false;
    QString message;
    connect(&worker, &ChannelCopySession::transferFinished, this,
            [&done, &ok, &message](const QString &, bool o, const QString &m) {
                done = true;
                ok = o;
                message = m;
            });

    worker.upload(localUp, remoteFile, true);
    QTRY_VERIFY_WITH_TIMEOUT(done || !lastError.isEmpty(), 60000);
    QVERIFY2(lastError.isEmpty(), qPrintable(lastError));
    QVERIFY2(ok, qPrintable(message));
    // Success note names the method and carries the md5.
    QVERIFY2(message.contains(QStringLiteral("md5")), qPrintable(message));

    const QString localDown = m_dir.filePath(QStringLiteral("down.bin"));
    done = false;
    worker.download(remoteFile, localDown, true);
    QTRY_VERIFY_WITH_TIMEOUT(done || !lastError.isEmpty(), 60000);
    QVERIFY2(lastError.isEmpty(), qPrintable(lastError));
    QVERIFY2(ok, qPrintable(message));

    QFile check(localDown);
    QVERIFY(check.open(QIODevice::ReadOnly));
    QCOMPARE(check.readAll(), content);

    // Cleanup the remote scratch file via a base64-mode exec is overkill;
    // leave /tmp/hssh_channelcopy_test.bin (tmpfs, wiped on reboot).
    worker.stop();
}

void TestChannelCopy::base64RoundTrip()
{
    roundTrip(ChannelCopySession::Mode::Base64);
}

void TestChannelCopy::scpRoundTrip()
{
    roundTrip(ChannelCopySession::Mode::Scp);
}

QTEST_GUILESS_MAIN(TestChannelCopy)
#include "test_channel_copy.moc"

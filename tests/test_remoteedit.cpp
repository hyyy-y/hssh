#include "app/RemoteEditManager.h"

#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using namespace hssh;

// PH3-11: RemoteEditManager lock/watch/debounce semantics. No SFTP involved
// — the widget drives the very same API in production.
class TestRemoteEdit : public QObject {
    Q_OBJECT

private slots:
    void testLockConflict()
    {
        RemoteEditManager &mgr = RemoteEditManager::instance();
        const QString session = QStringLiteral("user@10.0.0.1:22");
        const QString remote = QStringLiteral("/etc/app.conf");
        const QString local = m_dir.filePath(QStringLiteral("app.conf"));
        QVERIFY(QFile(local).open(QIODevice::WriteOnly));

        QString conflict;
        QVERIFY(mgr.startEdit(session, remote, local, 0644, &conflict));
        QVERIFY(conflict.isEmpty());
        QVERIFY(mgr.isEdited(session, remote));

        // Same session + same remote path: the lock refuses a second editor.
        QVERIFY(!mgr.startEdit(session, remote, m_dir.filePath(QStringLiteral("other")),
                               0644, &conflict));
        QCOMPARE(conflict, QStringLiteral("user@10.0.0.1:22 /etc/app.conf"));

        // A different file (or a different session) is unaffected.
        QVERIFY(mgr.startEdit(session, QStringLiteral("/etc/other.conf"),
                              m_dir.filePath(QStringLiteral("other.conf")), 0644));
        QVERIFY(mgr.startEdit(QStringLiteral("user@10.0.0.2:22"), remote,
                              m_dir.filePath(QStringLiteral("app2.conf")), 0644));

        // endEditsForSession drops exactly that session's edits.
        mgr.endEditsForSession(session);
        QVERIFY(!mgr.isEdited(session, remote));
        QVERIFY(!mgr.isEdited(session, QStringLiteral("/etc/other.conf")));
        QVERIFY(mgr.isEdited(QStringLiteral("user@10.0.0.2:22"), remote));
        mgr.endEdit(QStringLiteral("user@10.0.0.2:22"), remote);
        QVERIFY(!mgr.isEdited(QStringLiteral("user@10.0.0.2:22"), remote));
    }

    void testSaveTriggersUploadAfterDebounce()
    {
        RemoteEditManager &mgr = RemoteEditManager::instance();
        const QString session = QStringLiteral("s");
        const QString remote = QStringLiteral("/tmp/f.txt");
        const QString local = m_dir.filePath(QStringLiteral("f.txt"));
        QVERIFY(writeAll(local, "one"));

        QString conflict;
        QVERIFY(mgr.startEdit(session, remote, local, 0600, &conflict));
        mgr.beginWatch(local); // download finished in production

        QSignalSpy savedSpy(&mgr, &RemoteEditManager::editSaved);
        writeAll(local, "two");
        // Two quick saves (format-on-save style) must merge into ONE upload.
        writeAll(local, "two+");

        QVERIFY(savedSpy.wait(3000));
        QTest::qWait(300); // let any spurious extra signal surface
        QCOMPARE(savedSpy.count(), 1);
        QCOMPARE(savedSpy.at(0).at(0).toString(), session);
        QCOMPARE(savedSpy.at(0).at(1).toString(), remote);
        QCOMPARE(savedSpy.at(0).at(2).toString(), local);
        QCOMPARE(savedSpy.at(0).at(3).toUInt(), quint32(0600));

        // markSynced (the upload-back finished): same mtime must not
        // re-trigger; a real new save must.
        mgr.markSynced(local);
        QTest::qWait(800);
        QCOMPARE(savedSpy.count(), 1);

        writeAll(local, "three");
        QVERIFY(savedSpy.wait(3000));
        QCOMPARE(savedSpy.count(), 2);

        mgr.endEdit(session, remote);
    }

    void testDeletedLocalFileEndsEdit()
    {
        RemoteEditManager &mgr = RemoteEditManager::instance();
        const QString session = QStringLiteral("s2");
        const QString remote = QStringLiteral("/tmp/g.txt");
        const QString local = m_dir.filePath(QStringLiteral("g.txt"));
        QVERIFY(writeAll(local, "x"));

        QVERIFY(mgr.startEdit(session, remote, local, 0644));
        mgr.beginWatch(local);

        QSignalSpy goneSpy(&mgr, &RemoteEditManager::editGone);
        QFile(local).remove();
        QVERIFY(goneSpy.wait(3000));
        QCOMPARE(goneSpy.at(0).at(0).toString(), session);
        QCOMPARE(goneSpy.at(0).at(1).toString(), remote);
        QVERIFY(!mgr.isEdited(session, remote));
    }

private:
    static bool writeAll(const QString &path, const QByteArray &content)
    {
        for (int attempt = 0; attempt < 3; ++attempt) {
            QFile f(path);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(content);
                f.close();
                // QFileSystemWatcher is mtime-granular on some filesystems:
                // force a distinct timestamp so the change is visible.
                QThread::msleep(30);
                return true;
            }
            QThread::msleep(50);
        }
        return false;
    }

    QTemporaryDir m_dir;
};

QTEST_MAIN(TestRemoteEdit)
#include "test_remoteedit.moc"

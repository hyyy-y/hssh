#include "core/KeyStore.h"
#include "app/dialogs/KeyManagerDialog.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>

using namespace hssh;

class TestKeyStore : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QCoreApplication::setApplicationName(QStringLiteral("hssh_test_keystore"));
        QCoreApplication::setOrganizationName(QStringLiteral("hssh_project_test"));
        // Start from a clean key store.
        QDir dir(KeyStore::keysDir());
        dir.removeRecursively();
        QDir().mkpath(KeyStore::keysDir());
    }

    void testGenerateEd25519()
    {
        QString path;
        QString error;
        QVERIFY2(KeyStore::generateKey(KeyStore::KeyType::Ed25519,
                                       QStringLiteral("test_ed25519"), QString(), &path, &error),
                 qPrintable(error));
        QVERIFY(QFile::exists(path));
        QVERIFY(QFile::exists(path + QStringLiteral(".pub")));
        QCOMPARE(QFileInfo(path).fileName(), QStringLiteral("test_ed25519"));

        const QString pub = readFile(path + QStringLiteral(".pub"));
        QVERIFY2(pub.startsWith(QStringLiteral("ssh-ed25519 ")), qPrintable(pub));

        const auto keys = KeyStore::listKeys();
        const auto it = std::find_if(keys.begin(), keys.end(),
                                     [](const KeyStore::KeyInfo &k) {
                                         return k.fileName == QStringLiteral("test_ed25519");
                                     });
        QVERIFY(it != keys.end());
        QVERIFY(!it->encrypted);
        QCOMPARE(it->type, QStringLiteral("ssh-ed25519"));
        QVERIFY(it->fingerprintSha256.startsWith(QStringLiteral("SHA256:")));
        // OpenSSH style: unpadded base64 (no trailing '=' allowed after strip).
        QVERIFY(!it->fingerprintSha256.endsWith(QLatin1Char('=')));
    }

    void testGenerateRsa()
    {
        // Known limitation: the libssh mbedTLS backend stubs
        // pki_private_key_to_pem() (pki_mbedcrypto.c), so private-key export
        // only works for Ed25519 (openssh-container path). If that ever
        // changes, this test verifies the RSA path end to end.
        QString path;
        QString error;
        if (!KeyStore::generateKey(KeyStore::KeyType::Rsa2048,
                                   QStringLiteral("test_rsa"), QString(), &path, &error)) {
            QSKIP("RSA private-key export unsupported by the mbedTLS libssh backend");
        }
        const auto keys = KeyStore::listKeys();
        for (const KeyStore::KeyInfo &k : keys) {
            if (k.fileName == QStringLiteral("test_rsa")) {
                QCOMPARE(k.type, QStringLiteral("ssh-rsa"));
                return;
            }
        }
        QVERIFY2(false, "test_rsa not listed");
    }

    void testGenerateSanitizesAndUniquifiesNames()
    {
        QString first;
        QString second;
        QString error;
        QVERIFY(KeyStore::generateKey(KeyStore::KeyType::Ed25519,
                                     QStringLiteral("../weird name<>"), QString(), &first, &error));
        QVERIFY(KeyStore::generateKey(KeyStore::KeyType::Ed25519,
                                     QStringLiteral("../weird name<>"), QString(), &second, &error));
        QVERIFY(first != second);
        QVERIFY(QFileInfo(first).fileName() != QFileInfo(second).fileName());
        // The stored file name must contain no path separators.
        QVERIFY(!QFileInfo(first).fileName().contains(QLatin1Char('/')));
        QVERIFY(!QFileInfo(first).fileName().contains(QLatin1Char('\\')));
    }

    void testEncryptedKey()
    {
        QString path;
        QString error;
        QVERIFY2(KeyStore::generateKey(KeyStore::KeyType::Ed25519,
                                       QStringLiteral("test_locked"), QStringLiteral("secret123"),
                                       &path, &error),
                 qPrintable(error));

        for (const KeyStore::KeyInfo &k : KeyStore::listKeys()) {
            if (k.fileName == QStringLiteral("test_locked")) {
                QVERIFY2(k.encrypted, "passphrase-protected key must be listed as encrypted");
            }
        }

        QString line;
        QVERIFY2(!KeyStore::exportPublicKey(path, QStringLiteral("wrong"), &line, &error),
                 "wrong passphrase must fail");
        QVERIFY2(KeyStore::exportPublicKey(path, QStringLiteral("secret123"), &line, &error),
                 qPrintable(error));
        QVERIFY(line.startsWith(QStringLiteral("ssh-ed25519 ")));
    }

    void testImportKeyFile()
    {
        QString path;
        QString error;
        QVERIFY(KeyStore::generateKey(KeyStore::KeyType::Ed25519,
                                      QStringLiteral("test_source"), QString(), &path, &error));

        // Copy the source outside the key store, then import it back under a
        // new name.
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        const QString external = tempDir.path() + QStringLiteral("/my_external_key");
        QVERIFY(QFile::copy(path, external));

        QString imported;
        QVERIFY2(KeyStore::importKeyFile(external, QString(), &imported, &error), qPrintable(error));
        QVERIFY(QFile::exists(imported));
        QVERIFY(imported.startsWith(KeyStore::keysDir()));
        QCOMPARE(QFileInfo(imported).fileName(), QStringLiteral("my_external_key"));

        // Wrong passphrase must be rejected before anything is copied.
        const QString locked = tempDir.path() + QStringLiteral("/locked_key");
        QString genError;
        QVERIFY(KeyStore::generateKey(KeyStore::KeyType::Ed25519,
                                      QStringLiteral("import_locked_src"),
                                      QStringLiteral("pass1"), &path, &genError));
        QVERIFY(QFile::copy(path, locked));
        QString badImport;
        QVERIFY(!KeyStore::importKeyFile(locked, QStringLiteral("nope"), &badImport, &error));
        QVERIFY(badImport.isEmpty());
    }

    void testDeleteKey()
    {
        QString path;
        QString error;
        QVERIFY(KeyStore::generateKey(KeyStore::KeyType::Ed25519,
                                      QStringLiteral("test_delete_me"), QString(), &path, &error));
        QVERIFY(QFile::exists(path));
        QVERIFY(QFile::exists(path + QStringLiteral(".pub")));
        QVERIFY2(KeyStore::deleteKey(path, &error), qPrintable(error));
        QVERIFY(!QFile::exists(path));
        QVERIFY(!QFile::exists(path + QStringLiteral(".pub")));

        // Files outside the key store must not be deletable.
        QTemporaryDir tempDir;
        const QString outside = tempDir.path() + QStringLiteral("/outside_key");
        QFile f(outside);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
        QVERIFY(!KeyStore::deleteKey(outside));
        QVERIFY(QFile::exists(outside));
    }

    void testDialogConstructs()
    {
        // Smoke: the dialog builds, populates from the key store and paints.
        KeyManagerDialog dialog;
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QTest::qWait(50);
        dialog.close();
    }

    // PH2-12: known_hosts listing and removal against a hand-crafted file.
    void testKnownHostsListAndRemove()
    {
        // A real key line (listKnownHosts computes the fingerprint from it).
        QString keyPath;
        QString genError;
        QVERIFY2(KeyStore::generateKey(KeyStore::KeyType::Ed25519,
                                       QStringLiteral("kh_probe"), QString(), &keyPath,
                                       &genError),
                 qPrintable(genError));
        QFile pubFile(keyPath + QStringLiteral(".pub"));
        QVERIFY2(pubFile.open(QIODevice::ReadOnly | QIODevice::Text), "no .pub sibling");
        const QString pubLine = QString::fromUtf8(pubFile.readLine()).simplified();
        pubFile.close();
        QVERIFY(pubLine.startsWith(QStringLiteral("ssh-ed25519 ")));

        const QString path = KeyStore::knownHostsPath();
        QDir().mkpath(QFileInfo(path).absolutePath());
        {
            QFile hosts(path);
            QVERIFY2(hosts.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text),
                     "cannot write known_hosts");
            hosts.write(QStringLiteral("host1,192.168.1.1 %1\n").arg(pubLine).toUtf8());
            hosts.write(QStringLiteral("[host2]:2222 %1\n").arg(pubLine).toUtf8());
            hosts.write(QStringLiteral("# a comment line\n").toUtf8());
            hosts.close();
        }

        const QVector<KeyStore::KnownHostEntry> entries = KeyStore::listKnownHosts();
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).hosts, QStringLiteral("host1,192.168.1.1"));
        QCOMPARE(entries.at(0).keyType, QStringLiteral("ssh-ed25519"));
        QVERIFY2(entries.at(0).fingerprintSha256.startsWith(QStringLiteral("SHA256:")),
                 "fingerprint missing (pubkey import failed?)");
        QCOMPARE(entries.at(1).hosts, QStringLiteral("[host2]:2222"));

        // Prefix removal keeps non-matching entries.
        QVERIFY2(KeyStore::removeKnownHost(QStringLiteral("host1")), "remove failed");
        const QVector<KeyStore::KnownHostEntry> rest = KeyStore::listKnownHosts();
        QCOMPARE(rest.size(), 1);
        QCOMPARE(rest.at(0).hosts, QStringLiteral("[host2]:2222"));

        // cleanup
        QVERIFY(QFile::remove(path));
        KeyStore::deleteKey(keyPath);
    }

private:
    [[nodiscard]] static QString readFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return {};
        }
        return QString::fromUtf8(file.readAll());
    }
};

QTEST_MAIN(TestKeyStore)
#include "test_keystore.moc"

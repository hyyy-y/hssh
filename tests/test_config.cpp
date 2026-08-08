#include "utils/Config.h"

#include "utils/Crypto.h"

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

    void cleanupTestCase()
    {
        Crypto::clearMasterPassword();
        Config::instance().remove(QStringLiteral("security/masterEnabled"));
        Config::instance().remove(QStringLiteral("security/verifier"));
        Config::instance().sync();
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

    void testCryptoRoundTrip()
    {
        const QByteArray secret("s3cret-password!");
        const QByteArray blob = Crypto::encryptData(secret);
        QVERIFY(blob.startsWith("HSE1"));
        QVERIFY(!blob.contains(secret));

        bool ok = false;
        const QByteArray plain = Crypto::decryptData(blob, &ok);
        QVERIFY(ok);
        QCOMPARE(plain, secret);
    }

    void testCryptoTamperDetection()
    {
        QByteArray blob = Crypto::encryptData(QByteArray("topsecret"));
        QVERIFY(blob.startsWith("HSE1"));
        blob[blob.size() / 2] = blob.at(blob.size() / 2) ^ 0x01;

        bool ok = true;
        Crypto::decryptData(blob, &ok);
        QVERIFY(!ok);
    }

    void testCryptoLegacyPlaintext()
    {
        bool ok = false;
        const QByteArray plain = Crypto::decryptData(QByteArray("legacy-plain"), &ok);
        QVERIFY(ok);
        QCOMPARE(plain, QByteArray("legacy-plain"));
    }

    void testCryptoMasterPassword()
    {
        QVERIFY(Crypto::setupMasterPassword(QStringLiteral("master-123")));
        QVERIFY(Crypto::usesMasterPassword());
        QVERIFY(Crypto::isUnlocked());

        const QByteArray secret("machine-independent");
        const QByteArray blob = Crypto::encryptData(secret);
        QVERIFY(blob.startsWith("HSE1"));

        // Wrong password must not unlock.
        Crypto::lock();
        QVERIFY(!Crypto::isUnlocked());
        QVERIFY(!Crypto::unlock(QStringLiteral("wrong")));
        QVERIFY(!Crypto::isUnlocked());

        // The verifier accepts only the real password; unlock and round-trip.
        QVERIFY(Crypto::unlock(QStringLiteral("master-123")));
        bool ok = false;
        QCOMPARE(Crypto::decryptData(blob, &ok), secret);
        QVERIFY(ok);

        Crypto::clearMasterPassword();
        QVERIFY(!Crypto::usesMasterPassword());
    }

    void testRsaKeyPairRoundTrip()
    {
        QVERIFY(Crypto::ensureAgentKeyPair());
        const QByteArray pub = Crypto::agentPublicKeyPem();
        QVERIFY(pub.startsWith("-----BEGIN PUBLIC KEY-----"));
        QVERIFY(!Crypto::agentKeyFingerprint().isEmpty());

        const QByteArray secret("cipher-secret-123");
        bool ok = false;
        const QByteArray cipher = Crypto::rsaEncryptWithPublicKey(secret, pub, &ok);
        QVERIFY(ok);
        QVERIFY(!cipher.isEmpty());
        QVERIFY(!cipher.contains(secret));

        const QByteArray plain = Crypto::rsaDecrypt(cipher, &ok);
        QVERIFY(ok);
        QCOMPARE(plain, secret);
    }

    void testRsaDecryptRejectsGarbage()
    {
        QVERIFY(Crypto::ensureAgentKeyPair());
        bool ok = true;
        Crypto::rsaDecrypt(QByteArray("not-valid-base64!!!"), &ok);
        QVERIFY(!ok);

        ok = true;
        Crypto::rsaDecrypt(QByteArray("YmxhYmxhYmxhYmxhYmxhYmxhYmxhYmxh"), &ok);
        QVERIFY(!ok);
    }
};

QTEST_MAIN(TestConfig)
#include "test_config.moc"

#include "app/dialogs/PermissionsDialog.h"

#include <QLabel>
#include <QLineEdit>
#include <QtTest/QtTest>

using namespace hssh;

// PH3-14: the pure half of the permission editor (octal parsing and
// symbolic rendering); the checkbox/octal sync drives the same helpers.
class TestPermissions : public QObject {
    Q_OBJECT

private slots:
    void testParseOctal()
    {
        quint32 mode = 0;
        QVERIFY(PermissionsDialog::parseOctal(QStringLiteral("644"), &mode));
        QCOMPARE(mode, quint32(0644));
        QVERIFY(PermissionsDialog::parseOctal(QStringLiteral("755"), &mode));
        QCOMPARE(mode, quint32(0755));
        QVERIFY(PermissionsDialog::parseOctal(QStringLiteral("000"), &mode));
        QCOMPARE(mode, quint32(0));
        QVERIFY(PermissionsDialog::parseOctal(QStringLiteral("777"), &mode));
        QCOMPARE(mode, quint32(0777));
        // Whitespace is tolerated.
        QVERIFY(PermissionsDialog::parseOctal(QStringLiteral(" 600 "), &mode));
        QCOMPARE(mode, quint32(0600));

        // Rejects non-octal / out-of-range / partial input.
        QVERIFY(!PermissionsDialog::parseOctal(QStringLiteral("6444"), &mode));
        QVERIFY(!PermissionsDialog::parseOctal(QStringLiteral("888"), &mode));
        QVERIFY(!PermissionsDialog::parseOctal(QStringLiteral("64"), &mode));
        QVERIFY(!PermissionsDialog::parseOctal(QStringLiteral("abc"), &mode));
        QVERIFY(!PermissionsDialog::parseOctal(QString(), &mode));
        QVERIFY(!PermissionsDialog::parseOctal(QStringLiteral("-wx"), &mode));
    }

    void testSymbolicFromMode()
    {
        QCOMPARE(PermissionsDialog::symbolicFromMode(0644), QStringLiteral("rw-r--r--"));
        QCOMPARE(PermissionsDialog::symbolicFromMode(0755), QStringLiteral("rwxr-xr-x"));
        QCOMPARE(PermissionsDialog::symbolicFromMode(0), QStringLiteral("---------"));
        QCOMPARE(PermissionsDialog::symbolicFromMode(0600), QStringLiteral("rw-------"));
        QCOMPARE(PermissionsDialog::symbolicFromMode(0111), QStringLiteral("--x--x--x"));
        // Bits above 0777 are ignored by the mask in setCurrentMode; the
        // renderer itself only looks at the 9 low bits.
        QCOMPARE(PermissionsDialog::symbolicFromMode(0100644 & 0777), QStringLiteral("rw-r--r--"));
    }

    void testDialogRoundTrip()
    {
        SftpFileInfo info;
        info.name = QStringLiteral("deploy.sh");
        info.isDir = false;
        info.permissions = 0100755; // regular file + rwxr-xr-x

        PermissionsDialog dialog(QStringLiteral("/srv/deploy.sh"), info, nullptr);

        // The octal box starts from the incoming mode (masked to 0777).
        QLineEdit *octal = dialog.findChild<QLineEdit *>();
        QVERIFY(octal);
        QCOMPARE(octal->text(), QStringLiteral("755"));

        // Typing a complete octal (real key events: textEdited, not the
        // programmatic setText path) re-renders the symbolic form.
        octal->clear();
        QTest::keyClicks(octal, QStringLiteral("600"));
        const QList<QLabel *> labels = dialog.findChildren<QLabel *>();
        bool sawSymbolic = false;
        for (QLabel *label : labels) {
            if (label->text() == QStringLiteral("rw-------")) {
                sawSymbolic = true;
            }
        }
        QVERIFY(sawSymbolic);
    }
};

QTEST_MAIN(TestPermissions)
#include "test_permissions.moc"

#include "terminal/LocalShellProcess.h"
#include "terminal/TerminalWidget.h"

#include <QApplication>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QtTest/QtTest>

using namespace hssh;

class TestTerminal : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qApp->setApplicationName(QStringLiteral("hssh_test_terminal"));
        qApp->setOrganizationName(QStringLiteral("hssh_project_test"));
    }

    void testTerminalWidgetFeedData()
    {
        TerminalWidget widget;
        widget.show();
        QTest::qWaitForWindowExposed(&widget);

        QVERIFY(widget.columns() > 0);
        QVERIFY(widget.rows() > 0);

        // Feeding plain text should not crash and the widget should remain valid.
        widget.feedData("hello world\r\n");
        QTest::qWait(100);
        QVERIFY(widget.columns() > 0);
    }

    void testLocalShellProcessLifecycle()
    {
        const QString shell = defaultShellForPlatform();
        LocalShellProcess process(shell);
        QSignalSpy dataSpy(&process, &LocalShellProcess::dataReceived);
        QSignalSpy errorSpy(&process, &LocalShellProcess::errorOccurred);

        QVERIFY2(process.start(),
                 errorSpy.isEmpty() ? "process.start() failed" : errorSpy.first().first().toString().toUtf8().constData());
        QVERIFY(process.isRunning());

        // Give the shell a moment to print its prompt.
        QTest::qWait(500);

        process.write(echoCommand());

        // Wait for the echoed text to appear in the output.
        QByteArray allOutput;
        bool foundEcho = false;
        for (int i = 0; i < 100 && !foundEcho; ++i) {
            QTest::qWait(100);
            allOutput.clear();
            for (int j = 0; j < dataSpy.size(); ++j) {
                allOutput.append(dataSpy.at(j).at(0).toByteArray());
            }
            foundEcho = allOutput.contains("hssh_test");
        }
        qDebug() << "PTY output:" << allOutput;
        QVERIFY(foundEcho);

        process.close();
        QTRY_VERIFY_WITH_TIMEOUT(!process.isRunning(), 3000);
    }

    void testLocalShellProcessResize()
    {
        const QString shell = defaultShellForPlatform();
        LocalShellProcess process(shell);
        QVERIFY(process.start());
        QVERIFY(process.isRunning());

        // Resize should not crash. The PTY will forward SIGWINCH (Unix) or
        // equivalent (ConPTY) to the child.
        process.resize(120, 40);
        QTest::qWait(100);
        QVERIFY(process.isRunning());

        process.close();
        QTRY_VERIFY_WITH_TIMEOUT(!process.isRunning(), 3000);
    }

    void testLocalShellProcessGracefulExit()
    {
        const QString shell = defaultShellForPlatform();
        LocalShellProcess process(shell);
        QSignalSpy finishedSpy(&process, &LocalShellProcess::finished);

        QVERIFY(process.start());
        process.write(exitCommand());

        QTRY_VERIFY_WITH_TIMEOUT(!finishedSpy.isEmpty(), 5000);
    }

    void testLocalShellProcessCloseKill()
    {
        const QString shell = defaultShellForPlatform();
        LocalShellProcess process(shell);
        QVERIFY(process.start());
        QVERIFY(process.isRunning());

        process.close();
        QTRY_VERIFY_WITH_TIMEOUT(!process.isRunning(), 5000);
    }

    void testAvailableShells()
    {
        const QStringList shells = LocalShellProcess::availableShells();
        QVERIFY(!shells.isEmpty());

        const QString def = LocalShellProcess::defaultShell();
        QVERIFY(!def.isEmpty());
        QVERIFY(!LocalShellProcess::shellExecutable(def).isEmpty());
    }

#ifdef HSSH_HAS_LIBVTERM
    void testDsrReplyFlushedOnFeedData()
    {
        TerminalWidget widget;
        widget.show();
        QTest::qWaitForWindowExposed(&widget);

        QSignalSpy sendSpy(&widget, &TerminalWidget::dataToSend);

        // Cursor position request (readline sends \x1b[6n when redrawing the
        // prompt). The terminal must reply \x1b[<row>;<col>R even when input
        // is driven through the API (no key events), or readline's line
        // redraws misplace content and the display glues lines together.
        widget.feedData(QByteArray("\x1b[6n"));
        QVERIFY(!sendSpy.isEmpty());
        const QByteArray reply = sendSpy.takeFirst().at(0).toByteArray();
        QVERIFY2(reply.startsWith("\x1b["), reply.toHex().constData());
        QVERIFY2(reply.endsWith("R"), reply.toHex().constData());
    }

    void testCtrlKeySequences()
    {
        TerminalWidget widget;
        widget.show();
        QTest::qWaitForWindowExposed(&widget);

        QSignalSpy sendSpy(&widget, &TerminalWidget::dataToSend);

        // Qt delivers Ctrl+letter with the control character in text();
        // the terminal must emit the raw control byte (0x03 / 0x04), not a
        // libvterm CSI u sequence that the remote shell can't parse.
        QKeyEvent ctrlC(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, QString(QChar(0x03)));
        QApplication::sendEvent(&widget, &ctrlC);
        QVERIFY(!sendSpy.isEmpty());
        QCOMPARE(sendSpy.takeFirst().at(0).toByteArray(), QByteArray(1, '\x03'));

        QKeyEvent ctrlD(QEvent::KeyPress, Qt::Key_D, Qt::ControlModifier, QString(QChar(0x04)));
        QApplication::sendEvent(&widget, &ctrlD);
        QVERIFY(!sendSpy.isEmpty());
        QCOMPARE(sendSpy.takeFirst().at(0).toByteArray(), QByteArray(1, '\x04'));
    }
#endif

private:
    [[nodiscard]] static QString defaultShellForPlatform()
    {
#ifdef Q_OS_WIN
        return QStringLiteral("CMD");
#else
        return QStringLiteral("bash");
#endif
    }

    [[nodiscard]] static QByteArray echoCommand()
    {
#ifdef Q_OS_WIN
        return "echo hssh_test\r\n";
#else
        return "echo hssh_test\n";
#endif
    }

    [[nodiscard]] static QByteArray exitCommand()
    {
#ifdef Q_OS_WIN
        return "exit\r\n";
#else
        return "exit\n";
#endif
    }
};

QTEST_MAIN(TestTerminal)
#include "test_terminal.moc"

#include "terminal/ZModemEngine.h"

#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace hssh;

namespace {

// Cross-connects two engines: the sender's output is fed to the receiver's
// input and vice versa (both directions synchronous, like a loopback cable).
// The receiver is byte-strict (subpacket + frame CRC validation), so any
// sender-side encoding bug surfaces exactly like it would against real rz.
class Loopback {
public:
    Loopback()
    {
        receiver.setDownloadDirectory(dir.path());
        QObject::connect(&sender, &ZModemEngine::output, &receiver,
                [this](const QByteArray &d) { receiver.feed(d); });
        QObject::connect(&receiver, &ZModemEngine::output, &sender,
                [this](const QByteArray &d) { sender.feed(d); });
    }

    ZModemEngine sender;
    ZModemEngine receiver;
    QTemporaryDir dir;
};

QByteArray makePattern(int size)
{
    QByteArray data;
    data.resize(size);
    for (int i = 0; i < size; ++i) {
        data[i] = static_cast<char>(i & 0xFF);
    }
    return data;
}

QByteArray makeEscapeStress()
{
    // Every value in the ZDLE escape set plus long runs of ZDLE itself —
    // the classic encoder traps.
    QByteArray data(1024, char(0x18));
    const unsigned char specials[] = {0x18, 0x10, 0x11, 0x13, 0x8d, 0x90, 0x91,
                                      0x93, 0x0d, 0x0a, 0x7f, 0x86, 0x87, 0xff};
    for (int round = 0; round < 64; ++round) {
        for (unsigned char c : specials) {
            data.append(static_cast<char>(c));
        }
    }
    for (int i = 0; i < 256; ++i) {
        data.append(static_cast<char>(i));
    }
    return data;
}

QByteArray makeRandom(int size, unsigned int seed)
{
    QByteArray data;
    data.resize(size);
    unsigned int x = seed ? seed : 1;
    for (int i = 0; i < size; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        data[i] = static_cast<char>(x & 0xFF);
    }
    return data;
}

} // namespace

class TestZmodem : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qApp->setApplicationName(QStringLiteral("hssh_test_zmodem"));
        qApp->setOrganizationName(QStringLiteral("hssh_project_test"));
    }

    void testLoopbackSmall()
    {
        runLoopback(QByteArray("hello zmodem loopback\n"), "small.txt");
    }

    void testLoopbackExactChunk()
    {
        // Exactly one 1024-byte subpacket.
        runLoopback(makePattern(1024), "chunk1024.bin");
    }

    void testLoopbackMultiChunk()
    {
        runLoopback(makePattern(64 * 1024 + 777), "pattern64k.bin");
    }

    void testLoopbackEscapeStress()
    {
        runLoopback(makeEscapeStress(), "escape.bin");
    }

    void testLoopbackRandom300k()
    {
        runLoopback(makeRandom(300 * 1024, 0x20260921u), "random300k.bin");
    }

private:
    void runLoopback(const QByteArray &payload, const QString &name)
    {
        Loopback loop;

        const QString srcPath = loop.dir.path() + QDir::separator() + name;
        {
            QFile f(srcPath);
            QVERIFY2(f.open(QIODevice::WriteOnly), "cannot write source file");
            f.write(payload);
            f.close();
        }

        QSignalSpy receiverDone(&loop.receiver, &ZModemEngine::finished);
        QSignalSpy senderDone(&loop.sender, &ZModemEngine::finished);

        QVERIFY2(loop.sender.startSend(srcPath), "startSend failed");
        // The whole session runs synchronously through the cross-connected
        // signals; process pending timer activity (retries) as a safety net.
        for (int i = 0; i < 20 && receiverDone.isEmpty(); ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        }

        QVERIFY2(receiverDone.size() == 1,
                 "receiver never finished (stalled session or CRC rejection)");
        QVERIFY2(receiverDone.first().first().toBool(),
                 "receiver reported failure (CRC error / rejected frame)");
        QVERIFY2(senderDone.size() == 1, "sender never finished");
        QVERIFY2(senderDone.first().first().toBool(), "sender reported failure");

        const QString saved = receiverDone.first().at(1).toString();
        QVERIFY2(!saved.isEmpty(), "no saved file path");
        QFile savedFile(saved);
        QVERIFY2(savedFile.open(QIODevice::ReadOnly), "saved file missing");
        const QByteArray got = savedFile.readAll();
        savedFile.close();
        QCOMPARE(got.size(), payload.size());
        QVERIFY2(got == payload,
                 qPrintable(QStringLiteral("content mismatch: sizes %1/%2, first diff at %3")
                                .arg(got.size()).arg(payload.size())
                                .arg(firstDiff(got, payload))));
    }

    [[nodiscard]] static int firstDiff(const QByteArray &a, const QByteArray &b)
    {
        const int n = qMin(a.size(), b.size());
        for (int i = 0; i < n; ++i) {
            if (a.at(i) != b.at(i)) {
                return i;
            }
        }
        return n < qMax(a.size(), b.size()) ? n : -1;
    }
};

QTEST_MAIN(TestZmodem)
#include "test_zmodem.moc"

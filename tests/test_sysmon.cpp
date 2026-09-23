#include "app/ServerMonitor.h"
#include "app/dialogs/DockerDialog.h"
#include "app/dialogs/ServerTransferDialog.h"
#include "app/dialogs/NetworkToolsDialog.h"
#include "app/dialogs/ProcessDialog.h"

#include <QtTest/QtTest>

using namespace hssh;

// B6-1: the pure /proc parsers of the server monitor. Synthetic inputs
// mirror the real kernel file formats (the wire truth comes from B1-5).
class TestSysMon : public QObject {
    Q_OBJECT

private slots:
    void testParseCpuPair()
    {
        // Deltas: total 1050-1000 = 50 jiffies, idle 885-850 = 35 -> 30% busy.
        QCOMPARE(ServerMonitor::parseCpuPair(QStringLiteral("cpu  100 0 50 850 0 0 0 0"),
                                             QStringLiteral("cpu  110 0 55 885 0 0 0 0")),
                 30.0);
        // iowait counts as idle.
        QCOMPARE(ServerMonitor::parseCpuPair(QStringLiteral("cpu  100 0 0 100 0 0 0 0"),
                                             QStringLiteral("cpu  100 0 0 100 50 0 0 0")),
                 0.0);
        // Malformed / non-aggregate lines never divide by zero.
        QCOMPARE(ServerMonitor::parseCpuPair(QString(), QString()), 0.0);
        QCOMPARE(ServerMonitor::parseCpuPair(QStringLiteral("cpu0 1 2 3 4 5 6 7 8"),
                                             QStringLiteral("cpu0 1 2 3 4 5 6 7 8")),
                 0.0);
    }

    void testParseMemInfo()
    {
        MonitorSample sample;
        ServerMonitor::parseMemInfo(
            QStringLiteral("MemTotal:       1000 kB\nMemAvailable:    400 kB\n"), &sample);
        QCOMPARE(sample.memTotalKb, qint64(1000));
        QCOMPARE(sample.memAvailableKb, qint64(400));
        QCOMPARE(sample.memPercent, 60.0);
    }

    void testParseNetDev()
    {
        const QString body = QStringLiteral(
            "eth0: 1000 1 0 0 0 0 0 0 500 1 0 0 0 0 0 0\n"
            "lo: 5 0 0 0 0 0 0 0 5 0 0 0 0 0 0 0\n"
            "not-an-iface\n");
        const auto counters = ServerMonitor::parseNetDev(body);
        QCOMPARE(counters.size(), 2);
        QCOMPARE(counters.value(QStringLiteral("eth0")).first, qint64(1000));
        QCOMPARE(counters.value(QStringLiteral("eth0")).second, qint64(500));
        QCOMPARE(counters.value(QStringLiteral("lo")).first, qint64(5));
    }

    void testParseDf()
    {
        const QString body = QStringLiteral(
            "Filesystem 1024-blocks Used Available Capacity Mounted on\n"
            "/dev/sda1 100 40 60 40% /\n"
            "tmpfs 50 1 49 2% /media/My\\040Disk\n");
        const QList<MonitorSample::DiskUse> disks = ServerMonitor::parseDf(body);
        QCOMPARE(disks.size(), 2);
        QCOMPARE(disks.at(0).fs, QStringLiteral("/dev/sda1"));
        QCOMPARE(disks.at(0).totalKb, qint64(100));
        QCOMPARE(disks.at(0).usedKb, qint64(40));
        QCOMPARE(disks.at(0).usedPercent, 40.0);
        QCOMPARE(disks.at(0).mount, QStringLiteral("/"));
        // df -P escapes spaces as \040.
        QCOMPARE(disks.at(1).mount, QStringLiteral("/media/My Disk"));
    }

    void testEndpointWarning()
    {
        SessionConfig saved;
        saved.setName(QStringLiteral("board"));
        saved.setHost(QStringLiteral("10.0.0.2"));
        saved.setPort(22);
        saved.setUsername(QStringLiteral("root"));

        QList<QPair<QString, QString>> tabs;
        // No open tab: nothing to disagree with.
        QVERIFY(ServerTransferDialog::endpointWarning(saved, tabs).isEmpty());
        // Same-name tab on the same endpoint: quiet.
        tabs.append({QStringLiteral("board"), QStringLiteral("root@10.0.0.2:22")});
        QVERIFY(ServerTransferDialog::endpointWarning(saved, tabs).isEmpty());
        // Stale: the open tab still points at the OLD machine.
        tabs.clear();
        tabs.append({QStringLiteral("board"), QStringLiteral("root@10.0.0.1:22")});
        const QString warning = ServerTransferDialog::endpointWarning(saved, tabs);
        QVERIFY(warning.contains(QLatin1String("10.0.0.2")));
        QVERIFY(warning.contains(QLatin1String("10.0.0.1")));
        // Different-name tabs never trigger; ad-hoc (no name) never does.
        tabs.append({QStringLiteral("other"), QStringLiteral("root@10.9.9.9:22")});
        SessionConfig adHoc;
        adHoc.setHost(QStringLiteral("10.0.0.3"));
        QVERIFY(ServerTransferDialog::endpointWarning(adHoc, tabs).isEmpty());
    }

    void testParseSampleIdentity()
    {
        const QString raw = QStringLiteral(
            "__HSSH_ID__\n"
            "rk3588-board\n"
            "192.0.2.10 192.0.2.11 \n"
            "__HSSH_CPU__\ncpu  1 0 1 8 0 0 0 0\ncpu  2 0 1 8 0 0 0 0\n"
            "__HSSH_MEM__\nMemTotal:       10 kB\nMemAvailable:    5 kB\n"
            "__HSSH_NET__\n"
            "__HSSH_DISK__\n");
        const MonitorSample sample = ServerMonitor::parseSample(raw, MonitorSample(), 2.0);
        QCOMPARE(sample.hostName, QStringLiteral("rk3588-board"));
        QCOMPARE(sample.hostIps, QStringLiteral("192.0.2.10 192.0.2.11"));

        // Samples without the ID section still parse (identity empty).
        const QString legacy = QStringLiteral(
            "__HSSH_CPU__\ncpu  1 0 1 8 0 0 0 0\ncpu  2 0 1 8 0 0 0 0\n"
            "__HSSH_MEM__\nMemTotal:       10 kB\nMemAvailable:    5 kB\n"
            "__HSSH_NET__\n"
            "__HSSH_DISK__\n");
        const MonitorSample legacySample = ServerMonitor::parseSample(legacy, MonitorSample(), 2.0);
        QVERIFY(legacySample.hostName.isEmpty());
    }

    void testParseSampleFull()
    {
        const QString raw = QStringLiteral(
            "__HSSH_CPU__\n"
            "cpu  100 0 50 850 0 0 0 0\n"
            "cpu  110 0 55 885 0 0 0 0\n"
            "__HSSH_MEM__\n"
            "MemTotal:       2000 kB\n"
            "MemAvailable:    500 kB\n"
            "__HSSH_NET__\n"
            "eth0: 1100 1 0 0 0 0 0 0 600 1 0 0 0 0 0 0\n"
            "__HSSH_DISK__\n"
            "/dev/sda1 100 40 60 40% /\n");

        MonitorSample previous;
        previous.netRaw.insert(QStringLiteral("eth0"), {1000, 500});

        const MonitorSample sample = ServerMonitor::parseSample(raw, previous, 2.0);
        QVERIFY(sample.valid);
        QCOMPARE(sample.cpuPercent, 30.0);
        QCOMPARE(sample.memTotalKb, qint64(2000));
        QCOMPARE(sample.memPercent, 75.0);
        QCOMPARE(sample.net.size(), 1);
        QCOMPARE(sample.net.at(0).name, QStringLiteral("eth0"));
        QCOMPARE(sample.net.at(0).rxBps, 50.0); // (1100-1000)/2s
        QCOMPARE(sample.net.at(0).txBps, 50.0); // (600-500)/2s
        QCOMPARE(sample.disks.size(), 1);
        QCOMPARE(sample.netRaw.value(QStringLiteral("eth0")).first, qint64(1100));
    }

    void testCounterResetSkipped()
    {
        // Interface bounced: counters went DOWN; this tick must report no
        // rate instead of a huge negative one.
        const QString raw = QStringLiteral(
            "__HSSH_CPU__\ncpu  1 0 1 8 0 0 0 0\ncpu  2 0 1 8 0 0 0 0\n"
            "__HSSH_MEM__\nMemTotal:       10 kB\nMemAvailable:    5 kB\n"
            "__HSSH_NET__\neth0: 100 1 0 0 0 0 0 0 50 1 0 0 0 0 0 0\n"
            "__HSSH_DISK__\n");
        MonitorSample previous;
        previous.netRaw.insert(QStringLiteral("eth0"), {9000, 9000});
        const MonitorSample sample = ServerMonitor::parseSample(raw, previous, 2.0);
        QVERIFY(sample.net.isEmpty());
    }

    void testParsePs()
    {
        const QString output = QStringLiteral(
            "1 0 root 0.0 0.1 Ss /sbin/init splash\n"
            "  1234 1 moobot 12.5 3.2 Sl python3 /srv/app/main.py --verbose\n"
            "garbage line\n"
            "999 1 root 1.0 0.5 Rs /usr/bin/dockerd\n");
        const QList<ProcessDialog::PsEntry> entries = ProcessDialog::parsePs(output);
        QCOMPARE(entries.size(), 3);
        QCOMPARE(entries.at(0).pid, qint64(1));
        QCOMPARE(entries.at(0).ppid, qint64(0));
        QCOMPARE(entries.at(0).user, QStringLiteral("root"));
        QCOMPARE(entries.at(0).args, QStringLiteral("/sbin/init splash"));
        // Leading spaces (ps column padding) and args with flags survive.
        QCOMPARE(entries.at(1).pid, qint64(1234));
        QCOMPARE(entries.at(1).cpuPercent, 12.5);
        QCOMPARE(entries.at(1).args,
                 QStringLiteral("python3 /srv/app/main.py --verbose"));
        QCOMPARE(entries.at(2).stat, QStringLiteral("Rs"));
    }

    void testNetworkBuildCommand()
    {
        bool streaming = false;

        // ping/traceroute stream; ss and the probe are one-shot.
        QVERIFY(!NetworkToolsDialog::buildCommand(QStringLiteral("ping"),
                                                  QStringLiteral("10.0.0.1"), QString(),
                                                  &streaming)
                    .isEmpty());
        QVERIFY(streaming);
        QVERIFY(!NetworkToolsDialog::buildCommand(QStringLiteral("ss"), QString(), QString(),
                                                  &streaming)
                    .isEmpty());
        QVERIFY(!streaming);

        // Port probe builds the /dev/tcp form with a valid port.
        const QString probe = NetworkToolsDialog::buildCommand(QStringLiteral("port"),
                                                                QStringLiteral("example.com"),
                                                                QStringLiteral("443"), &streaming);
        QVERIFY(probe.contains(QStringLiteral("/dev/tcp/example.com/443")));
        QVERIFY(!streaming);
        QVERIFY(NetworkToolsDialog::buildCommand(QStringLiteral("port"),
                                                 QStringLiteral("example.com"), QStringLiteral("0"),
                                                 nullptr)
                    .isEmpty());
        QVERIFY(NetworkToolsDialog::buildCommand(QStringLiteral("port"),
                                                 QStringLiteral("example.com"),
                                                 QStringLiteral("99999"), nullptr)
                    .isEmpty());

        // Injection attempts are rejected wholesale.
        QVERIFY(NetworkToolsDialog::buildCommand(
                    QStringLiteral("ping"), QStringLiteral("8.8.8.8; rm -rf /"), QString(), nullptr)
                    .isEmpty());
        QVERIFY(NetworkToolsDialog::buildCommand(
                    QStringLiteral("ping"), QStringLiteral("$(reboot)"), QString(), nullptr)
                    .isEmpty());
        QVERIFY(NetworkToolsDialog::buildCommand(QStringLiteral("ping"), QStringLiteral("a|b"),
                                                 QString(), nullptr)
                    .isEmpty());
    }

    void testParseDocker()
    {
        const QString ps = QStringLiteral(
            "{\"Command\":\"nginx\",\"ID\":\"abc123\",\"Image\":\"nginx:1.25\","
            "\"Names\":\"web\",\"State\":\"running\",\"Status\":\"Up 2 hours\"}\n"
            "{\"ID\":\"def456\",\"Image\":\"redis:7\",\"Names\":\"cache\","
            "\"State\":\"exited\",\"Status\":\"Exited (0) 3 hours ago\"}\n"
            "not json\n");
        const QList<DockerDialog::ContainerEntry> containers = DockerDialog::parseContainers(ps);
        QCOMPARE(containers.size(), 2);
        QCOMPARE(containers.at(0).id, QStringLiteral("abc123"));
        QCOMPARE(containers.at(0).name, QStringLiteral("web"));
        QCOMPARE(containers.at(0).state, QStringLiteral("running"));
        QCOMPARE(containers.at(1).image, QStringLiteral("redis:7"));

        const QString images = QStringLiteral(
            "{\"ID\":\"sha256:feed\", \"Repository\":\"ubuntu\", \"Tag\":\"24.04\", \"Size\":\"78MB\"}\n");
        const QList<DockerDialog::ImageEntry> parsed = DockerDialog::parseImages(images);
        QCOMPARE(parsed.size(), 1);
        QCOMPARE(parsed.at(0).repository, QStringLiteral("ubuntu"));
        QCOMPARE(parsed.at(0).tag, QStringLiteral("24.04"));
        QCOMPARE(parsed.at(0).size, QStringLiteral("78MB"));
    }
};

QTEST_GUILESS_MAIN(TestSysMon)
#include "test_sysmon.moc"

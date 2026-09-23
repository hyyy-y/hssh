#include "ServerMonitor.h"

#include "core/RemoteCommandChannel.h"

#include <QRegularExpression>

namespace hssh {

namespace {

// Section markers inside the combined sample command.
constexpr auto kMarkId = "__HSSH_ID__";
constexpr auto kMarkCpu = "__HSSH_CPU__";
constexpr auto kMarkMem = "__HSSH_MEM__";
constexpr auto kMarkNet = "__HSSH_NET__";
constexpr auto kMarkDisk = "__HSSH_DISK__";

QString section(const QString &raw, const char *name, const char *next)
{
    const int from = raw.indexOf(QLatin1String(name));
    if (from < 0) {
        return QString();
    }
    const int start = from + int(qstrlen(name));
    const int to = next ? raw.indexOf(QLatin1String(next), start) : -1;
    return to < 0 ? raw.mid(start) : raw.mid(start, to - start);
}

// "cpu  123 0 45 6789 ..." -> field sums (all jiffies except idle-class).
struct CpuJiffies {
    qint64 total = 0;
    qint64 idle = 0; // idle + iowait
    bool ok = false;
};

CpuJiffies parseCpuLine(const QString &line)
{
    CpuJiffies j;
    const QStringList parts = line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 5 || parts.first() != QLatin1String("cpu")) {
        return j;
    }
    bool okAll = true;
    qint64 total = 0;
    qint64 idle = 0;
    for (int i = 1; i < parts.size(); ++i) {
        bool ok = false;
        const qint64 v = parts.at(i).toLongLong(&ok);
        if (!ok) {
            okAll = false;
            break;
        }
        total += v;
        if (i == 4 || i == 5) { // idle, iowait
            idle += v;
        }
    }
    if (okAll) {
        j.total = total;
        j.idle = idle;
        j.ok = true;
    }
    return j;
}

} // namespace

QString ServerMonitor::sampleCommand()
{
    // One exec per tick. The two /proc/stat reads bracket a sleep so the CPU
    // delta covers a full second regardless of the tick interval.
    return QStringLiteral(
        "echo __HSSH_ID__; hostname; hostname -I 2>/dev/null; "
        "echo __HSSH_CPU__; grep '^cpu ' /proc/stat; sleep 1; grep '^cpu ' /proc/stat; "
        "echo __HSSH_MEM__; grep -E '^(MemTotal|MemAvailable):' /proc/meminfo; "
        "echo __HSSH_NET__; tail -n +3 /proc/net/dev; "
        "echo __HSSH_DISK__; df -P 2>/dev/null");
}

double ServerMonitor::parseCpuPair(const QString &before, const QString &after)
{
    const CpuJiffies a = parseCpuLine(before.simplified());
    const CpuJiffies b = parseCpuLine(after.simplified());
    if (!a.ok || !b.ok || b.total <= a.total) {
        return 0;
    }
    const double busy = double(b.total - a.total - (b.idle - a.idle));
    const double all = double(b.total - a.total);
    return all > 0 ? qBound(0.0, busy / all * 100.0, 100.0) : 0.0;
}

void ServerMonitor::parseMemInfo(const QString &meminfo, MonitorSample *out)
{
    static const QRegularExpression rx(QStringLiteral("^(MemTotal|MemAvailable):\\s+(\\d+) kB"),
                                       QRegularExpression::MultilineOption);
    auto it = rx.globalMatch(meminfo);
    while (it.hasNext()) {
        const auto m = it.next();
        const qint64 kb = m.captured(2).toLongLong();
        if (m.captured(1) == QLatin1String("MemTotal")) {
            out->memTotalKb = kb;
        } else {
            out->memAvailableKb = kb;
        }
    }
    if (out->memTotalKb > 0) {
        out->memPercent =
            qBound(0.0, double(out->memTotalKb - out->memAvailableKb) / out->memTotalKb * 100.0, 100.0);
    }
}

QMap<QString, QPair<qint64, qint64>> ServerMonitor::parseNetDev(const QString &body)
{
    QMap<QString, QPair<qint64, qint64>> result;
    const QStringList lines = body.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0) {
            continue;
        }
        const QString name = line.left(colon).simplified();
        const QStringList fields =
            line.mid(colon + 1).simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.size() < 9) {
            continue; // need rx bytes (0) and tx bytes (8)
        }
        bool okRx = false;
        bool okTx = false;
        const qint64 rx = fields.at(0).toLongLong(&okRx);
        const qint64 tx = fields.at(8).toLongLong(&okTx);
        if (okRx && okTx) {
            result.insert(name, {rx, tx});
        }
    }
    return result;
}

QList<MonitorSample::DiskUse> ServerMonitor::parseDf(const QString &body)
{
    QList<MonitorSample::DiskUse> result;
    const QStringList lines = body.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QStringList f = line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (f.size() != 6 || f.at(0) == QLatin1String("Filesystem")) {
            continue;
        }
        bool okTotal = false;
        bool okUsed = false;
        bool okPct = false;
        MonitorSample::DiskUse disk;
        disk.fs = f.at(0);
        disk.totalKb = f.at(1).toLongLong(&okTotal);
        disk.usedKb = f.at(2).toLongLong(&okUsed);
        const double pct = f.at(4).chopped(1).toDouble(&okPct); // "45%"
        disk.usedPercent = okPct ? pct : 0;
        // df -P escapes spaces in mount points as \040.
        disk.mount = f.at(5);
        disk.mount.replace(QStringLiteral("\\040"), QStringLiteral(" "));
        if (okTotal && okUsed) {
            result.append(disk);
        }
    }
    return result;
}

MonitorSample ServerMonitor::parseSample(const QString &raw, const MonitorSample &previous,
                                         double intervalSeconds)
{
    MonitorSample sample;

    const QStringList idLines =
        section(raw, kMarkId, kMarkCpu).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (!idLines.isEmpty()) {
        sample.hostName = idLines.at(0).simplified();
    }
    if (idLines.size() > 1) {
        sample.hostIps = idLines.at(1).simplified();
    }

    const QString cpuSection = section(raw, kMarkCpu, kMarkMem);
    const QStringList cpuLines = cpuSection.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (cpuLines.size() >= 2) {
        sample.cpuPercent = parseCpuPair(cpuLines.at(0), cpuLines.at(1));
    }

    parseMemInfo(section(raw, kMarkMem, kMarkNet), &sample);

    sample.netRaw = parseNetDev(section(raw, kMarkNet, kMarkDisk));
    if (intervalSeconds > 0 && !previous.netRaw.isEmpty()) {
        for (auto it = sample.netRaw.constBegin(); it != sample.netRaw.constEnd(); ++it) {
            const auto prev = previous.netRaw.constFind(it.key());
            if (prev == previous.netRaw.constEnd() || it.value() == prev.value()) {
                continue; // counters must be non-decreasing AND different
            }
            if (it.value().first < prev.value().first || it.value().second < prev.value().second) {
                continue; // counter reset (interface bounce): skip this tick
            }
            MonitorSample::IfaceRate rate;
            rate.name = it.key();
            rate.rxBps = double(it.value().first - prev.value().first) / intervalSeconds;
            rate.txBps = double(it.value().second - prev.value().second) / intervalSeconds;
            sample.net.append(rate);
        }
    }

    sample.disks = parseDf(section(raw, kMarkDisk, nullptr));
    sample.valid = sample.memTotalKb > 0 || !sample.netRaw.isEmpty() || !sample.disks.isEmpty();
    return sample;
}

ServerMonitor::ServerMonitor(const SessionConfig &config, QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<MonitorSample>("hssh::MonitorSample");
    // Parentless worker (it moveToThreads itself).
    m_channel = new RemoteCommandChannel(config);
    connect(m_channel, &RemoteCommandChannel::connected, this,
            &ServerMonitor::channelConnected);
    connect(m_channel, &RemoteCommandChannel::commandFinished, this,
            [this](const QString &id, int exitCode, const QString &error) {
                if (id != QLatin1String("sample")) {
                    return;
                }
                if (!error.isEmpty()) {
                    stop();
                    emit stopped(error);
                    return;
                }
                Q_UNUSED(exitCode);
            });
    connect(m_channel, &RemoteCommandChannel::commandOutput, this,
            [this](const QString &id, const QByteArray &chunk) {
                if (id != QLatin1String("sample")) {
                    return;
                }
                const MonitorSample sample =
                    parseSample(QString::fromUtf8(chunk), m_previous, m_intervalMs / 1000.0);
                if (sample.valid) {
                    m_previous = sample; // keep raw counters for rate deltas
                    emit sampleReady(sample);
                }
            });
    m_timer.setSingleShot(false);
    connect(&m_timer, &QTimer::timeout, this, &ServerMonitor::tick);
}

ServerMonitor::~ServerMonitor()
{
    stop();
    m_channel->stop();
    delete m_channel; // stop() joined the worker: direct delete is safe
}

void ServerMonitor::start(int intervalMs)
{
    if (m_running) {
        return;
    }
    m_intervalMs = qMax(1500, intervalMs); // the embedded sleep 1 needs headroom
    m_previous = MonitorSample();
    m_channel->start();
    m_running = true;
    tick(); // first sample right away
    m_timer.start(m_intervalMs);
}

void ServerMonitor::stop()
{
    if (!m_running) {
        return;
    }
    m_running = false;
    m_timer.stop();
    m_channel->cancel(QStringLiteral("sample"));
}

void ServerMonitor::tick()
{
    m_channel->runCommand(QStringLiteral("sample"), sampleCommand(), m_intervalMs * 6);
}

} // namespace hssh

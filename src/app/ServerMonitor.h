#ifndef HSSH_APP_SERVERMONITOR_H
#define HSSH_APP_SERVERMONITOR_H

#include "core/SessionConfig.h"

#include <QList>
#include <QMap>
#include <QObject>
#include <QPair>
#include <QString>
#include <QTimer>

namespace hssh {

class RemoteCommandChannel;

// One parsed monitoring sample. `net` carries byte-per-second rates derived
// from the RAW counters in `netRaw` of consecutive samples; the first sample
// after a start has no rates (no previous point).
struct MonitorSample {
    bool valid = false;
    // __HSSH_ID__ section: `hostname` + `hostname -I`. The identity line is
    // the anti-confusion check for cloned-image fleets (identical host keys
    // defeat known_hosts; different boards still show different IPs).
    QString hostName;
    QString hostIps;
    double cpuPercent = 0; // 0-100, all cores averaged
    qint64 memTotalKb = 0;
    qint64 memAvailableKb = 0;
    double memPercent = 0;
    struct IfaceRate {
        QString name;
        double rxBps = 0;
        double txBps = 0;
    };
    QList<IfaceRate> net;
    struct DiskUse {
        QString fs;
        QString mount;
        qint64 totalKb = 0;
        qint64 usedKb = 0;
        double usedPercent = 0;
    };
    QList<DiskUse> disks;
    // Raw /proc/net/dev counters (bytes) of THIS sample — rate input for the
    // NEXT one. Kept out of any display logic.
    QMap<QString, QPair<qint64, qint64>> netRaw;
};

// B6-1: periodic sampler for one SSH session. All sampling runs over a
// RemoteCommandChannel (dedicated connection) — the visible terminal is
// never touched. One combined round-trip per tick keeps the overhead at a
// single short-lived exec per interval; the CPU usage comes from two
// /proc/stat reads taken 1 s apart INSIDE that one command.
class ServerMonitor : public QObject {
    Q_OBJECT

public:
    // The single command a tick runs (sections separated by __NAME__ markers).
    [[nodiscard]] static QString sampleCommand();

    // Pure parsers (unit-tested):
    // "cpu  u n s i w ir sr st ..." pair -> 0-100 busy percent across cores.
    [[nodiscard]] static double parseCpuPair(const QString &before, const QString &after);
    // grep-style meminfo lines -> fills memTotalKb/memAvailableKb/memPercent.
    static void parseMemInfo(const QString &meminfo, MonitorSample *out);
    // /proc/net/dev body (without header lines) -> iface -> (rxBytes, txBytes).
    [[nodiscard]] static QMap<QString, QPair<qint64, qint64>> parseNetDev(const QString &body);
    // df -P output (with or without header) -> disk rows.
    [[nodiscard]] static QList<MonitorSample::DiskUse> parseDf(const QString &body);
    // Full sample assembly: sections + net-rate deltas against `previous`.
    [[nodiscard]] static MonitorSample parseSample(const QString &raw,
                                                   const MonitorSample &previous,
                                                   double intervalSeconds);

    explicit ServerMonitor(const SessionConfig &config, QObject *parent = nullptr);
    ~ServerMonitor() override;

    void start(int intervalMs = 2000);
    void stop();

signals:
    void sampleReady(const hssh::MonitorSample &sample);
    // Real socket peer of the sampling connection (numeric IP).
    void channelConnected(const QString &peerAddress);
    // Connection lost / command failed: sampling stopped itself.
    void stopped(const QString &reason);

private:
    void tick();

    RemoteCommandChannel *m_channel = nullptr;
    QTimer m_timer;
    int m_intervalMs = 2000;
    MonitorSample m_previous;
    bool m_running = false;
};

} // namespace hssh

Q_DECLARE_METATYPE(hssh::MonitorSample)

#endif // HSSH_APP_SERVERMONITOR_H

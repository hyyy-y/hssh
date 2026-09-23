#ifndef HSSH_APP_WIDGETS_MONITORWIDGET_H
#define HSSH_APP_WIDGETS_MONITORWIDGET_H

#include "app/ServerMonitor.h"
#include "core/SessionConfig.h"

#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;
class QPushButton;
class QTableWidget;

namespace hssh {

// B6-1: server monitor dock content. A session picker (fed by the main
// window), one sampler at a time, self-drawn CPU/memory history and a disk
// table. Sampling stops by itself when the connection dies.
class MonitorWidget : public QWidget {
    Q_OBJECT

public:
    // Returns connectable SSH sessions as (displayName, config).
    using SessionProvider = std::function<QList<QPair<QString, SessionConfig>>()>;

    explicit MonitorWidget(QWidget *parent = nullptr);
    ~MonitorWidget() override;

    void setSessionProvider(const SessionProvider &provider);

private:
    void refreshSessions();
    void onStartStop();
    void onSample(const MonitorSample &sample);
    void onMonitorStopped(const QString &reason);
    void updatePlot(const MonitorSample &sample);

    QComboBox *m_sessionBox = nullptr;
    QPushButton *m_toggleButton = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_netLabel = nullptr;
    QTableWidget *m_diskTable = nullptr;
    QWidget *m_plot = nullptr;

    SessionProvider m_provider;
    ServerMonitor *m_monitor = nullptr;
    QString m_monitorName;
    QString m_peer; // real socket peer of the sampling connection
    // Plot histories (0-100 percent), newest last; capped in updatePlot.
    QVector<double> m_cpuHistory;
    QVector<double> m_memHistory;
};

} // namespace hssh

#endif // HSSH_APP_WIDGETS_MONITORWIDGET_H

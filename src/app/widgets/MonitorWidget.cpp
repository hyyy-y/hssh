#include "MonitorWidget.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace hssh {

namespace {

constexpr int kHistoryCap = 300; // ~10 minutes at a 2 s tick

// Self-drawn percent history: two polylines over a 0-100 grid.
class MonitorPlot : public QWidget {
public:
    using QWidget::QWidget;

    void setData(QVector<double> *cpu, QVector<double> *mem)
    {
        m_cpu = cpu;
        m_mem = mem;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0x14, 0x16, 0x1a));

        const int left = 34;
        const int top = 8;
        const int right = width() - 8;
        const int bottom = height() - 18;
        if (right <= left || bottom <= top) {
            return;
        }
        const QRect area(left, top, right - left, bottom - top);

        p.setPen(QPen(QColor(0x33, 0x38, 0x44), 1));
        QFont small = font();
        small.setPointSizeF(font().pointSizeF() * 0.8);
        p.setFont(small);
        for (int pct = 0; pct <= 100; pct += 25) {
            const int y = area.bottom() - int(area.height() * pct / 100.0);
            p.drawLine(area.left(), y, area.right(), y);
            p.setPen(QColor(0x8a, 0x8f, 0x98));
            p.drawText(QRect(0, y - 8, left - 4, 16),
                       Qt::AlignRight | Qt::AlignVCenter, QString::number(pct));
            p.setPen(QPen(QColor(0x33, 0x38, 0x44), 1));
        }

        const auto drawSeries = [&p, &area](const QVector<double> *series, const QColor &color) {
            if (!series || series->size() < 2) {
                return;
            }
            QPainterPath path;
            const double stepX = double(area.width()) / (series->size() - 1);
            for (int i = 0; i < series->size(); ++i) {
                const double x = area.left() + i * stepX;
                const double y = area.bottom() - area.height() * qBound(0.0, series->at(i), 100.0) / 100.0;
                if (i == 0) {
                    path.moveTo(x, y);
                } else {
                    path.lineTo(x, y);
                }
            }
            p.setPen(QPen(color, 2));
            p.drawPath(path);
        };
        drawSeries(m_cpu, QColor(0x13, 0xbb, 0x70));
        drawSeries(m_mem, QColor(0x00, 0xa4, 0xef));

        // Legend with the newest values.
        QString cpuText = QStringLiteral("CPU");
        QString memText = QStringLiteral("MEM");
        if (m_cpu && !m_cpu->isEmpty()) {
            cpuText += QStringLiteral(" %1%").arg(QString::number(m_cpu->last(), 'f', 1));
        }
        if (m_mem && !m_mem->isEmpty()) {
            memText += QStringLiteral(" %1%").arg(QString::number(m_mem->last(), 'f', 1));
        }
        p.setPen(QColor(0x13, 0xbb, 0x70));
        p.drawText(area.left() + 6, area.top() + 12, cpuText);
        p.setPen(QColor(0x00, 0xa4, 0xef));
        p.drawText(area.left() + 6 + p.fontMetrics().horizontalAdvance(cpuText) + 16,
                   area.top() + 12, memText);
    }

private:
    QVector<double> *m_cpu = nullptr;
    QVector<double> *m_mem = nullptr;
};

QString humanRate(double bytesPerSecond)
{
    const QLocale locale;
    return locale.formattedDataSize(qint64(bytesPerSecond)) + QStringLiteral("/s");
}

} // namespace

MonitorWidget::MonitorWidget(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto *bar = new QHBoxLayout;
    m_sessionBox = new QComboBox(this);
    m_toggleButton = new QPushButton(tr("Start"), this);
    auto *refreshButton = new QPushButton(tr("Refresh"), this);
    bar->addWidget(m_sessionBox, 1);
    bar->addWidget(refreshButton);
    bar->addWidget(m_toggleButton);
    layout->addLayout(bar);

    m_plot = new MonitorPlot(this);
    m_plot->setMinimumHeight(160);
    static_cast<MonitorPlot *>(m_plot)->setData(&m_cpuHistory, &m_memHistory);
    layout->addWidget(m_plot, 2);

    m_netLabel = new QLabel(tr("No sample yet"), this);
    layout->addWidget(m_netLabel);

    m_diskTable = new QTableWidget(0, 5, this);
    m_diskTable->setHorizontalHeaderLabels({tr("Mount"), tr("Filesystem"), tr("Used"), tr("Total"), tr("%")});
    m_diskTable->verticalHeader()->setVisible(false);
    m_diskTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_diskTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_diskTable->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(m_diskTable, 1);

    m_statusLabel = new QLabel(this);
    layout->addWidget(m_statusLabel);

    connect(m_toggleButton, &QPushButton::clicked, this, &MonitorWidget::onStartStop);
    connect(refreshButton, &QPushButton::clicked, this, &MonitorWidget::refreshSessions);
    refreshSessions();
}

MonitorWidget::~MonitorWidget()
{
    delete m_monitor; // ServerMonitor dtor stops its channel
}

void MonitorWidget::setSessionProvider(const SessionProvider &provider)
{
    m_provider = provider;
    refreshSessions();
}

void MonitorWidget::refreshSessions()
{
    if (m_monitor) {
        return; // locked while sampling
    }
    m_sessionBox->clear();
    if (!m_provider) {
        return;
    }
    const auto sessions = m_provider();
    for (const auto &entry : sessions) {
        m_sessionBox->addItem(entry.first);
    }
    m_statusLabel->setText(sessions.isEmpty() ? tr("No connected SSH sessions") : QString());
}

void MonitorWidget::onStartStop()
{
    if (m_monitor) {
        delete m_monitor;
        m_monitor = nullptr;
        m_toggleButton->setText(tr("Start"));
        m_statusLabel->setText(tr("Stopped."));
        refreshSessions();
        return;
    }

    if (m_sessionBox->count() == 0 || !m_provider) {
        return;
    }
    const auto sessions = m_provider();
    const int index = m_sessionBox->currentIndex();
    if (index < 0 || index >= sessions.size()) {
        return;
    }
    m_monitorName = sessions.at(index).first;
    m_monitor = new ServerMonitor(sessions.at(index).second, this);
    connect(m_monitor, &ServerMonitor::sampleReady, this, &MonitorWidget::onSample);
    connect(m_monitor, &ServerMonitor::stopped, this, &MonitorWidget::onMonitorStopped);
    m_cpuHistory.clear();
    m_memHistory.clear();
    m_plot->update();
    m_monitor->start();
    m_toggleButton->setText(tr("Stop"));
    m_statusLabel->setText(tr("Sampling %1…").arg(m_monitorName));
}

void MonitorWidget::onSample(const MonitorSample &sample)
{
    updatePlot(sample);

    // Identity line (hostname + IPs): the check that survives cloned-image
    // fleets where known_hosts cannot tell boards apart.
    if (!sample.hostName.isEmpty()) {
        m_statusLabel->setText(tr("%1 — %2 [%3]")
                                   .arg(m_monitorName, sample.hostName, sample.hostIps));
    }

    // Top-3 interfaces by receive rate.
    QList<MonitorSample::IfaceRate> rates = sample.net;
    std::sort(rates.begin(), rates.end(),
              [](const MonitorSample::IfaceRate &a, const MonitorSample::IfaceRate &b) {
                  return a.rxBps + a.txBps > b.rxBps + b.txBps;
              });
    QStringList netText;
    for (int i = 0; i < qMin(3, rates.size()); ++i) {
        netText.append(QStringLiteral("%1 ↓%2 ↑%3")
                           .arg(rates.at(i).name,
                                humanRate(rates.at(i).rxBps),
                                humanRate(rates.at(i).txBps)));
    }
    m_netLabel->setText(netText.isEmpty() ? tr("net: waiting for the next sample…")
                                          : netText.join(QStringLiteral("   ")));

    const QLocale locale;
    m_diskTable->setRowCount(sample.disks.size());
    for (int i = 0; i < sample.disks.size(); ++i) {
        const auto &disk = sample.disks.at(i);
        m_diskTable->setItem(i, 0, new QTableWidgetItem(disk.mount));
        m_diskTable->setItem(i, 1, new QTableWidgetItem(disk.fs));
        m_diskTable->setItem(i, 2, new QTableWidgetItem(locale.formattedDataSize(disk.usedKb * 1024)));
        m_diskTable->setItem(i, 3, new QTableWidgetItem(locale.formattedDataSize(disk.totalKb * 1024)));
        auto *pct = new QTableWidgetItem(QString::number(disk.usedPercent, 'f', 1));
        pct->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_diskTable->setItem(i, 4, pct);
    }
}

void MonitorWidget::onMonitorStopped(const QString &reason)
{
    // Self-stop on connection loss; the button state follows.
    if (!m_monitor) {
        return;
    }
    delete m_monitor;
    m_monitor = nullptr;
    m_toggleButton->setText(tr("Start"));
    m_statusLabel->setText(tr("Stopped (%1).").arg(reason));
    refreshSessions();
}

void MonitorWidget::updatePlot(const MonitorSample &sample)
{
    m_cpuHistory.append(sample.cpuPercent);
    m_memHistory.append(sample.memPercent);
    if (m_cpuHistory.size() > kHistoryCap) {
        m_cpuHistory.remove(0, m_cpuHistory.size() - kHistoryCap);
        m_memHistory.remove(0, m_memHistory.size() - kHistoryCap);
    }
    m_plot->update();
}

} // namespace hssh

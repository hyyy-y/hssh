#ifndef HSSH_APP_DIALOGS_DOCKERDIALOG_H
#define HSSH_APP_DIALOGS_DOCKERDIALOG_H

#include "core/SessionConfig.h"

#include <QDialog>

class QPushButton;
class QTabWidget;
class QTableWidget;

namespace hssh {

class RemoteCommandChannel;

// B6-4: Docker management over a dedicated connection — container/image
// tables (docker --format '{{json .}}'), start/stop/restart/rm buttons and
// a streaming log view (docker logs -f). No daemon socket is opened; every
// action is a plain docker CLI call, so permissions follow the shell user.
class DockerDialog : public QDialog {
    Q_OBJECT

public:
    struct ContainerEntry {
        QString id;      // short id
        QString name;
        QString image;
        QString state;   // running / exited / ...
        QString status;  // human text
    };
    struct ImageEntry {
        QString id;
        QString repository;
        QString tag;
        QString size;
    };

    explicit DockerDialog(const SessionConfig &config, QWidget *parent = nullptr);
    ~DockerDialog() override;

    // docker ps -a --format '{{json .}}' (one JSON object per line).
    [[nodiscard]] static QList<ContainerEntry> parseContainers(const QString &output);
    // docker images --format '{{json .}}'.
    [[nodiscard]] static QList<ImageEntry> parseImages(const QString &output);

private:
    void refresh();
    void containerAction(const QString &verb);
    void showLogs();
    void fillContainers();
    void fillImages();
    void onFinished(const QString &id, int exitCode, const QString &error);
    [[nodiscard]] QString selectedContainerId() const;

    SessionConfig m_config;
    RemoteCommandChannel *m_channel = nullptr;
    QTabWidget *m_tabs = nullptr;
    QTableWidget *m_containerTable = nullptr;
    QTableWidget *m_imageTable = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_stopButton = nullptr;
    QPushButton *m_restartButton = nullptr;
    QPushButton *m_rmButton = nullptr;
    QPushButton *m_logsButton = nullptr;

    QList<ContainerEntry> m_containers;
    QList<ImageEntry> m_images;
    QString m_pendingRefreshId;
};

} // namespace hssh

#endif // HSSH_APP_DIALOGS_DOCKERDIALOG_H

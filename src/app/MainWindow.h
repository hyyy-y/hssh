#ifndef HSSH_MAINWINDOW_H
#define HSSH_MAINWINDOW_H

#include <QMainWindow>
#include <QModelIndex>

QT_BEGIN_NAMESPACE
class QDockWidget;
class QLabel;
class QTabWidget;
QT_END_NAMESPACE

namespace hssh {

class SessionConfig;
class SessionManagerWidget;
class SessionModel;
class SessionRepository;
class LocalFileWidget;
class TransfersWidget;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onNewSession();
    void onNewLocalTerminal(const QString &shellType = QString());
    void onEditSession(const QModelIndex &index);
    void onRemoveSession(const QModelIndex &index);
    void onNewFolder();
    void onEditFolder(const QModelIndex &index);
    void onRemoveFolder(const QModelIndex &index);
    void onSessionActivated(const SessionConfig &config);
    void onConnect();
    void onDisconnect();
    void onOpenSftp();
    void onOpenCompare();

private:
    void setupUi();
    void setupMenuBar();
    void setupToolBar();
    void setupDockWidgets();
    void setupCentralWidget();
    void setupStatusBar();
    void loadSessions();
    void addSftpTab(const SessionConfig &config, const QString &remotePath = QString());
    void updateStatusBarInfo();

    QModelIndex selectedSessionIndex() const;
    QModelIndex selectedFolderIndex() const;

    SessionManagerWidget *m_sessionManager = nullptr;
    SessionModel *m_sessionModel = nullptr;
    SessionRepository *m_sessionRepository = nullptr;
    LocalFileWidget *m_localFiles = nullptr;
    TransfersWidget *m_transfers = nullptr;
    QDockWidget *m_sessionDock = nullptr;
    QDockWidget *m_fileDock = nullptr;
    QDockWidget *m_transfersDock = nullptr;
    QLabel *m_sessionInfoLabel = nullptr;
    QLabel *m_termSizeLabel = nullptr;
    QTabWidget *m_tabWidget = nullptr;
};

} // namespace hssh

#endif // HSSH_MAINWINDOW_H

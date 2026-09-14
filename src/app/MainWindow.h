#ifndef HSSH_MAINWINDOW_H
#define HSSH_MAINWINDOW_H

#include <QMainWindow>
#include <QModelIndex>
#include <QPointer>

#include "agent/AgentHttpServer.h"

QT_BEGIN_NAMESPACE
class QDockWidget;
class QLabel;
class QTabWidget;
QT_END_NAMESPACE

namespace hssh {

class AgentHttpServer;
class SessionConfig;
class SessionManagerWidget;
class SessionModel;
class SessionRepository;
class SessionTab;
class SshSession;
class LocalFileWidget;
class TransfersWidget;

class MainWindow : public QMainWindow, public AgentTabsInterface {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // AgentTabsInterface: lets the local REST API list/read/send to the open
    // terminal tabs (AI agents and scripts use this to drive SSH windows).
    QVariantList listTabs() const override;
    bool sendToTab(int index, const QString &text) override;
    bool sendInputToTab(int index, const QString &data) override;
    bool readTab(int index, int maxLines, QString *text) const override;
    bool readTabRange(int index, int fromLine, int maxLines, QString *text) const override;
    int openLocalTab(const QString &shellType) override;
    int openSessionTab(const QString &name) override;
    int openSshTab(const SessionConfig &config) override;
    QVariantList listSavedSessions() const override;
    bool closeTab(int index) override;
    // Enter-to-reconnect equivalent for the agent API: only acts on a
    // disconnected SSH tab (never kills a live tab's foreground program).
    bool reconnectTab(int index) override;
    bool sendSecretToTab(int index, const QString &text) override;
    SessionConfig sessionConfigForTab(int index) const override;
    bool confirmSudo(int index, const QString &command, QString *reason) override;
    bool sudoExec(int index, const QString &command, const QString &secret,
                  bool useStoredCredential, int timeoutMs,
                  QString *output, bool *timedOut, int *exitCode,
                  QString *errorMessage) override;

    // Starts the local agent API if not already running (idempotent). Used
    // by the --agent CLI flag: the MCP bridge auto-launches the GUI this way.
    void startAgent();

private:
    // IP of the tab's live SSH transport peer (empty when undeterminable).
    QString tabPeerAddress(SshSession *session) const;

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
    void onPortForwarding();
    void onImportSessions();
    void onExportSessions();
    void onToggleAgent();
    void onOpenSettings();
    void onSendCommand();
    void onFocusMode();
    void onSetLockPassword();
    void onLockScreen();
    void onSetMasterPassword();
    void onClearMasterPassword();

private:
    // Sudo-flow helpers (buffer polling for prompts/completion).
    bool waitForPromptPattern(SessionTab *tab, const QStringList &patterns, int timeoutMs);

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
    void updateRecentMenu();
    void applyFocusMode(bool enabled);
    void applyDefaultDockLayout();
    void saveWindowState();
    // Tab persistence (PH0-04): remember open tabs across restarts.
    QVariantList collectOpenTabs() const;
    void restorePreviousTabs();

protected:
    void closeEvent(QCloseEvent *event) override;

    QModelIndex selectedSessionIndex() const;
    QModelIndex selectedFolderIndex() const;

    SessionManagerWidget *m_sessionManager = nullptr;
    SessionModel *m_sessionModel = nullptr;
    SessionRepository *m_sessionRepository = nullptr;
    LocalFileWidget *m_localFiles = nullptr;
    TransfersWidget *m_transfers = nullptr;
    AgentHttpServer *m_agentServer = nullptr;
    QDockWidget *m_sessionDock = nullptr;
    QDockWidget *m_fileDock = nullptr;
    QDockWidget *m_transfersDock = nullptr;
    QLabel *m_sessionInfoLabel = nullptr;
    QLabel *m_termSizeLabel = nullptr;
    QTabWidget *m_tabWidget = nullptr;
    QAction *m_agentAction = nullptr;
    QMenu *m_recentMenu = nullptr;
    bool m_focusMode = false;
    // Tabs already approved for sudo execution (session-scoped).
    QList<QPointer<SessionTab>> m_sudoApproved;
};

} // namespace hssh

#endif // HSSH_MAINWINDOW_H

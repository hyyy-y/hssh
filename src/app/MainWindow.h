#ifndef HSSH_MAINWINDOW_H
#define HSSH_MAINWINDOW_H

#include <QMainWindow>
#include <QModelIndex>
#include <QPointer>

#include <optional>

#include "agent/AgentHttpServer.h"

QT_BEGIN_NAMESPACE
class QAction;
class QDockWidget;
class QIcon;
class QLabel;
class QLineEdit;
class QTabWidget;
QT_END_NAMESPACE

QT_BEGIN_NAMESPACE
class QColor;
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
    explicit MainWindow(int agentPort = 8222, bool skipTabRestore = false,
                        QWidget *parent = nullptr);
    ~MainWindow() override;

    // AgentTabsInterface: lets the local REST API list/read/send to the open
    // terminal tabs (AI agents and scripts use this to drive SSH windows).
    QVariantList listTabs() const override;
    bool sendToTab(int index, const QString &text) override;
    bool sendInputToTab(int index, const QString &data) override;
    // PH2-10: ZMODEM push (local file -> remote rz) in the visible tab.
    bool zmodemSendToTab(int index, const QString &localPath) override;
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
    // Non-blocking sudo: the confirmation dialog shows WITHOUT a nested
    // event loop (30 s auto-reject timer), the callback fires when the
    // whole flow (or the rejection) is done. Fixes the race between the
    // 30 s dialog and the MCP client's 30 s tool timeout (2026-09-20).
    void sudoAsync(int index, const QString &command, const QString &secret,
                   bool useStoredCredential, int timeoutMs,
                   const AgentTabsInterface::SudoAsyncCallback &cb) override;

    // Starts the local agent API if not already running (idempotent). Used
    // by the --agent CLI flag: the MCP bridge auto-launches the GUI this way.
    // The port parameter is mainly for secondary test instances.
    void startAgent(int port = 8222);
    // Test-instance switch (--no-restore): suppresses the crash-restore
    // prompt so automated runs never block on a modal dialog.
    void setSkipTabRestore(bool skip) { m_skipTabRestore = skip; }

public slots:
    // PH1-05: apply a terminal font to every open tab (and remember it for
    // new tabs). Invoked by the Appearance dialog and startup.
    void applyTerminalFont(const QFont &font);

private:
    // IP of the tab's live SSH transport peer (empty when undeterminable).
    QString tabPeerAddress(SshSession *session) const;

private slots:
    void onNewSession();
    void onNewLocalTerminal(const QString &shellType = QString());
    void onEditSession(const QModelIndex &index);
    // Copy a session's full configuration into a new session (fresh id,
    // prefilled dialog for name/host adjustments).
    void onDuplicateSession(const QModelIndex &index);
    // PH2-15: favorite pinning and tag editing.
    void onToggleFavorite(const QModelIndex &index);
    void onEditTags(const QModelIndex &index);
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
    // PH1-06: toolbar quick-connect bar ("user@host[:port]" + password).
    void onQuickConnect();
    // PH1-07: import sessions from ~/.ssh/config.
    void onImportSshConfig();
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
    // PH1-08: per-tab enhancements — alias, color, pin, sync-input group.
    void onTabContextMenu(const QPoint &pos);
    void applyTabColor(int index, const QColor &color);
    void setTabPinned(int index, bool pinned);
    // PH1-08: tab detach — the tab moves into its own FloatingTabWindow and
    // back ("drag out of the tab bar" or the context-menu action).
    void detachTab(int index);
    void dockFloatingTab(class FloatingTabWindow *window);
    void closeFloatingTab(class FloatingTabWindow *window);
    // PH2-08: mirror keyboard input to every other tab in the sync group.
    void routeSyncInput(SessionTab *source, const QByteArray &data);
    // Shared wiring for every newly created SessionTab.
    void wireSessionTab(SessionTab *tab);
    // PH2-07: build the palette items and show the command palette.
    void showCommandPalette();
    // Small colored bar icon used for tab color grouping.
    static QIcon colorSwatchIcon(const QColor &color);

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

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
    QDockWidget *m_outlineDock = nullptr;
    QLabel *m_sessionInfoLabel = nullptr;
    QLabel *m_termSizeLabel = nullptr;
    QTabWidget *m_tabWidget = nullptr;
    QAction *m_agentAction = nullptr;
    QMenu *m_recentMenu = nullptr;
    QLineEdit *m_quickConnectEdit = nullptr;
    // PH2-08 Free Type: checkable toolbar toggle; when on, keyboard input is
    // mirrored to every other SSH tab (Xshell-style).
    QAction *m_freeTypeAction = nullptr;
    bool m_freeTypeMode = false;
    // PH1-08: tab-bar drag-out state (event filter on the tab bar).
    int m_barPressedIndex = -1;
    QList<QPointer<class FloatingTabWindow>> m_floatingTabs;
    bool m_focusMode = false;
    bool m_skipTabRestore = false;
    int m_agentPort = 8222;
    // PH1-05: persisted terminal font. A default-constructed QFont is NOT an
    // "unset" marker (its family resolves to the application font), so absence
    // is modelled explicitly — otherwise every tab got the proportional UI
    // font and glyphs drifted off the terminal's cell grid.
    std::optional<QFont> m_terminalFont;
    // Tabs already approved for sudo execution (session-scoped).
    QList<QPointer<SessionTab>> m_sudoApproved;
    // PH2-08: tabs mirroring each other's keyboard input.
    QList<QPointer<SessionTab>> m_syncInputTabs;
};

} // namespace hssh

#endif // HSSH_MAINWINDOW_H

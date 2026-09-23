#include "MainWindow.h"

#include "agent/AgentAudit.h"
#include "agent/AgentHttpServer.h"
#include "agent/AgentSudoAuth.h"
#include "app/InputBroadcaster.h"
#include "app/FloatingTabWindow.h"
#include "app/SessionTab.h"
#include "terminal/TerminalSession.h"
#include "app/dialogs/AppearanceDialog.h"
#include "app/dialogs/KeyManagerDialog.h"
#include "app/dialogs/EditFolderDialog.h"
#include "app/dialogs/ImportSshConfigDialog.h"
#include "app/dialogs/NewSessionDialog.h"
#include "app/dialogs/PortForwardDialog.h"
#include "app/dialogs/ProcessDialog.h"
#include "app/dialogs/DockerDialog.h"
#include "app/dialogs/NetworkToolsDialog.h"
#include "app/dialogs/SchedulerDialog.h"
#include "app/dialogs/SessionLogViewer.h"
#include "app/dialogs/ServerTransferDialog.h"
#include "app/dialogs/SettingsDialog.h"
#include "app/widgets/CommandPalette.h"
#include "app/widgets/FileCompareWidget.h"
#include "app/widgets/LocalFileWidget.h"
#include "app/widgets/MonitorWidget.h"
#include "app/widgets/SessionManagerWidget.h"
#include "app/widgets/TerminalOutlineWidget.h"
#include "app/widgets/SftpWidget.h"
#include "app/widgets/TransfersWidget.h"
#include "core/SessionRepository.h"
#include "core/PortForward.h"
#include "core/SshSession.h"
#include "hssh/Version.h"
#include "terminal/LocalShellProcess.h"
#include "terminal/TerminalWidget.h"
#include "utils/Config.h"
#include "utils/Crypto.h"

#include <QApplication>
#include <QColor>
#include <QCursor>
#include <QIcon>
#include <QPainter>
#include <QShortcut>
#include <QUuid>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStatusBar>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#ifdef HSSH_HAS_LIBSSH
#  include <QMutexLocker>
#  ifdef Q_OS_WIN
#    include <winsock2.h>
#    include <ws2tcpip.h>
#  else
#    include <netdb.h>
#    include <sys/socket.h>
#  endif
#endif

namespace hssh {

MainWindow::MainWindow(int agentPort, bool skipTabRestore, QWidget *parent)
    : QMainWindow(parent)
    , m_sessionRepository(new SessionRepository(this))
    , m_agentPort(agentPort)
    , m_skipTabRestore(skipTabRestore)
{
    setupUi();
    setWindowTitle("hssh - Modern SSH Client");
    resize(1280, 800);

    loadSessions();

    // Restore the previous window/dock layout; fall back to sane defaults.
    const QByteArray geometry = Config::instance().value(QStringLiteral("ui/geometry")).toByteArray();
    const QByteArray state = Config::instance().value(QStringLiteral("ui/state")).toByteArray();
    bool restored = false;
    if (!geometry.isEmpty()) {
        restored = restoreGeometry(geometry);
    }
    if (!state.isEmpty()) {
        restored = restoreState(state) || restored;
    }
    if (!restored) {
        applyDefaultDockLayout();
    }

    // Restore the tabs that were open in the previous run.
    restorePreviousTabs();

    // PH1-05: persisted terminal font (empty family = widget default).
    const QString fontFamily = Config::instance()
                                   .value(QStringLiteral("terminal/fontFamily"))
                                   .toString();
    if (!fontFamily.isEmpty()) {
        QFont font(fontFamily);
        font.setPointSize(Config::instance()
                              .value(QStringLiteral("terminal/fontSize"), 10)
                              .toInt());
        m_terminalFont = font;
    }

    // Persisted preference: start the local agent API with the GUI so
    // external tools can drive open terminal tabs without a manual step.
    if (Config::instance().boolValue(QStringLiteral("agent/autostart"), false)) {
        onToggleAgent();
    }
}

void MainWindow::applyDefaultDockLayout()
{
    // Left column: session tree ~280px, file manager ~360px wide.
    resizeDocks({m_sessionDock, m_fileDock}, {280, 360}, Qt::Horizontal);
    // Split the left column vertically: sessions ~40%, files ~60%.
    resizeDocks({m_sessionDock, m_fileDock}, {320, 480}, Qt::Vertical);
    // Bottom transfers strip.
    resizeDocks({m_transfersDock}, {160}, Qt::Vertical);
}

void MainWindow::saveWindowState()
{
    Config::instance().setValue(QStringLiteral("ui/geometry"), saveGeometry());
    Config::instance().setValue(QStringLiteral("ui/state"), saveState());
    Config::instance().sync();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    Config::instance().setValue(QStringLiteral("ui/openTabs"), collectOpenTabs());
    saveWindowState();
    QMainWindow::closeEvent(event);
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi()
{
    // Docks first: the View menu lists their toggleViewActions.
    // Status bar before the central widget: adding the first tab emits
    // currentChanged, which updates the status bar labels.
    setupDockWidgets();
    setupMenuBar();
    setupToolBar();
    setupStatusBar();
    setupCentralWidget();
}

void MainWindow::setupMenuBar()
{
    // File menu
    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&New Session"), QKeySequence::New, this, &MainWindow::onNewSession);
    fileMenu->addAction(tr("New &Local Terminal"), QKeySequence(tr("Ctrl+Shift+T")), this, [this]() {
        onNewLocalTerminal(QString());
    });
    fileMenu->addAction(tr("&Connect"), QKeySequence(tr("Ctrl+Shift+C")), this, &MainWindow::onConnect);
    fileMenu->addAction(tr("&Disconnect"), this, &MainWindow::onDisconnect);
    fileMenu->addSeparator();
    m_recentMenu = fileMenu->addMenu(tr("Recent Sessions"));
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Import Sessions..."), this, &MainWindow::onImportSessions);
    fileMenu->addAction(tr("Import Open&SSH config..."), this, &MainWindow::onImportSshConfig);
    fileMenu->addAction(tr("&Export Sessions..."), this, &MainWindow::onExportSessions);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    // View menu
    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_sessionDock->toggleViewAction());
    viewMenu->addAction(m_fileDock->toggleViewAction());
    viewMenu->addAction(m_transfersDock->toggleViewAction());
    viewMenu->addAction(m_outlineDock->toggleViewAction());
    viewMenu->addAction(m_monitorDock->toggleViewAction());
    viewMenu->addSeparator();
    viewMenu->addAction(tr("Full Screen"), QKeySequence::FullScreen, this, [this]() {
        isFullScreen() ? showNormal() : showFullScreen();
    });
    viewMenu->addAction(tr("Focus Mode"), QKeySequence(tr("F11")), this, &MainWindow::onFocusMode);
    viewMenu->addSeparator();
    viewMenu->addAction(tr("Reset Layout"), this, [this]() {
        Config::instance().remove(QStringLiteral("ui/geometry"));
        Config::instance().remove(QStringLiteral("ui/state"));
        applyDefaultDockLayout();
    });

    // Tools menu
    QMenu *toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->addAction(tr("&Port Forwarding..."), this, &MainWindow::onPortForwarding);
    toolsMenu->addAction(tr("Send Command to All Terminals..."), this, &MainWindow::onSendCommand);
    // PH3-13: copy a file between two saved sessions (verified two-hop).
    toolsMenu->addAction(tr("Server-to-Server Transfer..."), this, [this]() {
        // Real endpoints of open SSH tabs: the transfer dialog warns when a
        // selected session has drifted from an open same-name tab.
        QList<QPair<QString, QString>> endpoints;
        for (int i = 0; i < m_tabWidget->count(); ++i) {
            auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(i));
            if (!tab || tab->config().sessionType() != SessionType::Ssh
                || !tab->sshSession()) {
                continue;
            }
            const QString peer = tabPeerAddress(tab->sshSession());
            const SessionConfig &config = tab->config();
            endpoints.append({config.name(),
                              QStringLiteral("%1@%2:%3")
                                  .arg(config.username(),
                                       peer.isEmpty() ? config.host() : peer)
                                  .arg(config.port())});
        }
        ServerTransferDialog dialog(m_sessionRepository->loadAllSessions(), endpoints, this);
        dialog.exec();
    });
    toolsMenu->addAction(tr("&Appearance..."), this, [this]() {
        AppearanceDialog dialog(this);
        dialog.exec();
    });
    toolsMenu->addAction(tr("&Settings..."), this, &MainWindow::onOpenSettings);
    toolsMenu->addAction(tr("&Key Manager..."), this, [this]() {
        KeyManagerDialog dialog(this);
        dialog.exec();
    });
    // PH2-15: browse the automatic per-tab session logs.
    toolsMenu->addAction(tr("Session &Log Viewer..."), this, [this]() {
        SessionLogViewer viewer(this);
        viewer.exec();
    });
    toolsMenu->addSeparator();
    toolsMenu->addAction(tr("Set Lock Password..."), this, &MainWindow::onSetLockPassword);
    toolsMenu->addAction(tr("Lock Screen"), QKeySequence(tr("Ctrl+Alt+L")), this, &MainWindow::onLockScreen);
    toolsMenu->addSeparator();
    toolsMenu->addAction(tr("Set Master Password..."), this, &MainWindow::onSetMasterPassword);
    toolsMenu->addAction(tr("Disable Master Password"), this, &MainWindow::onClearMasterPassword);
    toolsMenu->addSeparator();
    m_agentAction = toolsMenu->addAction(tr("Start Agent"), this, &MainWindow::onToggleAgent);
    QAction *autostartAction = toolsMenu->addAction(tr("Auto-start Agent with GUI"), this, [this](bool checked) {
        Config::instance().setValue(QStringLiteral("agent/autostart"), checked);
        Config::instance().sync();
    });
    autostartAction->setCheckable(true);
    autostartAction->setChecked(Config::instance().boolValue(QStringLiteral("agent/autostart"), false));

    // Help menu
    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&About"), this, [this]() {
        QDialog *dialog = new QDialog(this);
        dialog->setWindowTitle(tr("About hssh"));
        dialog->setMinimumSize(400, 220);
        dialog->setAttribute(Qt::WA_DeleteOnClose);

        QVBoxLayout *layout = new QVBoxLayout(dialog);

        QLabel *label = new QLabel(
            QStringLiteral("<h2>hssh %1</h2>"
                           "<p>A fully open-source, cross-platform, high-performance GUI SSH client.</p>"
                           "<p>License: Apache-2.0</p>")
                .arg(QApplication::applicationVersion().isEmpty() ? QStringLiteral("0.1.0") : QApplication::applicationVersion()));
        label->setAlignment(Qt::AlignCenter);
        layout->addWidget(label);

        QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, dialog);
        connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
        layout->addWidget(buttons);

        dialog->show();
    });
}

void MainWindow::setupToolBar()
{
    QToolBar *toolBar = addToolBar(tr("Main Toolbar"));
    toolBar->setMovable(false);

    toolBar->addAction(tr("New"), this, &MainWindow::onNewSession);
    toolBar->addAction(tr("Connect"), this, &MainWindow::onConnect);
    toolBar->addAction(tr("Disconnect"), this, &MainWindow::onDisconnect);
    toolBar->addSeparator();

    // PH2-08: Free Type — keyboard input goes to every other SSH tab.
    m_freeTypeAction = toolBar->addAction(tr("Free Type"), this, [this](bool on) {
        if (on) {
            int targets = 0;
            for (int i = 0; i < m_tabWidget->count(); ++i) {
                if (auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(i));
                    tab && tab->sshSession()) {
                    ++targets;
                }
            }
            if (targets > 0
                && QMessageBox::question(
                       this, tr("Free Type Mode"),
                       tr("Keyboard input will be sent to %1 other SSH tab(s) at the same "
                          "time.\nBeware of vim, sudo and password prompts in mirrored "
                          "tabs. Continue?")
                           .arg(targets),
                       QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes)
                       != QMessageBox::Yes) {
                const QSignalBlocker block(m_freeTypeAction);
                m_freeTypeAction->setChecked(false);
                return;
            }
        }
        m_freeTypeMode = on;
        statusBar()->showMessage(on ? tr("Free Type ON — input is mirrored to every other SSH tab")
                                    : tr("Free Type OFF"),
                                 on ? 0 : 3000);
    });
    m_freeTypeAction->setCheckable(true);
    toolBar->addSeparator();

    // PH1-06: quick connect — "user@host[:port]" (or bare "host") + Enter
    // opens an ad-hoc SSH tab; the password is prompted locally.
    m_quickConnectEdit = new QLineEdit(this);
    m_quickConnectEdit->setPlaceholderText(tr("Quick connect: user@host[:port]"));
    m_quickConnectEdit->setMinimumWidth(240);
    m_quickConnectEdit->setClearButtonEnabled(true);
    connect(m_quickConnectEdit, &QLineEdit::returnPressed, this, &MainWindow::onQuickConnect);
    toolBar->addWidget(m_quickConnectEdit);
    toolBar->addSeparator();

    auto *localButton = new QToolButton(this);
    localButton->setText(tr("Local"));
    localButton->setPopupMode(QToolButton::InstantPopup);
    localButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    QMenu *localMenu = new QMenu(localButton);
    localMenu->addAction(tr("Default Terminal"), this, [this]() {
        onNewLocalTerminal(QString());
    });
    localMenu->addSeparator();

    const QStringList shells = LocalShellProcess::availableShells();
    for (const QString &name : shells) {
        localMenu->addAction(name, this, [this, name]() {
            onNewLocalTerminal(name);
        });
    }
    localButton->setMenu(localMenu);
    toolBar->addWidget(localButton);

    toolBar->addSeparator();
    toolBar->addAction(tr("SFTP"), this, &MainWindow::onOpenSftp);
    toolBar->addAction(tr("Compare"), this, &MainWindow::onOpenCompare);
    toolBar->addAction(tr("Settings"), this, &MainWindow::onOpenSettings);
}

void MainWindow::setupDockWidgets()
{
    m_sessionManager = new SessionManagerWidget(this);
    m_sessionModel = m_sessionManager->model();

    connect(m_sessionManager, &SessionManagerWidget::sessionActivated, this, &MainWindow::onSessionActivated);
    connect(m_sessionManager, &SessionManagerWidget::newSessionRequested, this, &MainWindow::onNewSession);
    connect(m_sessionManager, &SessionManagerWidget::newFolderRequested, this, &MainWindow::onNewFolder);
    connect(m_sessionManager, &SessionManagerWidget::editRequested, this, [this](const QModelIndex &index) {
        if (m_sessionModel->nodeType(index) == SessionModel::NodeType::Session) {
            onEditSession(index);
        } else {
            onEditFolder(index);
        }
    });
    connect(m_sessionManager, &SessionManagerWidget::removeRequested, this, [this](const QModelIndex &index) {
        if (m_sessionModel->nodeType(index) == SessionModel::NodeType::Session) {
            onRemoveSession(index);
        } else {
            onRemoveFolder(index);
        }
    });
    connect(m_sessionManager, &SessionManagerWidget::duplicateRequested, this, [this](const QModelIndex &index) {
        if (m_sessionModel->nodeType(index) == SessionModel::NodeType::Session) {
            onDuplicateSession(index);
        }
    });
    // PH2-15: favorite pinning / tag editing.
    connect(m_sessionManager, &SessionManagerWidget::favoriteToggleRequested, this, [this](const QModelIndex &index) {
        if (m_sessionModel->nodeType(index) == SessionModel::NodeType::Session) {
            onToggleFavorite(index);
        }
    });
    connect(m_sessionManager, &SessionManagerWidget::tagsEditRequested, this, [this](const QModelIndex &index) {
        if (m_sessionModel->nodeType(index) == SessionModel::NodeType::Session) {
            onEditTags(index);
        }
    });

    m_sessionDock = new QDockWidget(tr("Session Manager"), this);
    m_sessionDock->setObjectName(QStringLiteral("sessionDock"));
    m_sessionDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_sessionDock->setWidget(m_sessionManager);
    addDockWidget(Qt::LeftDockWidgetArea, m_sessionDock);

    // Local file manager below the session tree (WindTerm-style left column).
    m_localFiles = new LocalFileWidget(this);
    m_fileDock = new QDockWidget(tr("File Manager"), this);
    m_fileDock->setObjectName(QStringLiteral("fileDock"));
    m_fileDock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    m_fileDock->setWidget(m_localFiles);
    addDockWidget(Qt::LeftDockWidgetArea, m_fileDock);
    splitDockWidget(m_sessionDock, m_fileDock, Qt::Vertical);

    // Transfer queue along the bottom.
    m_transfers = new TransfersWidget(this);
    m_transfersDock = new QDockWidget(tr("Transfers"), this);
    m_transfersDock->setObjectName(QStringLiteral("transfersDock"));
    m_transfers->setParent(m_transfersDock);
    m_transfersDock->setWidget(m_transfers);
    addDockWidget(Qt::BottomDockWidgetArea, m_transfersDock);

    // PH2-02: terminal outline (prompts / build steps / log headers).
    m_outlineDock = new QDockWidget(tr("Outline"), this);
    m_outlineDock->setObjectName(QStringLiteral("outlineDock"));
    auto *outline = new TerminalOutlineWidget(m_outlineDock);
    outline->setTerminalProvider([this]() -> TerminalWidget * {
        auto *tab = qobject_cast<SessionTab *>(m_tabWidget->currentWidget());
        return tab && tab->terminalSession() ? tab->terminalSession()->terminalWidget()
                                             : nullptr;
    });
    m_outlineDock->setWidget(outline);
    addDockWidget(Qt::RightDockWidgetArea, m_outlineDock);
    m_outlineDock->hide();

    // B6-1: server monitor (CPU/mem curves, net rates, disks) — samples over
    // a dedicated connection, never through the visible terminal.
    m_monitorDock = new QDockWidget(tr("Server Monitor"), this);
    m_monitorDock->setObjectName(QStringLiteral("monitorDock"));
    auto *monitor = new MonitorWidget(m_monitorDock);
    monitor->setSessionProvider([this]() -> QList<QPair<QString, SessionConfig>> {
        // Called during MainWindow construction (docks build before the
        // central tab widget) — stay null-safe.
        QList<QPair<QString, SessionConfig>> sessions;
        if (!m_tabWidget) {
            return sessions;
        }
        for (int i = 0; i < m_tabWidget->count(); ++i) {
            auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(i));
            if (tab && tab->config().sessionType() == SessionType::Ssh && tab->sshSession()
                && tab->sshSession()->isConnected()) {
                sessions.append({m_tabWidget->tabText(i), tab->config()});
            }
        }
        return sessions;
    });
    m_monitorDock->setWidget(monitor);
    addDockWidget(Qt::RightDockWidgetArea, m_monitorDock);
    m_monitorDock->hide();
}

void MainWindow::setupCentralWidget()
{
    QWidget *centralWidget = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_tabWidget = new QTabWidget(this);
    m_tabWidget->setTabsClosable(true);
    m_tabWidget->setMovable(true);
    m_tabWidget->setDocumentMode(true);

    // PH1-08: per-tab context menu (rename / color / pin / clone / sync).
    m_tabWidget->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tabWidget->tabBar(), &QWidget::customContextMenuRequested,
            this, &MainWindow::onTabContextMenu);
    // PH1-08: dragging a tab out of the bar detaches it into its own window.
    m_tabWidget->tabBar()->installEventFilter(this);

    // PH1-08: Ctrl+1..9 jumps to the n-th tab.
    for (int i = 1; i <= 9; ++i) {
        auto *shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(i)), this);
        connect(shortcut, &QShortcut::activated, this, [this, i]() {
            if (i - 1 < m_tabWidget->count()) {
                m_tabWidget->setCurrentIndex(i - 1);
            }
        });
    }

    // PH2-07: command palette.
    auto *paletteShortcut = new QShortcut(QKeySequence(tr("Ctrl+Shift+P")), this);
    connect(paletteShortcut, &QShortcut::activated, this, &MainWindow::showCommandPalette);

    connect(m_tabWidget, &QTabWidget::tabCloseRequested, this, [this](int index) {
        QWidget *widget = m_tabWidget->widget(index);
        auto *tab = qobject_cast<SessionTab *>(widget);
        if (tab) {
            // PH1-08: pinned tabs refuse close requests (unpin first).
            if (tab->property("pinned").toBool()) {
                statusBar()->showMessage(
                    tr("Tab '%1' is pinned — unpin it before closing.")
                        .arg(m_tabWidget->tabText(index)), 4000);
                return;
            }
            tab->disconnectSession();
        }
        m_tabWidget->removeTab(index);
        // removeTab only detaches the widget; delete it so the shell process
        // object (and its ConPTY/session resources) is actually destroyed.
        if (widget) {
            if (tab) {
                InputBroadcaster::instance().unregisterTab(tab);
                m_syncInputTabs.removeAll(tab);
            }
            widget->deleteLater();
        }
        updateStatusBarInfo();
    });
    connect(m_tabWidget, &QTabWidget::currentChanged, this, [this](int) {
        updateStatusBarInfo();
    });

    QLabel *welcomeLabel = new QLabel(tr("Welcome to hssh\n\n"
                                          "A fully open-source GUI SSH client.\n"
                                          "Use File > New Session to connect to a remote host."));
    welcomeLabel->setAlignment(Qt::AlignCenter);
    m_tabWidget->addTab(welcomeLabel, tr("Welcome"));

    layout->addWidget(m_tabWidget);
    setCentralWidget(centralWidget);
}

void MainWindow::setupStatusBar()
{
    // WindTerm-style right-aligned session/terminal info; the QSS paints the
    // status bar with the accent color.
    m_sessionInfoLabel = new QLabel(this);
    m_termSizeLabel = new QLabel(this);
    statusBar()->addPermanentWidget(m_sessionInfoLabel);
    statusBar()->addPermanentWidget(m_termSizeLabel);
    statusBar()->showMessage(tr("Ready"));
    updateStatusBarInfo();
}

void MainWindow::updateStatusBarInfo()
{
    if (!m_sessionInfoLabel || !m_termSizeLabel) {
        return;
    }
    QWidget *widget = m_tabWidget ? m_tabWidget->currentWidget() : nullptr;
    if (auto *tab = qobject_cast<SessionTab *>(widget)) {
        const SessionConfig config = tab->config();
        m_sessionInfoLabel->setText(config.sessionType() == SessionType::Ssh
                                        ? tr("SSH · %1").arg(config.displayName())
                                        : tr("Local · %1").arg(config.shellType()));
        // The size label is filled by the tab's sizeChanged signal; clear it
        // until the next emission.
        m_termSizeLabel->clear();
    } else if (widget) {
        m_sessionInfoLabel->setText(m_tabWidget->tabText(m_tabWidget->currentIndex()));
        m_termSizeLabel->clear();
    } else {
        m_sessionInfoLabel->clear();
        m_termSizeLabel->clear();
    }
}

void MainWindow::loadSessions()
{
    if (!m_sessionRepository->initialize()) {
        statusBar()->showMessage(tr("Failed to open session database: %1").arg(m_sessionRepository->lastError()), 5000);
        return;
    }

    const SessionRepository::TreeData data = m_sessionRepository->loadTree();
    m_sessionModel->loadSessions(data.sessions, data.folderPaths, data.folderNames);
    updateRecentMenu();
}

void MainWindow::onNewSession()
{
    NewSessionDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    SessionConfig config = dialog.sessionConfig();
    config.setGroup(selectedFolderIndex().isValid() ? m_sessionModel->sessionConfig(selectedFolderIndex()).group() : QString());

    if (!m_sessionRepository->saveSession(config)) {
        QMessageBox::warning(this, tr("Error"), tr("Failed to save session: %1").arg(m_sessionRepository->lastError()));
        return;
    }

    m_sessionModel->addSession(config, selectedFolderIndex());
}

void MainWindow::onNewLocalTerminal(const QString &shellType)
{
    auto *tab = SessionTab::createLocal(shellType, this);
    wireSessionTab(tab);
    const QString title = tab->config().displayName();
    const int index = m_tabWidget->addTab(tab, title);
    m_tabWidget->setCurrentIndex(index);
    InputBroadcaster::instance().registerTab(tab);
}

void MainWindow::onImportSshConfig()
{
    ImportSshConfigDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const QList<SessionConfig> configs = dialog.selectedConfigs();
    if (configs.isEmpty()) {
        return;
    }
    int imported = 0;
    for (const SessionConfig &config : configs) {
        if (!m_sessionRepository->saveSession(config)) {
            QMessageBox::warning(this, tr("Error"),
                                 tr("Failed to save session '%1': %2")
                                     .arg(config.displayName(),
                                          m_sessionRepository->lastError()));
            continue;
        }
        m_sessionModel->addSession(config);
        ++imported;
    }
    statusBar()->showMessage(tr("Imported %1 session(s) from ssh config").arg(imported), 5000);
}

void MainWindow::onEditSession(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }

    SessionConfig config = m_sessionModel->sessionConfig(index);
    NewSessionDialog dialog(this);
    dialog.setSessionConfig(config);
    dialog.setWindowTitle(tr("Edit Session"));

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    config = dialog.sessionConfig();
    if (!m_sessionRepository->saveSession(config)) {
        QMessageBox::warning(this, tr("Error"), tr("Failed to save session: %1").arg(m_sessionRepository->lastError()));
        return;
    }

    m_sessionModel->setSessionConfig(index, config);
}

void MainWindow::onDuplicateSession(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }

    const SessionConfig source = m_sessionModel->sessionConfig(index);
    // Full-field copy (credentials, key paths, keepalive, ...), but with a
    // FRESH id: saveSession is INSERT OR REPLACE, so keeping the source id
    // would silently overwrite the original.
    QVariantMap map = source.toMap();
    map[QStringLiteral("id")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    SessionConfig copy = SessionConfig::fromMap(map);
    copy.setName(source.name() + QStringLiteral("-copy"));

    NewSessionDialog dialog(this);
    dialog.setSessionConfig(copy);
    dialog.setWindowTitle(tr("Duplicate Session"));
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    copy = dialog.sessionConfig();
    if (!m_sessionRepository->saveSession(copy)) {
        QMessageBox::warning(this, tr("Error"),
                             tr("Failed to save session: %1").arg(m_sessionRepository->lastError()));
        return;
    }
    // Insert next to the source so the copy is visible immediately.
    m_sessionModel->addSession(copy, index.parent());
}

// PH2-15: pin/unpin a session. Full reload instead of setSessionConfig
// because the favorite flag moves the row through the proxy's sort order
// (folders → favorites → alphabetical) and loadSessions keeps the model and
// the database in one step.
void MainWindow::onToggleFavorite(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }

    SessionConfig config = m_sessionModel->sessionConfig(index);
    config.setFavorite(!config.favorite());
    if (!m_sessionRepository->saveSession(config)) {
        QMessageBox::warning(this, tr("Error"),
                             tr("Failed to save session: %1").arg(m_sessionRepository->lastError()));
        return;
    }
    loadSessions();
}

// PH2-15: edit the free-form tags (comma separated, searchable via the
// quick filter).
void MainWindow::onEditTags(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }

    const SessionConfig config = m_sessionModel->sessionConfig(index);
    bool ok = false;
    const QString text = QInputDialog::getText(
        this, tr("Edit Tags"), tr("Tags for '%1' (comma separated):").arg(config.displayName()),
        QLineEdit::Normal, config.tags().join(QStringLiteral(", ")), &ok);
    if (!ok) {
        return;
    }

    QStringList tags;
    for (QString tag : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        tag = tag.trimmed();
        if (!tag.isEmpty() && !tags.contains(tag, Qt::CaseInsensitive)) {
            tags.append(tag);
        }
    }
    tags.sort(Qt::CaseInsensitive);

    if (tags == config.tags()) {
        return;
    }

    SessionConfig updated = config;
    updated.setTags(tags);
    if (!m_sessionRepository->saveSession(updated)) {
        QMessageBox::warning(this, tr("Error"),
                             tr("Failed to save session: %1").arg(m_sessionRepository->lastError()));
        return;
    }
    loadSessions();
    statusBar()->showMessage(tr("Tags updated for '%1'").arg(updated.displayName()), 3000);
}

// PH1-06: "user@host[:port]", "host:port", "user@host" or bare "host".
void MainWindow::onQuickConnect()
{
    const QString text = m_quickConnectEdit->text().trimmed();
    if (text.isEmpty()) {
        return;
    }
    QString user;
    QString host = text;
    QString portStr;
    const int at = text.indexOf(QLatin1Char('@'));
    if (at > 0) {
        user = text.left(at);
        host = text.mid(at + 1);
    }
    const int colon = host.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        // IPv6 literals are not supported by the quick bar (use a session).
        const QString maybePort = host.mid(colon + 1);
        bool ok = false;
        const int port = maybePort.toInt(&ok);
        if (ok && port > 0 && port < 65536) {
            portStr = maybePort;
            host = host.left(colon);
        }
    }
    if (host.isEmpty()) {
        statusBar()->showMessage(tr("Quick connect: expected user@host[:port]"), 4000);
        return;
    }

    SessionConfig config;
    config.setHost(host);
    config.setPort(portStr.isEmpty() ? 22 : portStr.toInt());
    config.setUsername(user.isEmpty() ? QStringLiteral("root") : user);
    config.setAuthMethod(AuthMethod::Password);
    // Local password prompt (no echo); empty input is accepted for
    // key-based/agent setups where the password is simply not needed.
    bool ok = false;
    const QString password = QInputDialog::getText(
        this, tr("Quick Connect"),
        tr("Password for %1@%2:%3 (leave empty for key auth):")
            .arg(config.username(), host).arg(config.port()),
        QLineEdit::Password, QString(), &ok);
    if (!ok) {
        return; // cancelled
    }
    if (!password.isEmpty()) {
        config.setPassword(SecureString(password));
    }

    const int index = openSshTab(config);
    if (index < 0) {
        statusBar()->showMessage(tr("Quick connect: invalid target"), 4000);
        return;
    }
    m_quickConnectEdit->clear();
    statusBar()->showMessage(tr("Connecting to %1@%2:%3 ...")
                                 .arg(config.username(), host).arg(config.port()), 4000);
}

// PH1-08: rename (local alias), color, pin, clone, sync-input, close-others.
void MainWindow::onTabContextMenu(const QPoint &pos)
{
    const int index = m_tabWidget->tabBar()->tabAt(pos);
    if (index < 0) {
        return;
    }
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    if (!tab) {
        return;
    }
    m_tabWidget->setCurrentIndex(index);

    QMenu menu(this);
    menu.addAction(tr("Rename"), this, [this, index]() {
        bool ok = false;
        const QString name = QInputDialog::getText(
            this, tr("Rename Tab"), tr("Tab title:"),
            QLineEdit::Normal, m_tabWidget->tabText(index), &ok);
        if (ok && !name.trimmed().isEmpty()) {
            m_tabWidget->setTabText(index, name.trimmed());
        }
    });
    // PH1-08: detach into a floating window (drag-out does the same).
    menu.addAction(tr("Detach to Window"), this, [this, index]() {
        detachTab(index);
    });
    menu.addAction(tr("Color"), this, [this, index]() {
        // Compact preset palette keeps tab colors meaningful as groups.
        static const QList<QColor> palette = {
            QColor(0xe7, 0x48, 0x49), QColor(0xf7, 0x6b, 0x1c), QColor(0xff, 0xc9, 0x00),
            QColor(0x13, 0xbb, 0x70), QColor(0x00, 0xa4, 0xef), QColor(0x8e, 0x8c, 0xd8),
            QColor(0xb1, 0x46, 0xc1), QColor(0x60, 0x60, 0x60),
        };
        QWidget *tabWidget = m_tabWidget->widget(index);
        QColor current = tabWidget->property("tabColor").value<QColor>();
        // Simple grid dialog built from QColorDialog with a preset is not
        // portable; use a small inline picker.
        QMenu picker(this);
        QAction *none = picker.addAction(tr("No color"));
        for (const QColor &c : palette) {
            QAction *a = picker.addAction(picker.style()->standardIcon(QStyle::SP_ArrowRight),
                                          QString());
            a->setIcon(colorSwatchIcon(c));
        }
        QAction *chosen = picker.exec(QCursor::pos());
        if (!chosen) {
            return;
        }
        if (chosen == none) {
            applyTabColor(index, QColor());
        } else {
            applyTabColor(index, palette.at(picker.actions().indexOf(chosen) - 1));
        }
    });
    const bool pinned = tab->property("pinned").toBool();
    menu.addAction(tr(pinned ? "Unpin" : "Pin"), this, [this, index, pinned]() {
        setTabPinned(index, !pinned);
    });
    menu.addAction(tr("Clone Tab"), this, [this, tab]() {
        // Same config, fresh tab (ad-hoc copies do not touch the repository).
        const int i = openSshTab(tab->config());
        if (i >= 0) {
            m_tabWidget->setCurrentIndex(i);
        }
    });
    menu.addAction(tr("ZMODEM Send File..."), this, [this, tab]() {
        const QString path = QFileDialog::getOpenFileName(this, tr("Send via ZMODEM"));
        if (!path.isEmpty()) {
            if (!tab->zmodemSendFile(path)) {
                statusBar()->showMessage(
                    tr("Cannot start ZMODEM send (a transfer is active, or the "
                       "file is unreadable/empty)"), 5000);
            }
        }
    });
    // PH2-15: open this tab's own log file (preselected in the viewer).
    if (tab->terminalSession() && !tab->terminalSession()->logFilePath().isEmpty()) {
        menu.addAction(tr("View Session Log..."), this, [this, tab]() {
            SessionLogViewer viewer(this);
            viewer.selectFile(tab->terminalSession()->logFilePath());
            viewer.exec();
        });
    }
    // B6-2: process manager for SSH tabs (own connection, not the terminal).
    if (tab->config().sessionType() == SessionType::Ssh) {
        menu.addAction(tr("Process List..."), this, [this, tab]() {
            ProcessDialog dialog(tab->config(), this);
            dialog.exec();
        });
        // B6-3/4/5: network tools, docker management, scheduled tasks —
        // all on dedicated connections (or visible injection for terminal
        // mode tasks).
        menu.addAction(tr("Network Tools..."), this, [this, tab]() {
            NetworkToolsDialog dialog(tab->config(), this);
            dialog.exec();
        });
        menu.addAction(tr("Docker..."), this, [this, tab]() {
            DockerDialog dialog(tab->config(), this);
            dialog.exec();
        });
        menu.addAction(tr("Scheduled Tasks..."), this, [this, tab]() {
            SchedulerDialog dialog(tab, this);
            dialog.exec();
        });
    }
    menu.addSeparator();
    QAction *sync = menu.addAction(tr("Sync Input"), this, [this, tab](bool checked) {
        if (checked) {
            if (!m_syncInputTabs.contains(tab)) {
                m_syncInputTabs.append(tab);
            }
        } else {
            m_syncInputTabs.removeAll(tab);
        }
        statusBar()->showMessage(
            checked ? tr("Sync input ON for '%1' — typing is mirrored to %2 other tab(s)")
                          .arg(tab->config().displayName())
                          .arg(m_syncInputTabs.count() - 1)
                    : tr("Sync input OFF for '%1'").arg(tab->config().displayName()),
            4000);
    });
    sync->setCheckable(true);
    sync->setChecked(m_syncInputTabs.contains(tab));
    sync->setEnabled(m_tabWidget->count() > 1 || m_syncInputTabs.size() > 1);
    menu.addSeparator();
    menu.addAction(tr("Close Other Tabs"), this, [this, index]() {
        // Close from the end so indices stay valid.
        for (int i = m_tabWidget->count() - 1; i >= 0; --i) {
            if (i == index) {
                continue;
            }
            auto *other = qobject_cast<SessionTab *>(m_tabWidget->widget(i));
            if (other && other->property("pinned").toBool()) {
                continue; // pinned tabs refuse close
            }
            closeTab(i);
        }
    });
    menu.exec(m_tabWidget->tabBar()->mapToGlobal(pos));
}

void MainWindow::applyTabColor(int index, const QColor &color)
{
    QWidget *widget = m_tabWidget->widget(index);
    if (!widget) {
        return;
    }
    if (color.isValid()) {
        widget->setProperty("tabColor", color);
        m_tabWidget->setTabIcon(index, colorSwatchIcon(color));
    } else {
        widget->setProperty("tabColor", QVariant());
        m_tabWidget->setTabIcon(index, QIcon());
    }
}

void MainWindow::setTabPinned(int index, bool pinned)
{
    QWidget *widget = m_tabWidget->widget(index);
    if (!widget) {
        return;
    }
    widget->setProperty("pinned", pinned);
    if (pinned) {
        // Pinned tabs live at the front, in pin order.
        m_tabWidget->tabBar()->moveTab(index, 0);
        statusBar()->showMessage(
            tr("Pinned '%1' (protected from close)").arg(m_tabWidget->tabText(0)), 4000);
    }
}

// PH1-08: tab detach. Dragging a tab more than ~12px outside the tab bar
// aborts the built-in reorder and moves the tab into a FloatingTabWindow.
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_tabWidget->tabBar()) {
        const QEvent::Type type = event->type();
        if (type == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(event);
            m_barPressedIndex = (me->button() == Qt::LeftButton)
                                    ? m_tabWidget->tabBar()->tabAt(me->pos())
                                    : -1;
        } else if (type == QEvent::MouseButtonRelease) {
            m_barPressedIndex = -1;
        } else if (type == QEvent::MouseMove && m_barPressedIndex >= 0) {
            auto *me = static_cast<QMouseEvent *>(event);
            if (me->buttons() & Qt::LeftButton) {
                const QRect escapeZone = m_tabWidget->tabBar()->rect().adjusted(-12, -12, 12, 12);
                if (!escapeZone.contains(me->pos())) {
                    const int index = m_barPressedIndex;
                    m_barPressedIndex = -1;
                    // End the internal reorder drag before removing the tab.
                    QMouseEvent release(QEvent::MouseButtonRelease, me->position(),
                                        me->globalPosition(), Qt::LeftButton, Qt::NoButton,
                                        me->modifiers());
                    QCoreApplication::sendEvent(watched, &release);
                    detachTab(index);
                    return true;
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::detachTab(int index)
{
    if (index < 0 || index >= m_tabWidget->count()) {
        return;
    }
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    if (!tab) {
        return; // the welcome placeholder is not detachable
    }
    if (tab->property("pinned").toBool()) {
        statusBar()->showMessage(
            tr("Tab '%1' is pinned — unpin it before detaching.")
                .arg(m_tabWidget->tabText(index)), 4000);
        return;
    }
    const QString title = m_tabWidget->tabText(index);
    const QIcon icon = m_tabWidget->tabIcon(index);
    m_tabWidget->removeTab(index);

    auto *window = new FloatingTabWindow(tab, title, icon);
    connect(window, &FloatingTabWindow::dockRequested, this,
            &MainWindow::dockFloatingTab);
    connect(window, &FloatingTabWindow::closeRequested, this,
            &MainWindow::closeFloatingTab);
    m_floatingTabs.append(window);
    window->move(QCursor::pos() - QPoint(window->width() / 2, 16));
    window->show();
    statusBar()->showMessage(tr("'%1' detached to its own window").arg(title), 4000);
    updateStatusBarInfo();
}

void MainWindow::dockFloatingTab(FloatingTabWindow *window)
{
    if (!window) {
        return;
    }
    SessionTab *tab = window->tab();
    if (!tab) {
        window->deleteLater();
        return;
    }
    const bool pinned = tab->property("pinned").toBool();
    const QIcon icon = window->tabIcon();
    const QString title = window->tabTitle();
    window->takeCentralWidget(); // ownership of the tab back to us
    m_floatingTabs.removeAll(window);
    window->deleteLater();

    const int index = m_tabWidget->addTab(tab, title);
    if (!icon.isNull()) {
        m_tabWidget->setTabIcon(index, icon);
    }
    if (pinned) {
        setTabPinned(index, true);
    }
    m_tabWidget->setCurrentIndex(m_tabWidget->indexOf(tab));
    statusBar()->showMessage(tr("'%1' docked back").arg(title), 4000);
    updateStatusBarInfo();
}

void MainWindow::closeFloatingTab(FloatingTabWindow *window)
{
    if (!window) {
        return;
    }
    SessionTab *tab = window->tab();
    m_floatingTabs.removeAll(window);
    window->takeCentralWidget(); // keep deleteLater from destroying the tab
    window->deleteLater();
    if (tab) {
        tab->disconnectSession();
        InputBroadcaster::instance().unregisterTab(tab);
        m_syncInputTabs.removeAll(tab);
        tab->deleteLater();
    }
    updateStatusBarInfo();
}

// PH2-08: keyboard input typed in a sync-group tab is mirrored verbatim to
// the other members. Mirrored input re-enters through the API path, which
// does not re-emit inputTyped — no feedback loop.
void MainWindow::routeSyncInput(SessionTab *source, const QByteArray &data)
{
    QList<SessionTab *> targets;
    if (m_freeTypeMode) {
        // Free Type: every other SSH tab mirrors the input.
        for (int i = 0; i < m_tabWidget->count(); ++i) {
            if (auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(i));
                tab && tab != source && tab->sshSession()) {
                targets.append(tab);
            }
        }
    } else if (m_syncInputTabs.contains(source)) {
        for (QPointer<SessionTab> tab : std::as_const(m_syncInputTabs)) {
            if (tab && tab != source) {
                targets.append(tab);
            }
        }
    }
    if (!targets.isEmpty()) {
        InputBroadcaster::instance().broadcast(targets, QString::fromUtf8(data), false);
    }
}

// Shared wiring for every SessionTab, wherever it is created from.
void MainWindow::wireSessionTab(SessionTab *tab)
{
    connect(tab, &SessionTab::sizeChanged, this, [this, tab](int columns, int rows) {
        if (m_tabWidget->currentWidget() == tab) {
            m_termSizeLabel->setText(tr("%1×%2").arg(columns).arg(rows));
        }
    });
    connect(tab, &SessionTab::inputTyped, this, [this, tab](const QByteArray &data) {
        routeSyncInput(tab, data);
    });
    // PH1-05: honor the persisted terminal font.
    if (m_terminalFont.has_value()) {
        tab->setTerminalFont(*m_terminalFont);
    }
}

void MainWindow::applyTerminalFont(const QFont &font)
{
    m_terminalFont = font;
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        if (auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(i))) {
            tab->setTerminalFont(font);
        }
    }
}

QIcon MainWindow::colorSwatchIcon(const QColor &color)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter painter(&pm);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(color);
    painter.setPen(Qt::NoPen);
    painter.drawRoundedRect(2, 6, 12, 5, 2, 2);
    return QIcon(pm);
}

void MainWindow::onRemoveSession(const QModelIndex &index)
{    if (!index.isValid()) {
        return;
    }

    const SessionConfig config = m_sessionModel->sessionConfig(index);
    const int result = QMessageBox::question(this, tr("Confirm"),
                                              tr("Remove session '%1'?").arg(config.displayName()),
                                              QMessageBox::Yes | QMessageBox::No);
    if (result != QMessageBox::Yes) {
        return;
    }

    if (!m_sessionRepository->removeSession(config.id())) {
        QMessageBox::warning(this, tr("Error"), tr("Failed to remove session: %1").arg(m_sessionRepository->lastError()));
        return;
    }

    m_sessionModel->removeNode(index);
}

void MainWindow::onNewFolder()
{
    EditFolderDialog dialog(this);
    dialog.setWindowTitle(tr("New Folder"));
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString parentId = selectedFolderIndex().isValid() ? m_sessionModel->sessionConfig(selectedFolderIndex()).group() : QString();
    const QString folderId = m_sessionRepository->addFolder(dialog.folderName(), parentId);
    if (folderId.isEmpty()) {
        QMessageBox::warning(this, tr("Error"), tr("Failed to save folder: %1").arg(m_sessionRepository->lastError()));
        return;
    }

    m_sessionModel->addFolder(dialog.folderName(), selectedFolderIndex());
}

void MainWindow::onEditFolder(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }

    const QString folderId = m_sessionModel->nodeId(index);
    if (folderId.isEmpty()) {
        return;
    }

    EditFolderDialog dialog(this);
    dialog.setWindowTitle(tr("Rename Folder"));
    dialog.setFolderName(m_sessionModel->data(index, Qt::DisplayRole).toString());
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    const QString newName = dialog.folderName();
    if (!m_sessionRepository->updateFolder(folderId, newName)) {
        QMessageBox::warning(this, tr("Error"), tr("Failed to rename folder: %1").arg(m_sessionRepository->lastError()));
        return;
    }

    m_sessionModel->setNodeName(index, newName);
}

void MainWindow::onRemoveFolder(const QModelIndex &index)
{
    if (!index.isValid()) {
        return;
    }

    const int result = QMessageBox::question(this, tr("Confirm"),
                                              tr("Remove this folder and all its sessions?"),
                                              QMessageBox::Yes | QMessageBox::No);
    if (result != QMessageBox::Yes) {
        return;
    }

    const QString folderId = m_sessionModel->nodeId(index);
    if (!m_sessionRepository->removeFolder(folderId)) {
        QMessageBox::warning(this, tr("Error"), tr("Failed to remove folder: %1").arg(m_sessionRepository->lastError()));
        return;
    }

    m_sessionModel->removeNode(index);
}

void MainWindow::onSessionActivated(const SessionConfig &config)
{
    m_sessionRepository->recordSessionUse(config.id());
    updateRecentMenu();

    auto *tab = new SessionTab(config, this);
    wireSessionTab(tab);
    const int index = m_tabWidget->addTab(tab, config.displayName());
    m_tabWidget->setCurrentIndex(index);
    InputBroadcaster::instance().registerTab(tab);
}

// Ad-hoc SSH tab for the agent API (POST /tabs with host+...). Unlike
// onSessionActivated there is no repository entry to record.
int MainWindow::openSshTab(const SessionConfig &config)
{
    if (config.sessionType() != SessionType::Ssh || config.host().isEmpty()) {
        return -1;
    }
    auto *tab = new SessionTab(config, this);
    wireSessionTab(tab);
    const int index = m_tabWidget->addTab(tab, config.displayName());
    m_tabWidget->setCurrentIndex(index);
    InputBroadcaster::instance().registerTab(tab);
    return index;
}

void MainWindow::startAgent(int port)
{
    if (!m_agentServer) {
        m_agentPort = port;
        onToggleAgent();
    }
}

void MainWindow::onConnect()
{
    const QModelIndex index = selectedSessionIndex();
    if (!index.isValid()) {
        QMessageBox::information(this, tr("Connect"), tr("Please select a session first."));
        return;
    }
    onSessionActivated(m_sessionModel->sessionConfig(index));
}

void MainWindow::onDisconnect()
{
    const int index = m_tabWidget->currentIndex();
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    if (!tab) {
        // Current tab is not a terminal session (SFTP/compare/welcome).
        statusBar()->showMessage(tr("Select a terminal tab to disconnect."), 3000);
        return;
    }
    tab->disconnectSession();
    m_tabWidget->removeTab(index);
    InputBroadcaster::instance().unregisterTab(tab);
    tab->deleteLater();
}

void MainWindow::onOpenSftp()
{
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->currentWidget());
    if (!tab || tab->config().sessionType() != SessionType::Ssh) {
        statusBar()->showMessage(tr("Select an SSH session tab to open SFTP."), 5000);
        return;
    }
    addSftpTab(tab->config());
}

void MainWindow::addSftpTab(const SessionConfig &config, const QString &remotePath)
{
    auto *widget = new SftpWidget(config, this);
    if (!remotePath.isEmpty()) {
        // Queued: navigate once the connection is up (navigateTo before the
        // first listing would race the home-directory navigation).
        connect(widget, &SftpWidget::dirChanged, widget, [widget, remotePath](const QString &) {
            if (widget->currentPath() != remotePath) {
                widget->navigateTo(remotePath);
            }
        }, Qt::SingleShotConnection);
    }
    // Context-menu "Compare with local folder…" opens a compare tab rooted at
    // this remote directory.
    connect(widget, &SftpWidget::compareRequested, this, [this, config, widget]() {
        auto *compare = new FileCompareWidget(config, m_sessionRepository, this);
        const int index = m_tabWidget->addTab(compare, tr("Compare: %1").arg(config.displayName()));
        m_tabWidget->setCurrentIndex(index);
        if (!widget->currentPath().isEmpty()) {
            compare->navigateRemote(widget->currentPath());
        }
    });
    const int index = m_tabWidget->addTab(widget, tr("SFTP: %1").arg(config.displayName()));
    m_tabWidget->setCurrentIndex(index);
}

void MainWindow::onOpenCompare()
{
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->currentWidget());
    if (!tab || tab->config().sessionType() != SessionType::Ssh) {
        statusBar()->showMessage(tr("Select an SSH session tab to open the compare view."), 5000);
        return;
    }
    const SessionConfig config = tab->config();
    auto *widget = new FileCompareWidget(config, m_sessionRepository, this);
    const int index = m_tabWidget->addTab(widget, tr("Compare: %1").arg(config.displayName()));
    m_tabWidget->setCurrentIndex(index);
}

void MainWindow::onPortForwarding()
{
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->currentWidget());
    SshSession *session = tab ? tab->sshSession() : nullptr;
    if (!session || !session->isConnected()) {
        statusBar()->showMessage(tr("Select a connected SSH session tab to manage port forwarding."), 5000);
        return;
    }
    auto *dialog = new PortForwardDialog(session, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void MainWindow::onImportSessions()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Sessions"),
                                                       QString(), tr("hssh sessions (*.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    if (!m_sessionRepository->importSessionsFromJson(path)) {
        QMessageBox::warning(this, tr("Import Failed"),
                             tr("Could not import sessions: %1").arg(m_sessionRepository->lastError()));
        return;
    }
    loadSessions();
    statusBar()->showMessage(tr("Sessions imported."), 5000);
}

void MainWindow::onExportSessions()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Sessions"),
                                                       QStringLiteral("hssh-sessions.json"),
                                                       tr("hssh sessions (*.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    if (!m_sessionRepository->exportSessionsToJson(path)) {
        QMessageBox::warning(this, tr("Export Failed"),
                             tr("Could not export sessions: %1").arg(m_sessionRepository->lastError()));
        return;
    }
    statusBar()->showMessage(tr("Sessions exported to %1").arg(path), 5000);
}

void MainWindow::onToggleAgent()
{
    if (m_agentServer) {
        m_agentServer->stop();
        m_agentServer->deleteLater();
        m_agentServer = nullptr;
        m_agentAction->setText(tr("Start Agent"));
        statusBar()->showMessage(tr("Agent stopped."), 5000);
        return;
    }

    m_agentServer = new AgentHttpServer(this);
    m_agentServer->setTabsInterface(this);
    connect(m_agentServer, &AgentHttpServer::acceptErrorOccurred, this,
            [this](const QString &message) {
                statusBar()->showMessage(
                    tr("Agent accept error on %1: %2").arg(m_agentServer->url(), message),
                    15000);
            });
    if (!m_agentServer->start(m_agentPort)) {
        qWarning("agent start failed on port %d: %s",
                 m_agentPort, qPrintable(m_agentServer->errorString()));
        QMessageBox::warning(this, tr("Agent"),
                             tr("Failed to start agent: %1\n\n"
                                "Another hssh instance (or MCP auto-launched GUI) may already "
                                "own the port — check GET http://127.0.0.1:8222/api/v1/health "
                                "for its pid.")
                                 .arg(m_agentServer->errorString()));
        m_agentServer->deleteLater();
        m_agentServer = nullptr;
        return;
    }
    m_agentAction->setText(tr("Stop Agent"));
    qInfo("agent listening on port %d", m_agentPort);
    statusBar()->showMessage(tr("Agent listening on %1").arg(m_agentServer->url()), 5000);
}

QVariantList MainWindow::listTabs() const
{
    QVariantList result;
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(i));
        if (!tab) {
            continue;
        }
        QVariantMap entry;
        entry[QStringLiteral("index")] = i;
        entry[QStringLiteral("title")] = m_tabWidget->tabText(i);
        const SessionConfig config = tab->config();
        entry[QStringLiteral("type")] = config.sessionType() == SessionType::Ssh
                                            ? QStringLiteral("ssh")
                                            : QStringLiteral("local");
        if (config.sessionType() == SessionType::Ssh) {
            entry[QStringLiteral("sessionName")] = config.name();
            entry[QStringLiteral("host")] = config.host();
            entry[QStringLiteral("port")] = config.port();
            entry[QStringLiteral("username")] = config.username();
        }
        const bool connected = tab->sshSession()
                                   ? tab->sshSession()->isConnected()
                                   : true;
        entry[QStringLiteral("connected")] = connected;
        // Ground truth: the live socket's peer address. The configured host
        // can diverge from the real transport (session edited after the tab
        // opened — the 2026-09-11 wrong-machine accident), so consumers must
        // be able to see both.
        if (connected && tab->sshSession()) {
            const QString peer = tabPeerAddress(tab->sshSession());
            if (!peer.isEmpty()) {
                entry[QStringLiteral("peer")] = peer;
            }
        }
        result.append(entry);
    }
    return result;
}

// IP address of the live SSH transport's remote peer, or empty when it
// cannot be determined (non-libssh backend, already gone).
QString MainWindow::tabPeerAddress(SshSession *session) const
{
#ifdef HSSH_HAS_LIBSSH
    if (!session) {
        return {};
    }
    ssh_session handle = session->sessionHandle();
    QMutex *mutex = session->sessionMutex();
    if (!handle || !mutex) {
        return {};
    }
    QMutexLocker locker(mutex);
    const socket_t fd = ssh_get_fd(handle);
    if (fd == SSH_INVALID_SOCKET) {
        return {};
    }
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        return {};
    }
    char host[NI_MAXHOST] = {};
    if (getnameinfo(reinterpret_cast<sockaddr *>(&addr), len,
                    host, sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0) {
        return {};
    }
    return QString::fromLatin1(host);
#else
    Q_UNUSED(session);
    return {};
#endif
}

QVariantList MainWindow::listSavedSessions() const
{
    QVariantList result;
    const QList<SessionConfig> sessions = m_sessionRepository->loadAllSessions();
    for (const SessionConfig &config : sessions) {
        QVariantMap entry;
        entry[QStringLiteral("name")] = config.name();
        entry[QStringLiteral("displayName")] = config.displayName();
        entry[QStringLiteral("host")] = config.host();
        entry[QStringLiteral("port")] = config.port();
        entry[QStringLiteral("username")] = config.username();
        // Records whose secret failed to decrypt come back with an empty id.
        entry[QStringLiteral("locked")] = config.id().isEmpty();
        result.append(entry);
    }
    return result;
}

bool MainWindow::sendToTab(int index, const QString &text)
{
    if (index < 0 || index >= m_tabWidget->count()) {
        return false;
    }
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    if (!tab) {
        return false;
    }
    // API semantic: "send a command" includes the Enter key.
    tab->runCommand(text.endsWith(QLatin1Char('\n')) ? text : text + QLatin1Char('\n'));
    return true;
}

bool MainWindow::sendInputToTab(int index, const QString &data)
{
    if (index < 0 || index >= m_tabWidget->count()) {
        return false;
    }
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    if (!tab) {
        return false;
    }
    // Raw input: no appended Enter, control bytes go through as-is.
    tab->runCommand(data);
    return true;
}

bool MainWindow::zmodemSendToTab(int index, const QString &localPath)
{
    if (index < 0 || index >= m_tabWidget->count()) {
        return false;
    }
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    if (!tab) {
        return false;
    }
    return tab->zmodemSendFile(localPath);
}

bool MainWindow::readTab(int index, int maxLines, QString *text) const
{
    return readTabRange(index, 0, maxLines, text);
}

bool MainWindow::readTabRange(int index, int fromLine, int maxLines, QString *text) const
{
    if (index < 0 || index >= m_tabWidget->count() || !text) {
        return false;
    }
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    if (!tab) {
        return false;
    }
    *text = tab->readTerminalTextRange(fromLine, maxLines);
    return true;
}

int MainWindow::openLocalTab(const QString &shellType)
{
    onNewLocalTerminal(shellType);
    return m_tabWidget->count() - 1;
}

bool MainWindow::sendSecretToTab(int index, const QString &text)
{
    // Same write path as sendToTab: input is never logged (only received
    // output is), and sudo-style prompts disable terminal echo anyway.
    return sendToTab(index, text);
}

SessionConfig MainWindow::sessionConfigForTab(int index) const
{
    if (index < 0 || index >= m_tabWidget->count()) {
        return {};
    }
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    return tab ? tab->config() : SessionConfig();
}

bool MainWindow::confirmSudo(int index, const QString &command, QString *reason)
{
    const auto fail = [reason](const QString &code) {
        if (reason) {
            *reason = code;
        }
        return false;
    };
    if (index < 0 || index >= m_tabWidget->count()) {
        return fail(QStringLiteral("unknown_tab"));
    }
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    if (!tab) {
        return fail(QStringLiteral("unknown_tab"));
    }

    if (m_sudoApproved.contains(tab)) {
        return true; // Already approved within this tab's lifetime.
    }

    // Permanent grant list ("Always Allow" in a previous dialog).
    const QString identity = AgentSudoAuth::identityFor(tab->config());
    if (AgentSudoAuth::isAlwaysAllowed(identity)) {
        return true;
    }

    QMessageBox box(this);
    box.setWindowTitle(tr("Sudo Confirmation"));
    box.setIcon(QMessageBox::Warning);
    box.setText(tr("An AI agent requests to run this command with sudo in session \"%1\":")
                    .arg(m_tabWidget->tabText(index)));
    box.setInformativeText(tr("Allow it? \"Always Allow\" grants sudo for this machine "
                               "permanently; plain approval lasts while this tab stays open. "
                               "This dialog closes automatically in 30 seconds."));
    box.setDetailedText(command);
    QPushButton *yesButton = box.addButton(QMessageBox::Yes);
    box.addButton(QMessageBox::No);
    QAbstractButton *alwaysButton = nullptr;
    if (!identity.isEmpty()) {
        alwaysButton = box.addButton(tr("Always Allow"), QMessageBox::AcceptRole);
    }
    box.setDefaultButton(QMessageBox::No);
    box.setWindowFlags(box.windowFlags() | Qt::WindowStaysOnTopHint);

    // An OWNED dialog is hidden while its owner is minimized — the request
    // then auto-rejects after 30 s without the user ever seeing anything
    // (observed as mysterious 403 storms). Restore the window first and
    // flash the taskbar when the foreground lock refuses focus.
    if (isMinimized()) {
        showNormal();
    }
    raise();
    activateWindow();
    QApplication::alert(this);

    AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("sudo_confirm"),
                    QStringLiteral("tab=%1 identity=%2").arg(index).arg(identity),
                    QStringLiteral("dialog shown"));

    // Unanswered dialogs must not hang the agent request forever.
    bool timedOut = false;
    QTimer autoReject;
    autoReject.setSingleShot(true);
    autoReject.setInterval(30000);
    connect(&autoReject, &QTimer::timeout, &box, [&timedOut, &box]() {
        timedOut = true;
        box.reject();
    });
    autoReject.start();

    box.exec();
    QAbstractButton *clicked = box.clickedButton();
    if (clicked != yesButton && clicked != alwaysButton) {
        const QString code = timedOut ? QStringLiteral("timeout")
                                      : QStringLiteral("user_rejected");
        AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("sudo_confirm"),
                        QStringLiteral("tab=%1 identity=%2").arg(index).arg(identity),
                        timedOut ? QStringLiteral("auto-rejected: no answer within 30 s")
                                 : QStringLiteral("rejected by user"));
        return fail(code);
    }
    if (clicked == alwaysButton) {
        AgentSudoAuth::setAlwaysAllowed(identity);
    }
    AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("sudo_confirm"),
                    QStringLiteral("tab=%1 identity=%2").arg(index).arg(identity),
                    clicked == alwaysButton ? QStringLiteral("approved (always)")
                                            : QStringLiteral("approved"));
    m_sudoApproved.append(tab);
    return true;
}

bool MainWindow::waitForPromptPattern(SessionTab *tab, const QStringList &patterns, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        const QString text = tab->readTerminalText(6).toLower();
        for (const QString &pattern : patterns) {
            if (text.contains(pattern.toLower())) {
                return true;
            }
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(120);
    }
    return false;
}

void MainWindow::sudoAsync(int index, const QString &command, const QString &secret,
                           bool useStoredCredential, int timeoutMs,
                           const AgentTabsInterface::SudoAsyncCallback &cb)
{
    using Result = AgentTabsInterface::SudoAsyncResult;

    // Synchronously decidable gates first (no dialog needed).
    auto *tab = (index >= 0 && index < m_tabWidget->count())
        ? qobject_cast<SessionTab *>(m_tabWidget->widget(index)) : nullptr;
    if (!tab) {
        cb(Result{false, QStringLiteral("unknown_tab"), false, QString(), false, -1, QString()});
        return;
    }
    const bool alreadyApproved = m_sudoApproved.contains(tab);
    const QString identity = AgentSudoAuth::identityFor(tab->config());
    const bool alwaysAllowed = !identity.isEmpty() && AgentSudoAuth::isAlwaysAllowed(identity);

    const auto runExecution = [this, tab, index, command, secret, useStoredCredential,
                               timeoutMs, cb, identity]() {
        QString output;
        QString errorMessage;
        bool timedOut = false;
        int exitCode = -1;
        const bool ok = sudoExec(index, command, secret, useStoredCredential, timeoutMs,
                                 &output, &timedOut, &exitCode, &errorMessage);
        Result r;
        r.confirmed = true;
        r.ran = ok;
        r.output = output;
        r.timedOut = timedOut;
        r.exitCode = exitCode;
        r.errorMessage = errorMessage;
        cb(r);
    };

    if (alreadyApproved || alwaysAllowed) {
        runExecution();
        return;
    }

    // Dialog WITHOUT a nested event loop: show() + finished signal + 30 s
    // auto-reject timer. The agent stays fully responsive while it is up.
    if (isMinimized()) {
        showNormal();
    }
    raise();
    activateWindow();
    QApplication::alert(this);

    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowTitle(tr("Sudo Confirmation"));
    box->setIcon(QMessageBox::Warning);
    box->setText(tr("An AI agent requests to run this command with sudo in session \"%1\":")
                     .arg(m_tabWidget->tabText(index)));
    box->setInformativeText(tr("Allow it? \"Always Allow\" grants sudo for this machine "
                                "permanently; plain approval lasts while this tab stays open. "
                                "This dialog closes automatically in 30 seconds."));
    box->setDetailedText(command);
    QPushButton *yesButton = box->addButton(QMessageBox::Yes);
    box->addButton(QMessageBox::No);
    QAbstractButton *alwaysButton = nullptr;
    if (!identity.isEmpty()) {
        alwaysButton = box->addButton(tr("Always Allow"), QMessageBox::AcceptRole);
    }
    box->setDefaultButton(QMessageBox::No);
    box->setWindowFlags(box->windowFlags() | Qt::WindowStaysOnTopHint);

    AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("sudo_confirm"),
                    QStringLiteral("tab=%1 identity=%2").arg(index).arg(identity),
                    QStringLiteral("dialog shown"));

    QPointer<QMessageBox> guard(box);
    QPointer<SessionTab> tabGuard(tab);
    auto *autoReject = new QTimer(box);
    autoReject->setSingleShot(true);
    connect(autoReject, &QTimer::timeout, box, [guard]() {
        if (guard) {
            guard->setProperty("hsshTimedOut", true);
            guard->reject();
        }
    });
    connect(box, &QMessageBox::finished, this,
            [this, guard, tabGuard, yesButton, alwaysButton, identity, index,
             runExecution, cb](int) {
        if (!guard) {
            return;
        }
        QAbstractButton *clicked = guard->clickedButton();
        const bool timedOutDialog = guard->property("hsshTimedOut").toBool();
        if (clicked != yesButton && clicked != alwaysButton) {
            const QString code = timedOutDialog ? QStringLiteral("timeout")
                                                : QStringLiteral("user_rejected");
            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("sudo_confirm"),
                            QStringLiteral("tab=%1 identity=%2").arg(index).arg(identity),
                            timedOutDialog ? QStringLiteral("auto-rejected: no answer within 30 s")
                                           : QStringLiteral("rejected by user"));
            cb(Result{false, code, false, QString(), false, -1, QString()});
            return;
        }
        if (clicked == alwaysButton) {
            AgentSudoAuth::setAlwaysAllowed(identity);
        }
        AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("sudo_confirm"),
                        QStringLiteral("tab=%1 identity=%2").arg(index).arg(identity),
                        clicked == alwaysButton ? QStringLiteral("approved (always)")
                                                : QStringLiteral("approved"));
        if (tabGuard) {
            m_sudoApproved.append(tabGuard);
        }
        runExecution();
    });
    autoReject->start(30000);
    box->show();
}

bool MainWindow::sudoExec(int index, const QString &command, const QString &secret,
                          bool useStoredCredential, int timeoutMs,
                          QString *output, bool *timedOut, int *exitCode,
                          QString *errorMessage)
{
    if (index < 0 || index >= m_tabWidget->count()) {
        if (errorMessage) {
            *errorMessage = tr("Unknown tab index: %1").arg(index);
        }
        return false;
    }
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    if (!tab) {
        if (errorMessage) {
            *errorMessage = tr("Tab %1 is not a terminal session").arg(index);
        }
        return false;
    }
    if (timedOut) {
        *timedOut = false;
    }
    if (exitCode) {
        *exitCode = -1;
    }

    // Completion sentinel appended after the command: proves the command
    // really ran (the confirmation popup may have delayed the injection into
    // a busy terminal) and carries the real exit code back. The echoed
    // command line shows the token followed by a literal "$?", so only the
    // actual output line matches the trailing-digits pattern.
    const QString token = QStringLiteral("__HSSH_SUDO_RC_%1__")
        .arg(QRandomGenerator::global()->generate64(), 16, 16, QLatin1Char('0'));
    tab->runCommand(QStringLiteral("sudo ") + command
                    + QStringLiteral("; echo %1$?\n").arg(token));

    // Wait briefly for a password prompt; no prompt means cached sudo.
    // 10 s window: slow links need longer than the old 5 s to echo the
    // prompt, and missing it means sudo starves on a password we never send.
    const QStringList passwordPatterns = {
        QStringLiteral("[sudo] password"),
        QStringLiteral("password for"),
        QStringLiteral("password:"),
    };
    const bool needsPassword = waitForPromptPattern(tab, passwordPatterns, 10000);

    if (needsPassword) {
        QString password = secret;
        if (password.isEmpty() && useStoredCredential) {
            password = tab->config().password().toString();
        }
        if (password.isEmpty()) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("passwordRequired");
            }
            return false;
        }
        tab->runCommand(password + QLatin1Char('\n'));
        password.fill(QLatin1Char('\0'));
    }

    const int limitMs = timeoutMs > 0 ? timeoutMs : 60000;
    const QRegularExpression rcRe(QRegularExpression::escape(token)
                                  + QStringLiteral("(\\d+)"));
    QElapsedTimer timer;
    timer.start();
    bool finished = false;
    while (timer.elapsed() < limitMs) {
        const QString text = tab->readTerminalText(80);
        const QRegularExpressionMatch match = rcRe.match(text);
        if (match.hasMatch()) {
            if (exitCode) {
                *exitCode = match.captured(1).toInt();
            }
            finished = true;
            break;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(150);
    }
    if (output) {
        *output = tab->readTerminalText(60);
    }
    if (timedOut) {
        *timedOut = !finished;
    }
    return true;
}

int MainWindow::openSessionTab(const QString &name)
{
    const QList<SessionConfig> sessions = m_sessionRepository->loadAllSessions();
    for (const SessionConfig &config : sessions) {
        if (config.name() == name || config.displayName() == name
            || config.host() == name || config.id() == name) {
            onSessionActivated(config);
            return m_tabWidget->count() - 1;
        }
    }
    return -1;
}

// PH2-07: Ctrl+Shift+P palette over menu actions, saved sessions and tabs.
void MainWindow::showCommandPalette()
{
    using PaletteItem = CommandPalette::Item;
    QList<PaletteItem> items;

    // Menu actions (recursive walk of the menu bar).
    const auto walkMenu = [this, &items](const QMenu *menu, auto &&recurse) -> void {
        for (QAction *action : menu->actions()) {
            if (action->menu()) {
                recurse(action->menu(), recurse);
                continue;
            }
            if (action->isSeparator() || action->text().isEmpty()) {
                continue;
            }
            PaletteItem item;
            item.id = QStringLiteral("menu:") + action->text();
            item.title = QString(action->text()).remove(QLatin1Char('&'));
            item.category = tr("Command");
            item.shortcut = action->shortcut().toString(QKeySequence::NativeText);
            QAction *target = action;
            item.action = [target]() { target->trigger(); };
            items.append(item);
        }
    };
    for (QAction *top : menuBar()->actions()) {
        if (top->menu()) {
            walkMenu(top->menu(), walkMenu);
        }
    }

    // Saved sessions.
    for (const SessionConfig &config : m_sessionRepository->loadAllSessions()) {
        PaletteItem item;
        item.id = QStringLiteral("session:") + config.id();
        item.title = config.displayName();
        item.category = tr("Session");
        const QString host = QStringLiteral("%1@%2:%3")
                                 .arg(config.username(), config.host())
                                 .arg(config.port());
        item.shortcut = host;
        item.action = [this, config]() { onSessionActivated(config); };
        items.append(item);
    }

    // Open tabs (jump).
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(i));
        if (!tab) {
            continue;
        }
        PaletteItem item;
        item.id = QStringLiteral("tab:") + QString::number(i);
        item.title = m_tabWidget->tabText(i);
        item.category = tr("Go to Tab");
        item.action = [this, i]() {
            if (i < m_tabWidget->count()) {
                m_tabWidget->setCurrentIndex(i);
            }
        };
        items.append(item);
    }

    CommandPalette palette(this);
    palette.setItems(items);
    // Center horizontally over the main window, near the top.
    palette.adjustSize();
    const QPoint topLeft(mapToGlobal(QPoint(width() / 2 - palette.width() / 2,
                                             height() / 4)));
    palette.move(topLeft);
    palette.exec();
}

bool MainWindow::reconnectTab(int index)
{
    if (index < 0 || index >= m_tabWidget->count()) {
        return false;
    }
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    if (!tab || tab->config().sessionType() != SessionType::Ssh) {
        return false;
    }
    // Guard: reconnecting a LIVE tab would kill its foreground program.
    if (tab->sshSession() && tab->sshSession()->isConnected()) {
        return false;
    }
    tab->reconnectSession();
    return true;
}

// B5-1: shared tab -> PortForwardManager resolution for the agent API.
static PortForwardManager *forwardManagerForTab(const QTabWidget *tabs, int index,
                                                QString *errorMessage)
{
    if (index < 0 || index >= tabs->count()) {
        if (errorMessage) *errorMessage = QStringLiteral("unknown_tab");
        return nullptr;
    }
    auto *tab = qobject_cast<SessionTab *>(tabs->widget(index));
    SshSession *ssh = tab ? tab->sshSession() : nullptr;
    PortForwardManager *manager = ssh ? ssh->portForwardManager() : nullptr;
    if (!manager) {
        if (errorMessage) *errorMessage = QStringLiteral("not_an_ssh_tab");
    }
    return manager;
}

bool MainWindow::addForwardToTab(int index, const QVariantMap &spec, QString *errorMessage)
{
    const auto fail = [errorMessage](const QString &what) {
        if (errorMessage) {
            *errorMessage = what;
        }
        return false;
    };

    QString resolveError;
    PortForwardManager *manager = forwardManagerForTab(m_tabWidget, index, &resolveError);
    if (!manager) {
        return fail(resolveError);
    }

    ForwardSpec forward;
    const QString type = spec.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("remote")) {
        forward.type = ForwardSpec::Type::Remote;
    } else if (type == QLatin1String("dynamic")) {
        forward.type = ForwardSpec::Type::Dynamic;
    } else if (type.isEmpty() || type == QLatin1String("local")) {
        forward.type = ForwardSpec::Type::Local;
    } else {
        return fail(QStringLiteral("type must be local, remote or dynamic"));
    }

    forward.name = spec.value(QStringLiteral("name")).toString();
    QString bindAddress = spec.value(QStringLiteral("bindAddress")).toString();
    if (bindAddress.isEmpty()) {
        bindAddress = QStringLiteral("127.0.0.1");
    }
    forward.bindAddress = bindAddress;
    const int bindPort = spec.value(QStringLiteral("bindPort")).toInt();
    if (bindPort < 1 || bindPort > 65535) {
        return fail(QStringLiteral("bindPort must be 1-65535"));
    }
    forward.bindPort = static_cast<quint16>(bindPort);

    if (forward.type != ForwardSpec::Type::Dynamic) {
        forward.targetHost = spec.value(QStringLiteral("targetHost")).toString();
        const int targetPort = spec.value(QStringLiteral("targetPort")).toInt();
        if (forward.targetHost.isEmpty() || targetPort < 1 || targetPort > 65535) {
            return fail(QStringLiteral("targetHost and targetPort (1-65535) are required "
                                       "for local/remote forwards"));
        }
        forward.targetPort = static_cast<quint16>(targetPort);
    }

    return manager->addForward(forward, errorMessage);
}

QVariantList MainWindow::listForwardsForTab(int index) const
{
    QString resolveError;
    PortForwardManager *manager = forwardManagerForTab(m_tabWidget, index, &resolveError);
    if (!manager) {
        return {};
    }

    static const auto typeText = [](ForwardSpec::Type type) {
        switch (type) {
        case ForwardSpec::Type::Remote: return QStringLiteral("remote");
        case ForwardSpec::Type::Dynamic: return QStringLiteral("dynamic");
        case ForwardSpec::Type::Local: break;
        }
        return QStringLiteral("local");
    };

    QVariantList result;
    const QList<ForwardSpec> forwards = manager->forwards();
    for (int i = 0; i < forwards.size(); ++i) {
        const ForwardSpec &f = forwards.at(i);
        QVariantMap entry;
        entry[QStringLiteral("index")] = i;
        entry[QStringLiteral("type")] = typeText(f.type);
        entry[QStringLiteral("bindAddress")] = f.bindAddress;
        entry[QStringLiteral("bindPort")] = f.bindPort;
        entry[QStringLiteral("target")] = f.type == ForwardSpec::Type::Dynamic
            ? QString()
            : QStringLiteral("%1:%2").arg(f.targetHost).arg(f.targetPort);
        entry[QStringLiteral("active")] = manager->isActive(i);
        entry[QStringLiteral("status")] = manager->statusText(i);
        result.append(entry);
    }
    return result;
}

bool MainWindow::removeForwardFromTab(int index, int forwardIndex, QString *errorMessage)
{
    QString resolveError;
    PortForwardManager *manager = forwardManagerForTab(m_tabWidget, index, &resolveError);
    if (!manager) {
        if (errorMessage) *errorMessage = resolveError;
        return false;
    }
    if (forwardIndex < 0 || forwardIndex >= manager->count()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("forward index out of range");
        }
        return false;
    }
    manager->removeForward(forwardIndex);
    return true;
}

// B5-2: AgentPolicy consent dialog — the same show()/finished/30 s-timer
// pattern as sudoAsync (never a nested event loop: the HTTP request stays
// pending while the dialog is up, and the GUI keeps serving).
void MainWindow::confirmPolicyAsync(int index, const QString &operation,
                                    const QString &detail,
                                    const AgentTabsInterface::PolicyCallback &cb)
{
    using Answer = AgentTabsInterface::PolicyAnswer;

    if (index < 0 || index >= m_tabWidget->count()) {
        Answer a;
        a.reason = QStringLiteral("unknown_tab");
        cb(a);
        return;
    }
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    if (!tab) {
        Answer a;
        a.reason = QStringLiteral("unknown_tab");
        cb(a);
        return;
    }

    if (isMinimized()) {
        showNormal();
    }
    raise();
    activateWindow();
    QApplication::alert(this);

    const QString identity = AgentSudoAuth::identityFor(tab->config());
    auto *box = new QMessageBox(this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setWindowTitle(tr("Agent Request"));
    box->setIcon(QMessageBox::Question);
    box->setText(tr("An AI agent requests %1 on \"%2\":")
                     .arg(operation, m_tabWidget->tabText(index)));
    box->setInformativeText(tr("Allow it? \"Always\" remembers this host in "
                               "agent/policyRules; \"This session\" lasts until "
                               "the app closes. Auto-closes in 30 seconds."));
    box->setDetailedText(detail);
    QPushButton *onceButton = box->addButton(tr("This session"), QMessageBox::YesRole);
    QPushButton *alwaysButton = nullptr;
    if (!identity.isEmpty()) {
        alwaysButton = box->addButton(tr("Always"), QMessageBox::AcceptRole);
    }
    box->addButton(QMessageBox::No);
    box->setDefaultButton(QMessageBox::No);
    box->setWindowFlags(box->windowFlags() | Qt::WindowStaysOnTopHint);

    QPointer<QMessageBox> guard(box);
    auto *autoReject = new QTimer(box);
    autoReject->setSingleShot(true);
    connect(autoReject, &QTimer::timeout, box, [guard]() {
        if (guard) {
            guard->setProperty("hsshTimedOut", true);
            guard->reject();
        }
    });
    connect(box, &QMessageBox::finished, this, [guard, onceButton, alwaysButton, cb](int) {
        if (!guard) {
            return;
        }
        QAbstractButton *clicked = guard->clickedButton();
        Answer a;
        if (clicked == onceButton) {
            a.allowed = true;
        } else if (clicked == alwaysButton) {
            a.allowed = true;
            a.always = true;
        } else {
            a.reason = guard->property("hsshTimedOut").toBool()
                           ? QStringLiteral("timeout")
                           : QStringLiteral("user_rejected");
        }
        cb(a);
    });
    autoReject->start(30000);
    box->show();
}

bool MainWindow::closeTab(int index)
{
    if (index < 0 || index >= m_tabWidget->count()) {
        return false;
    }
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    if (!tab) {
        return false;
    }
    tab->disconnectSession();
    m_tabWidget->removeTab(index);
    InputBroadcaster::instance().unregisterTab(tab);
    m_syncInputTabs.removeAll(tab);
    tab->deleteLater();
    updateStatusBarInfo();
    return true;
}

void MainWindow::updateRecentMenu()
{
    if (!m_recentMenu) {
        return;
    }
    m_recentMenu->clear();
    const QList<SessionConfig> recent = m_sessionRepository->recentSessions();
    for (const SessionConfig &config : recent) {
        m_recentMenu->addAction(config.displayName(), this, [this, config]() {
            onSessionActivated(config);
        });
    }
    if (recent.isEmpty()) {
        QAction *emptyAction = m_recentMenu->addAction(tr("(no recent sessions)"));
        emptyAction->setEnabled(false);
    }
}

void MainWindow::onOpenSettings()
{
    SettingsDialog dialog(this);
    dialog.exec();
}

void MainWindow::onSendCommand()
{
    bool ok = false;
    const QString command = QInputDialog::getMultiLineText(
        this, tr("Send Command"),
        tr("Send this command to every open terminal tab:"), QString(), &ok);
    if (!ok || command.isEmpty()) {
        return;
    }

    const int sent = InputBroadcaster::instance().broadcast(
        InputBroadcaster::instance().tabs(), command, true);
    statusBar()->showMessage(tr("Command sent to %1 terminal(s).").arg(sent), 5000);
}

QVariantList MainWindow::collectOpenTabs() const
{
    QVariantList result;
    if (!m_tabWidget) {
        return result;
    }
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(i));
        if (!tab) {
            continue;
        }
        QVariantMap entry;
        if (tab->config().sessionType() == SessionType::Ssh
            || tab->config().sessionType() == SessionType::Serial) {
            entry[QStringLiteral("type")] =
                tab->config().sessionType() == SessionType::Serial
                    ? QStringLiteral("serial")
                    : QStringLiteral("ssh");
            entry[QStringLiteral("id")] = tab->config().id();
        } else {
            entry[QStringLiteral("type")] = QStringLiteral("local");
            entry[QStringLiteral("shell")] = tab->config().shellType();
        }
        // PH1-08: alias / color / pin survive restarts.
        entry[QStringLiteral("title")] = m_tabWidget->tabText(i);
        if (tab->property("pinned").toBool()) {
            entry[QStringLiteral("pinned")] = true;
        }
        const QColor color = tab->property("tabColor").value<QColor>();
        if (color.isValid()) {
            entry[QStringLiteral("color")] = color.name();
        }
        result.append(entry);
    }
    return result;
}

void MainWindow::restorePreviousTabs()
{
    // Test instances (--no-restore) skip both the prompt and the restore —
    // they must come up headless-dialog-free for automated smoke runs.
    if (m_skipTabRestore
        || !Config::instance().boolValue(QStringLiteral("session/restoreTabs"), true)) {
        return;
    }
    const QVariantList saved = Config::instance().value(QStringLiteral("ui/openTabs")).toList();
    if (saved.isEmpty()) {
        return;
    }

    const int result = QMessageBox::question(
        this, tr("Restore Session"),
        tr("Restore the %1 terminal tab(s) that were open last time?").arg(saved.size()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (result != QMessageBox::Yes) {
        return;
    }

    // Index saved sessions by id for quick lookup.
    QHash<QString, SessionConfig> byId;
    for (const SessionConfig &config : m_sessionRepository->loadAllSessions()) {
        byId.insert(config.id(), config);
    }

    for (const QVariant &entryVariant : saved) {
        const QVariantMap entry = entryVariant.toMap();
        const QString type = entry.value(QStringLiteral("type")).toString();
        int index = -1;
        if (type == QLatin1String("ssh") || type == QLatin1String("serial")) {
            const QString id = entry.value(QStringLiteral("id")).toString();
            if (byId.contains(id)) {
                onSessionActivated(byId.value(id));
                index = m_tabWidget->count() - 1;
            }
        } else if (type == QLatin1String("local")) {
            onNewLocalTerminal(entry.value(QStringLiteral("shell")).toString());
            index = m_tabWidget->count() - 1;
        }
        if (index < 0) {
            continue;
        }
        // PH1-08: restore alias / color / pin.
        const QString title = entry.value(QStringLiteral("title")).toString();
        if (!title.isEmpty()) {
            m_tabWidget->setTabText(index, title);
        }
        const QString colorName = entry.value(QStringLiteral("color")).toString();
        if (!colorName.isEmpty()) {
            applyTabColor(index, QColor(colorName));
        }
        if (entry.value(QStringLiteral("pinned")).toBool()) {
            setTabPinned(index, true);
        }
    }
}

void MainWindow::applyFocusMode(bool enabled)
{
    m_focusMode = enabled;
    m_sessionDock->setVisible(!enabled);
    m_fileDock->setVisible(!enabled);
    m_transfersDock->setVisible(!enabled);
    for (QToolBar *toolBar : findChildren<QToolBar *>()) {
        toolBar->setVisible(!enabled);
    }
    // Keep the menu bar visible: hiding it would disable the F11 shortcut.
    statusBar()->setVisible(!enabled);
}

void MainWindow::onFocusMode()
{
    applyFocusMode(!m_focusMode);
}

void MainWindow::onSetLockPassword()
{
    bool ok = false;
    const QString password = QInputDialog::getText(this, tr("Set Lock Password"),
                                                    tr("Password to unlock the screen:"),
                                                    QLineEdit::Password, QString(), &ok);
    if (!ok) {
        return;
    }
    if (password.isEmpty()) {
        QMessageBox::warning(this, tr("Set Lock Password"), tr("Password must not be empty."));
        return;
    }
    Config::instance().setValue(QStringLiteral("lock/password"), password);
    Config::instance().sync();
    statusBar()->showMessage(tr("Lock password set."), 5000);
}

void MainWindow::onLockScreen()
{
    const QString password = Config::instance().stringValue(QStringLiteral("lock/password"));
    if (password.isEmpty()) {
        QMessageBox::information(this, tr("Lock Screen"),
                                 tr("Set a lock password first (Tools > Set Lock Password...)."));
        return;
    }

    QDialog dialog(nullptr);
    dialog.setWindowTitle(tr("hssh Locked"));
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setFixedSize(420, 220);

    auto *layout = new QVBoxLayout(&dialog);
    auto *title = new QLabel(tr("<h2>Screen locked</h2>"), &dialog);
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    auto *passwordEdit = new QLineEdit(&dialog);
    passwordEdit->setEchoMode(QLineEdit::Password);
    passwordEdit->setPlaceholderText(tr("Enter lock password"));
    layout->addWidget(passwordEdit);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    const auto tryUnlock = [&dialog, passwordEdit, password]() {
        if (passwordEdit->text() == password) {
            dialog.accept();
        } else {
            passwordEdit->clear();
            passwordEdit->setPlaceholderText(tr("Wrong password"));
        }
    };
    connect(buttons, &QDialogButtonBox::accepted, &dialog, tryUnlock);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    connect(passwordEdit, &QLineEdit::returnPressed, &dialog, tryUnlock);

    hide();
    dialog.exec();
    show();
}

// Key rotation helper: decrypt every session with the currently active key
// (must be unlocked) so the caller can switch keys and re-save.
// Returns an empty list + shows a warning when decryption fails.
static QList<SessionConfig> decryptAllOrWarn(QWidget *parent, SessionRepository *repo)
{
    const QList<SessionConfig> sessions = repo->loadAllSessions();
    for (const SessionConfig &config : sessions) {
        if (config.id().isEmpty()) {
            QMessageBox::warning(parent, MainWindow::tr("Master Password"),
                                 MainWindow::tr("Stored secrets are locked or corrupted."));
            return {};
        }
    }
    return sessions;
}

void MainWindow::onSetMasterPassword()
{
    bool ok = false;
    const QString password = QInputDialog::getText(this, tr("Set Master Password"),
                                                    tr("New master password:"),
                                                    QLineEdit::Password, QString(), &ok);
    if (!ok || password.isEmpty()) {
        return;
    }
    const QString confirm = QInputDialog::getText(this, tr("Set Master Password"),
                                                   tr("Confirm master password:"),
                                                   QLineEdit::Password, QString(), &ok);
    if (!ok) {
        return;
    }
    if (confirm != password) {
        QMessageBox::warning(this, tr("Set Master Password"), tr("Passwords do not match."));
        return;
    }

    const QList<SessionConfig> sessions = decryptAllOrWarn(this, m_sessionRepository);
    if (!Crypto::setupMasterPassword(password)) {
        QMessageBox::warning(this, tr("Set Master Password"), tr("Failed to set the master password."));
        return;
    }
    for (const SessionConfig &config : sessions) {
        m_sessionRepository->saveSession(config);
    }
    statusBar()->showMessage(tr("Master password enabled. Secrets are now encrypted with it."), 5000);
}

void MainWindow::onClearMasterPassword()
{
    if (!Crypto::usesMasterPassword()) {
        statusBar()->showMessage(tr("Master password is not enabled."), 3000);
        return;
    }
    const int result = QMessageBox::question(
        this, tr("Disable Master Password"),
        tr("Disable the master password? Secrets will be re-encrypted with this machine's key instead."),
        QMessageBox::Yes | QMessageBox::No);
    if (result != QMessageBox::Yes) {
        return;
    }

    const QList<SessionConfig> sessions = decryptAllOrWarn(this, m_sessionRepository);
    Crypto::clearMasterPassword();
    for (const SessionConfig &config : sessions) {
        m_sessionRepository->saveSession(config);
    }
    statusBar()->showMessage(tr("Master password disabled."), 5000);
}

QModelIndex MainWindow::selectedSessionIndex() const
{
    const QModelIndex index = m_sessionManager->currentIndex();
    if (!index.isValid()) {
        return {};
    }
    if (m_sessionModel->nodeType(index) == SessionModel::NodeType::Session) {
        return index;
    }
    return {};
}

QModelIndex MainWindow::selectedFolderIndex() const
{
    const QModelIndex index = m_sessionManager->currentIndex();
    if (!index.isValid()) {
        return {};
    }
    if (m_sessionModel->nodeType(index) == SessionModel::NodeType::Folder) {
        return index;
    }
    return {};
}

} // namespace hssh

#include "MainWindow.h"

#include "agent/AgentHttpServer.h"
#include "app/SessionTab.h"
#include "app/dialogs/EditFolderDialog.h"
#include "app/dialogs/NewSessionDialog.h"
#include "app/dialogs/PortForwardDialog.h"
#include "app/widgets/FileCompareWidget.h"
#include "app/widgets/LocalFileWidget.h"
#include "app/widgets/SessionManagerWidget.h"
#include "app/widgets/SftpWidget.h"
#include "app/widgets/TransfersWidget.h"
#include "core/SessionRepository.h"
#include "core/SshSession.h"
#include "hssh/Version.h"
#include "terminal/LocalShellProcess.h"
#include "utils/Config.h"
#include "utils/Crypto.h"

#include <QApplication>
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
#include <QRegularExpression>
#include <QStatusBar>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

namespace hssh {

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_sessionRepository(new SessionRepository(this))
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
    fileMenu->addAction(tr("&Export Sessions..."), this, &MainWindow::onExportSessions);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    // View menu
    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(m_sessionDock->toggleViewAction());
    viewMenu->addAction(m_fileDock->toggleViewAction());
    viewMenu->addAction(m_transfersDock->toggleViewAction());
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
    toolsMenu->addAction(tr("&Settings"));
    toolsMenu->addAction(tr("&Key Manager"));
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
    toolBar->addAction(tr("Settings"), this, []() {
        // TODO: implement settings dialog
    });
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
    m_transfersDock->setAllowedAreas(Qt::BottomDockWidgetArea | Qt::TopDockWidgetArea);
    m_transfersDock->setWidget(m_transfers);
    addDockWidget(Qt::BottomDockWidgetArea, m_transfersDock);
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

    connect(m_tabWidget, &QTabWidget::tabCloseRequested, this, [this](int index) {
        QWidget *widget = m_tabWidget->widget(index);
        if (auto *tab = qobject_cast<SessionTab *>(widget)) {
            tab->disconnectSession();
        }
        m_tabWidget->removeTab(index);
        // removeTab only detaches the widget; delete it so the shell process
        // object (and its ConPTY/session resources) is actually destroyed.
        if (widget) {
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
    connect(tab, &SessionTab::sizeChanged, this, [this, tab](int columns, int rows) {
        if (m_tabWidget->currentWidget() == tab) {
            m_termSizeLabel->setText(tr("%1×%2").arg(columns).arg(rows));
        }
    });
    const QString title = tab->config().displayName();
    const int index = m_tabWidget->addTab(tab, title);
    m_tabWidget->setCurrentIndex(index);
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

void MainWindow::onRemoveSession(const QModelIndex &index)
{
    if (!index.isValid()) {
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
    connect(tab, &SessionTab::sizeChanged, this, [this, tab](int columns, int rows) {
        if (m_tabWidget->currentWidget() == tab) {
            m_termSizeLabel->setText(tr("%1×%2").arg(columns).arg(rows));
        }
    });
    const int index = m_tabWidget->addTab(tab, config.displayName());
    m_tabWidget->setCurrentIndex(index);
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
    if (!m_agentServer->start()) {
        QMessageBox::warning(this, tr("Agent"),
                             tr("Failed to start agent: %1").arg(m_agentServer->errorString()));
        m_agentServer->deleteLater();
        m_agentServer = nullptr;
        return;
    }
    m_agentAction->setText(tr("Stop Agent"));
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
        entry[QStringLiteral("connected")] = tab->sshSession()
                                                 ? tab->sshSession()->isConnected()
                                                 : true;
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

bool MainWindow::confirmSudo(int index, const QString &command)
{
    if (index < 0 || index >= m_tabWidget->count()) {
        return false;
    }
    auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(index));
    if (!tab) {
        return false;
    }

    if (m_sudoApproved.contains(tab)) {
        return true; // Already approved within this tab's lifetime.
    }

    QMessageBox box(this);
    box.setWindowTitle(tr("Sudo Confirmation"));
    box.setIcon(QMessageBox::Warning);
    box.setText(tr("An AI agent requests to run this command with sudo in session \"%1\":")
                    .arg(m_tabWidget->tabText(index)));
    box.setInformativeText(tr("Allow it? You will not be asked again while this tab stays open. "
                               "This dialog closes automatically in 30 seconds."));
    box.setDetailedText(command);
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.setDefaultButton(QMessageBox::No);
    box.setWindowFlags(box.windowFlags() | Qt::WindowStaysOnTopHint);

    // Make sure the request is noticed: bring the window forward first.
    raise();
    activateWindow();

    // Unanswered dialogs must not hang the agent request forever.
    QTimer autoReject;
    autoReject.setSingleShot(true);
    autoReject.setInterval(30000);
    connect(&autoReject, &QTimer::timeout, &box, &QMessageBox::reject);
    autoReject.start();

    if (box.exec() != QMessageBox::Yes) {
        return false;
    }
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

bool MainWindow::waitForCompletion(SessionTab *tab, int timeoutMs)
{
    // Completion heuristic: output stops changing for a quiet period, or
    // the last line looks like a shell prompt.
    QElapsedTimer timer;
    timer.start();
    QString lastSnapshot;
    qint64 lastChangeAt = timer.elapsed();
    static const QRegularExpression promptRe(QStringLiteral("[$#]\\s*$"));
    while (timer.elapsed() < timeoutMs) {
        const QString snapshot = tab->readTerminalText(40);
        if (snapshot != lastSnapshot) {
            lastSnapshot = snapshot;
            lastChangeAt = timer.elapsed();
        }
        const QStringList lines = snapshot.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (!lines.isEmpty() && promptRe.match(lines.last()).hasMatch()) {
            return true;
        }
        if (timer.elapsed() - lastChangeAt > 2000) {
            return true; // Quiet for 2s: assume the command finished.
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(150);
    }
    return false;
}

bool MainWindow::sudoExec(int index, const QString &command, const QString &secret,
                          bool useStoredCredential, int timeoutMs,
                          QString *output, bool *timedOut, QString *errorMessage)
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

    tab->runCommand(QStringLiteral("sudo ") + command + QLatin1Char('\n'));

    // Wait briefly for a password prompt; no prompt means cached sudo.
    const QStringList passwordPatterns = {
        QStringLiteral("[sudo] password"),
        QStringLiteral("password for"),
        QStringLiteral("password:"),
    };
    const bool needsPassword = waitForPromptPattern(tab, passwordPatterns, 5000);

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

    const bool finished = waitForCompletion(tab, timeoutMs > 0 ? timeoutMs : 60000);
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

void MainWindow::onSendCommand()
{
    bool ok = false;
    const QString command = QInputDialog::getMultiLineText(
        this, tr("Send Command"),
        tr("Send this command to every open terminal tab:"), QString(), &ok);
    if (!ok || command.isEmpty()) {
        return;
    }

    int sent = 0;
    const QString payload = command + QLatin1Char('\n');
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        if (auto *tab = qobject_cast<SessionTab *>(m_tabWidget->widget(i))) {
            tab->runCommand(payload);
            ++sent;
        }
    }
    statusBar()->showMessage(tr("Command sent to %1 terminal(s).").arg(sent), 5000);
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

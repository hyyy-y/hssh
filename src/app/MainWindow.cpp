#include "MainWindow.h"

#include "app/SessionTab.h"
#include "app/dialogs/EditFolderDialog.h"
#include "app/dialogs/NewSessionDialog.h"
#include "app/widgets/FileCompareWidget.h"
#include "app/widgets/LocalFileWidget.h"
#include "app/widgets/SessionManagerWidget.h"
#include "app/widgets/SftpWidget.h"
#include "app/widgets/TransfersWidget.h"
#include "core/SessionRepository.h"
#include "hssh/Version.h"
#include "terminal/LocalShellProcess.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTabWidget>
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

    // Tools menu
    QMenu *toolsMenu = menuBar()->addMenu(tr("&Tools"));
    toolsMenu->addAction(tr("&Settings"));
    toolsMenu->addAction(tr("&Key Manager"));
    toolsMenu->addSeparator();
    toolsMenu->addAction(tr("Start Agent"));
    toolsMenu->addAction(tr("Agent Settings"));

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
    m_sessionModel->loadSessions(data.sessions, data.folderPaths);

    // Update folder display names from database
    for (auto it = data.folderNames.cbegin(); it != data.folderNames.cend(); ++it) {
        // TODO: map folder id to model index and rename
        Q_UNUSED(it)
    }
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
    Q_UNUSED(index)
    // TODO: implement folder rename with repository mapping
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

    m_sessionModel->removeNode(index);
}

void MainWindow::onSessionActivated(const SessionConfig &config)
{
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
    if (tab) {
        tab->disconnectSession();
    }
    m_tabWidget->removeTab(index);
    if (tab) {
        tab->deleteLater();
    }
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

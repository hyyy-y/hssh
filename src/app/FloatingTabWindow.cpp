#include "FloatingTabWindow.h"

#include "SessionTab.h"

#include <QCloseEvent>
#include <QMessageBox>
#include <QToolBar>

namespace hssh {

FloatingTabWindow::FloatingTabWindow(SessionTab *tab, const QString &title, const QIcon &icon,
                                     QWidget *parent)
    : QMainWindow(parent)
    , m_tab(tab)
    , m_title(title)
    , m_windowIcon(icon)
{
    setWindowTitle(title);
    if (!icon.isNull()) {
        setWindowIcon(icon);
    }
    setAttribute(Qt::WA_DeleteOnClose, false); // closeEvent routes to the owner

    // Taking the tab as central widget reparents it; the SessionTab keeps
    // its connection, terminal and sync/pin state.
    setCentralWidget(tab);

    auto *toolBar = new QToolBar(this);
    toolBar->setMovable(false);
    toolBar->setIconSize(QSize(16, 16));
    toolBar->addAction(tr("Dock to Main Window"), this, [this]() {
        emit dockRequested(this);
    });
    addToolBar(Qt::TopToolBarArea, toolBar);

    resize(900, 620);
}

void FloatingTabWindow::closeEvent(QCloseEvent *event)
{
    if (m_tab && m_tab->property("pinned").toBool()) {
        QMessageBox::information(this, tr("Pinned Tab"),
                                 tr("This tab is pinned — unpin it before closing."));
        event->ignore();
        return;
    }
    event->accept();
    emit closeRequested(this);
}

} // namespace hssh

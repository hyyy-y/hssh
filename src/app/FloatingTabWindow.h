#ifndef HSSH_APP_FLOATINGTABWINDOW_H
#define HSSH_APP_FLOATINGTABWINDOW_H

#include <QIcon>
#include <QMainWindow>

namespace hssh {

class SessionTab;

// PH1-08: a tab dragged out of the main window lives in its own top-level
// window. The SessionTab object (and with it the live connection, sync-input
// membership and pin state) simply changes parent. Closing the window closes
// the session; the toolbar action docks the tab back into the main window.
class FloatingTabWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit FloatingTabWindow(SessionTab *tab, const QString &title, const QIcon &icon,
                               QWidget *parent = nullptr);

    [[nodiscard]] SessionTab *tab() const { return m_tab; }
    [[nodiscard]] QIcon tabIcon() const { return m_windowIcon; }
    // The tab caption given at detach time; used when docking back so a
    // mutated window title cannot leak into the tab label.
    [[nodiscard]] QString tabTitle() const { return m_title; }

signals:
    void dockRequested(FloatingTabWindow *window);
    void closeRequested(FloatingTabWindow *window);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    SessionTab *m_tab = nullptr;
    QString m_title;
    QIcon m_windowIcon;
};

} // namespace hssh

#endif // HSSH_APP_FLOATINGTABWINDOW_H

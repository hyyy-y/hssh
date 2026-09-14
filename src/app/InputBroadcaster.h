#ifndef HSSH_APP_INPUTBROADCASTER_H
#define HSSH_APP_INPUTBROADCASTER_H

#include <QObject>
#include <QPointer>
#include <QString>
#include <QList>

namespace hssh {

class SessionTab;

// Registry of open terminal tabs plus a broadcast primitive. Command sender,
// sync input, free-type mode and scripts all route injection through this
// class so targeting rules stay in one place.
class InputBroadcaster : public QObject {
    Q_OBJECT

public:
    static InputBroadcaster &instance();

    void registerTab(SessionTab *tab);
    void unregisterTab(SessionTab *tab);

    [[nodiscard]] QList<SessionTab *> tabs() const;

    // Send `text` to every tab in targets. Returns the number of tabs that
    // accepted the input. When appendNewline is true, a trailing newline is
    // added if missing (command semantics); otherwise the bytes go verbatim.
    int broadcast(const QList<SessionTab *> &targets, const QString &text,
                  bool appendNewline = true);

signals:
    void tabsChanged();

private:
    InputBroadcaster() = default;

    QList<QPointer<SessionTab>> m_tabs;
};

} // namespace hssh

#endif // HSSH_APP_INPUTBROADCASTER_H

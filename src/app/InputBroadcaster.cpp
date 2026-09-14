#include "InputBroadcaster.h"

#include "app/SessionTab.h"

namespace hssh {

InputBroadcaster &InputBroadcaster::instance()
{
    static InputBroadcaster broadcaster;
    return broadcaster;
}

void InputBroadcaster::registerTab(SessionTab *tab)
{
    if (!tab || m_tabs.contains(tab)) {
        return;
    }
    m_tabs.append(tab);
    emit tabsChanged();
}

void InputBroadcaster::unregisterTab(SessionTab *tab)
{
    const int removed = m_tabs.removeAll(tab);
    if (removed > 0) {
        emit tabsChanged();
    }
}

QList<SessionTab *> InputBroadcaster::tabs() const
{
    QList<SessionTab *> result;
    result.reserve(m_tabs.size());
    for (const QPointer<SessionTab> &tab : m_tabs) {
        if (tab) {
            result.append(tab);
        }
    }
    return result;
}

int InputBroadcaster::broadcast(const QList<SessionTab *> &targets, const QString &text,
                                bool appendNewline)
{
    if (text.isEmpty()) {
        return 0;
    }
    QString payload = text;
    if (appendNewline && !payload.endsWith(QLatin1Char('\n'))) {
        payload += QLatin1Char('\n');
    }
    int sent = 0;
    for (SessionTab *tab : targets) {
        if (tab) {
            tab->runCommand(payload);
            ++sent;
        }
    }
    return sent;
}

} // namespace hssh

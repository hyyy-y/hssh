#ifndef HSSH_AGENT_AGENTWEBSOCKETSERVER_H
#define HSSH_AGENT_AGENTWEBSOCKETSERVER_H

// PH2-14 / B5-4: agent WebSocket endpoint (/api/v1/ws semantics over a
// dedicated port, one above the REST agent port). The whole feature is
// guarded by HSSH_HAS_WEBSOCKETS — Qt6 WebSockets is an optional SDK module
// and builds without it must stay clean. NB: on machines without the module
// this code is NEVER compiled — treat it as untested until a module-equipped
// build runs (the build prints "agent WebSocket endpoint available" when on).
#ifdef HSSH_HAS_WEBSOCKETS

#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>

class QWebSocket;
class QWebSocketServer;

namespace hssh {

class AgentTabsInterface;

// Frame protocol (text frames, JSON):
//   {"type":"ping"}                          -> {"type":"pong"}
//   {"type":"shell-input","ref":"...","data":"..."} -> raw input to the tab
//                                              -> {"type":"ok"} / {"type":"error",...}
//   {"type":"read","ref":"...","lines":200}  -> {"type":"shell-output","ref":...,"text":...}
//   {"type":"subscribe","ref":"..."}         -> periodic shell-output pushes (200 ms)
//   {"type":"unsubscribe","ref":"..."}       -> stop pushing
// Everything addresses VISIBLE GUI tabs by the same drift-safe refs as the
// REST API; no headless sessions are created.
class AgentWebSocketServer : public QObject {
public:
    explicit AgentWebSocketServer(AgentTabsInterface *tabs, QObject *parent = nullptr);
    ~AgentWebSocketServer() override;

    // Starts listening; returns the actual port (0 on failure).
    quint16 start(quint16 port);

private:
    void handleFrame(QWebSocket *client, const QString &frame);
    int resolveRef(const QString &ref) const;
    void pumpSubscription(QWebSocket *client, const QString &ref);

    AgentTabsInterface *m_tabs = nullptr;
    QWebSocketServer *m_server = nullptr;
    // client -> {ref -> cursor (lines already sent)}
    QHash<QWebSocket *, QHash<QString, int>> m_cursors;
    QTimer m_pump; // drives all subscriptions from one timer
};

} // namespace hssh

#endif // HSSH_HAS_WEBSOCKETS
#endif // HSSH_AGENT_AGENTWEBSOCKETSERVER_H

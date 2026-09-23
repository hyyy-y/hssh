#include "AgentWebSocketServer.h"

#ifdef HSSH_HAS_WEBSOCKETS

#include "AgentHttpServer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

namespace hssh {

AgentWebSocketServer::AgentWebSocketServer(AgentTabsInterface *tabs, QObject *parent)
    : QObject(parent)
    , m_tabs(tabs)
    , m_server(new QWebSocketServer(QStringLiteral("hssh-agent"), QWebSocketServer::NonSecureMode,
                                    this))
{
    connect(m_server, &QWebSocketServer::newConnection, this, [this]() {
        while (m_server->hasPendingConnections()) {
            QWebSocket *client = m_server->nextPendingConnection();
            connect(client, &QWebSocket::textMessageReceived, this,
                    [this, client](const QString &frame) { handleFrame(client, frame); });
            connect(client, &QWebSocket::disconnected, this, [this, client]() {
                m_cursors.remove(client);
                client->deleteLater();
            });
        }
    });

    m_pump.setInterval(200);
    connect(&m_pump, &QTimer::timeout, this, [this]() {
        for (auto it = m_cursors.begin(); it != m_cursors.end(); ++it) {
            if (it.key()->isValid()) {
                const QStringList refs = it.value().keys();
                for (const QString &ref : refs) {
                    pumpSubscription(it.key(), ref);
                }
            }
        }
    });
}

AgentWebSocketServer::~AgentWebSocketServer()
{
    m_pump.stop();
    if (m_server->isListening()) {
        m_server->close();
    }
}

quint16 AgentWebSocketServer::start(quint16 port)
{
    if (!m_server->listen(QHostAddress::LocalHost, port)) {
        return 0;
    }
    m_pump.start();
    return m_server->serverPort();
}

// Same drift-safe resolution as the REST API: exact match on sessionName,
// host or title first; "<name>:<ordinal>" counts same-name tabs (1-based).
int AgentWebSocketServer::resolveRef(const QString &ref) const
{
    if (!m_tabs) {
        return -1;
    }
    const QVariantList tabs = m_tabs->listTabs();
    QString name = ref;
    int ordinal = 1;
    const int colon = ref.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        bool ok = false;
        ordinal = ref.mid(colon + 1).toInt(&ok);
        if (ok && ordinal > 0) {
            name = ref.left(colon);
        } else {
            ordinal = 1;
        }
    }
    int seen = 0;
    for (const QVariant &v : tabs) {
        const QVariantMap m = v.toMap();
        const QString sessionName = m.value(QStringLiteral("sessionName")).toString();
        const QString title = m.value(QStringLiteral("title")).toString();
        const QString host = m.value(QStringLiteral("host")).toString();
        if (sessionName == name || title == name || host == name) {
            ++seen;
            if (seen == ordinal) {
                return m.value(QStringLiteral("index")).toInt();
            }
        }
    }
    return -1;
}

void AgentWebSocketServer::handleFrame(QWebSocket *client, const QString &frame)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(frame.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        client->sendTextMessage(QStringLiteral("{\"type\":\"error\",\"message\":\"bad JSON\"}"));
        return;
    }
    const QJsonObject message = doc.object();
    const QString type = message.value(QStringLiteral("type")).toString();

    if (type == QLatin1String("ping")) {
        client->sendTextMessage(QStringLiteral("{\"type\":\"pong\"}"));
        return;
    }

    const QString ref = message.value(QStringLiteral("ref")).toString();
    if (ref.isEmpty()) {
        client->sendTextMessage(QStringLiteral("{\"type\":\"error\",\"message\":\"missing ref\"}"));
        return;
    }
    const int index = resolveRef(ref);
    if (index < 0) {
        client->sendTextMessage(QStringLiteral("{\"type\":\"error\",\"message\":\"unknown tab\"}"));
        return;
    }

    if (type == QLatin1String("shell-input")) {
        const QString data = message.value(QStringLiteral("data")).toString();
        if (m_tabs->sendInputToTab(index, data)) {
            client->sendTextMessage(QStringLiteral("{\"type\":\"ok\"}"));
        } else {
            client->sendTextMessage(QStringLiteral("{\"type\":\"error\",\"message\":\"send failed\"}"));
        }
        return;
    }

    if (type == QLatin1String("read")) {
        const int lines = message.value(QStringLiteral("lines")).toInt(200);
        QString text;
        if (m_tabs->readTab(index, lines, &text)) {
            QJsonObject out;
            out[QStringLiteral("type")] = QStringLiteral("shell-output");
            out[QStringLiteral("ref")] = ref;
            out[QStringLiteral("text")] = text;
            client->sendTextMessage(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
        } else {
            client->sendTextMessage(QStringLiteral("{\"type\":\"error\",\"message\":\"read failed\"}"));
        }
        return;
    }

    if (type == QLatin1String("subscribe")) {
        m_cursors[client].insert(ref, 0);
        if (!m_pump.isActive()) {
            m_pump.start();
        }
        pumpSubscription(client, ref); // immediate first frame
        return;
    }
    if (type == QLatin1String("unsubscribe")) {
        if (m_cursors.contains(client)) {
            m_cursors[client].remove(ref);
        }
        return;
    }

    client->sendTextMessage(QStringLiteral("{\"type\":\"error\",\"message\":\"unknown type\"}"));
}

void AgentWebSocketServer::pumpSubscription(QWebSocket *client, const QString &ref)
{
    const int index = resolveRef(ref);
    if (index < 0) {
        return; // tab closed: keep the subscription, resolve again later
    }
    const int from = m_cursors.value(client).value(ref, 0);
    QString text;
    if (!m_tabs->readTabRange(index, from, 0, &text) || text.isEmpty()) {
        return;
    }
    const int newLines = static_cast<int>(text.count(QLatin1Char('\n'))) + (text.endsWith(QLatin1Char('\n')) ? 0 : 1);
    m_cursors[client][ref] = from + newLines;
    QJsonObject out;
    out[QStringLiteral("type")] = QStringLiteral("shell-output");
    out[QStringLiteral("ref")] = ref;
    out[QStringLiteral("from")] = from;
    out[QStringLiteral("text")] = text;
    client->sendTextMessage(QString::fromUtf8(QJsonDocument(out).toJson(QJsonDocument::Compact)));
}

} // namespace hssh

#endif // HSSH_HAS_WEBSOCKETS

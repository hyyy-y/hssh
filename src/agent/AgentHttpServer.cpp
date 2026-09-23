#include "AgentHttpServer.h"

#include "agent/AgentAudit.h"
#include "core/ChannelCopySession.h"
#include "core/SftpSession.h"
#include "core/TransferSession.h"
#include "hssh/Version.h"
#include "utils/Config.h"
#include "utils/Crypto.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>
#include <QUrl>

namespace hssh {

struct HttpRequest {
    QByteArray method;
    QString path;
    QString query;
    QHash<QByteArray, QByteArray> headers;
    QByteArray body;
};

namespace {

// Display/lookup name of a tab entry: saved-session name, else host, else title.
QString tabNameOf(const QVariantMap &tab)
{
    QString name = tab.value(QStringLiteral("sessionName")).toString();
    if (name.isEmpty()) {
        name = tab.value(QStringLiteral("host")).toString();
    }
    if (name.isEmpty()) {
        name = tab.value(QStringLiteral("title")).toString();
    }
    return name;
}

// Adds "name", "ref" (name[:ordinal]) and "target" (user@host:port) to
// every entry of a listTabs() result, plus consistency flags:
//   "stale"        — the tab's sessionName resolves to a saved session whose
//                    host/port/username now DIFFERS from the tab's config
//                    (the session was edited after the tab opened; name-based
//                    routing would silently hit this tab — the 2026-09-11
//                    wrong-machine accident).
//   "peerMismatch" — the tab reports a live peer IP that differs from its
//                    configured host (only flagged when the configured host
//                    is an IP literal; hostname-vs-resolved-IP is normal).
// The ref is stable against cross-machine index drift: "board-a:2" always
// means the 2nd tab of board-a, never some other machine's tab.
void annotateTabRefs(QVariantList &tabs, const QVariantList &savedSessions)
{
    QHash<QString, int> totals;
    for (const QVariant &v : tabs) {
        ++totals[tabNameOf(v.toMap())];
    }
    QHash<QString, int> counts;
    for (QVariant &v : tabs) {
        QVariantMap m = v.toMap();
        const QString name = tabNameOf(m);
        const int ordinal = ++counts[name];
        m[QStringLiteral("name")] = name;
        m[QStringLiteral("ref")] = totals.value(name) > 1
            ? name + QLatin1Char(':') + QString::number(ordinal)
            : name;

        const bool isSsh = m.value(QStringLiteral("type")).toString() == QLatin1String("ssh");
        const QString host = m.value(QStringLiteral("host")).toString();
        const QString peer = m.value(QStringLiteral("peer")).toString();
        if (isSsh) {
            // The peer (live socket address) is the ground truth when known.
            const QString effective = peer.isEmpty() ? host : peer;
            m[QStringLiteral("target")] =
                QStringLiteral("%1@%2:%3")
                    .arg(m.value(QStringLiteral("username")).toString(),
                         effective,
                         m.value(QStringLiteral("port")).toString());
            // IP-literal host vs live peer: a real mismatch worth flagging.
            const QHostAddress hostAddr(host);
            if (!peer.isEmpty() && !hostAddr.isNull()
                && hostAddr != QHostAddress(peer)) {
                m[QStringLiteral("peerMismatch")] = true;
            }
            // Saved-session drift: same name, different endpoint now.
            const QString sessionName = m.value(QStringLiteral("sessionName")).toString();
            if (!sessionName.isEmpty()) {
                for (const QVariant &sv : savedSessions) {
                    const QVariantMap s = sv.toMap();
                    if (s.value(QStringLiteral("name")).toString() != sessionName
                        && s.value(QStringLiteral("displayName")).toString() != sessionName) {
                        continue;
                    }
                    const bool differs =
                        s.value(QStringLiteral("host")).toString() != host
                        || s.value(QStringLiteral("port")).toInt()
                               != m.value(QStringLiteral("port")).toInt()
                        || s.value(QStringLiteral("username")).toString()
                               != m.value(QStringLiteral("username")).toString();
                    if (differs) {
                        m[QStringLiteral("stale")] = true;
                        m[QStringLiteral("savedTarget")] =
                            QStringLiteral("%1@%2:%3")
                                .arg(s.value(QStringLiteral("username")).toString(),
                                     s.value(QStringLiteral("host")).toString(),
                                     s.value(QStringLiteral("port")).toString());
                    }
                    break;
                }
            }
        } else {
            m[QStringLiteral("target")] = QStringLiteral("local");
        }
        v = m;
    }
}

// Resolves a tab path segment to a positional index. A plain integer keeps
// the legacy meaning; anything else is "<name>" or "<name>:<ordinal>"
// matching sessionName/host/title (ordinal counts matches in tab order,
// 1-based; omitting it requires an unambiguous single match).
int resolveTabRef(const QVariantList &tabs, const QString &ref, QString *error)
{
    bool digitsOnly = !ref.isEmpty();
    for (const QChar c : ref) {
        if (!c.isDigit()) {
            digitsOnly = false;
            break;
        }
    }
    if (digitsOnly) {
        return ref.toInt(); // legacy positional index
    }
    const QString decoded = QString::fromUtf8(QByteArray::fromPercentEncoding(ref.toUtf8()));
    QString name = decoded;
    int ordinal = 1;
    const int colon = decoded.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        bool ok = false;
        const int n = decoded.mid(colon + 1).toInt(&ok);
        if (ok && n >= 1) {
            name = decoded.left(colon);
            ordinal = n;
        }
    }
    int matches = 0;
    int found = -1;
    for (const QVariant &v : tabs) {
        const QVariantMap m = v.toMap();
        const QStringList keys{tabNameOf(m), m.value(QStringLiteral("host")).toString(),
                               m.value(QStringLiteral("title")).toString()};
        if (!keys.contains(name)) {
            continue;
        }
        ++matches;
        if (matches == ordinal) {
            found = m.value(QStringLiteral("index")).toInt();
        }
    }
    if (found >= 0) {
        // Multiple matches with a bare name: first in tab order. Callers who
        // need a specific tab should use the explicit "<name>:<ordinal>" ref
        // advertised by GET /api/v1/tabs.
        return found;
    }
    if (error) {
        *error = matches > 0
            ? QStringLiteral("Tab ref '%1': only %2 tab(s) match '%3'")
                  .arg(decoded).arg(matches).arg(name)
            : QStringLiteral("Unknown tab: %1").arg(decoded);
    }
    return -1;
}

} // namespace

class AgentHttpServer::Impl {
public:
    // One in-flight async operation per request id; detail carries the
    // command/paths for the audit trail (never secrets).
    struct PendingOp {
        QTcpSocket *socket = nullptr;
        QString action;
        QString sessionId;
        QString detail;
        qint64 startedMs = 0; // health checks report latency
    };

    // A file transfer running for a GUI tab (via its own connection). The
    // worker is an SFTP, scp or base64-shell session; "auto" walks the
    // fallback chain (sftp -> scp -> shell) while zero bytes have moved.
    struct TabTransfer {
        TransferSession *worker = nullptr;
        int tabIndex = -1;
        SessionConfig config;
        bool isUpload = false;
        QString localPath;
        QString remotePath;
        bool verify = true;
        QStringList remainingMethods; // auto-fallback chain
        qint64 bytesDone = 0;
        qint64 bytesTotal = 0;
        QString currentMethod;         // for the status endpoint
        QString action;                // audit action ("tab_upload"/"tab_download")
        QString auditDetail;           // audit detail (async transfers have no PendingOp)
        bool async = false;            // started with no waiting HTTP client
        // Cancel was requested: the slot is logically free (status reports
        // inactive, new transfers allowed) while the worker winds down in
        // the background and its finish signal cleans the record up.
        bool cancelled = false;
        qint64 startedMs = 0;
    };

    // A visible exec running in a GUI tab: the command was typed into the
    // terminal bracketed by unique begin/end markers; a timer polls the
    // buffer until the end marker (with the real exit code) appears.
    struct TabExec {
        QTcpSocket *socket = nullptr;
        int tabIndex = -1;
        QString token;
        QString command;
        QString echoStrip; // last non-empty command line (echo removal)
        QString detail;
        QString target;    // user@host:port for the response
        bool polluted = false; // previous exec on this tab timed out
        qint64 deadlineMs = 0; // 0 = no limit
        QTimer *timer = nullptr;
    };

    QTcpServer *server = nullptr;
    QHash<QTcpSocket *, QByteArray> buffers;
    QHash<QString, PendingOp> pendingOps; // requestId -> in-flight operation
    QHash<QString, TabTransfer> tabTransfers; // requestId -> tab transfer
    QHash<QString, TabExec> tabExecs; // requestId -> visible exec
    QSet<int> tabExecBusy; // tab indices with a running visible exec
    // Tabs whose last exec ended on timeout: the command kept running and
    // its late output can pollute the NEXT exec's capture (the 2026-09-11
    // "FW_OK 假成功"). The next exec on such a tab gets a warning field.
    QSet<int> tabExecPolluted;
    AgentTabsInterface *tabs = nullptr;   // GUI tabs provider (GUI mode only)
    int listenPort = 8222;
    QString error;
};

AgentHttpServer::AgentHttpServer(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
}

AgentHttpServer::~AgentHttpServer()
{
    stop();
}

bool AgentHttpServer::start(int port)
{
    if (d->server) {
        return true;
    }

    auto *server = new QTcpServer(this);
    if (!server->listen(QHostAddress::LocalHost, port)) {
        d->error = server->errorString();
        delete server;
        return false;
    }
    d->server = server;
    d->listenPort = server->serverPort();
    connect(server, &QTcpServer::newConnection, this, &AgentHttpServer::onNewConnection);
    // Accept errors used to be silent — the agent looked alive but answered
    // nothing (the 2026-09-11 "Agent 无声消失"). Surface them instead.
    connect(server, &QTcpServer::acceptError, this, [this](QAbstractSocket::SocketError) {
        const QString message = d->server ? d->server->errorString()
                                          : QStringLiteral("unknown accept error");
        AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("agent"),
                        QStringLiteral("port=%1").arg(d->listenPort),
                        QStringLiteral("accept error: %1").arg(message));
        emit acceptErrorOccurred(message);
    });
    return true;
}

void AgentHttpServer::stop()
{
    if (!d->server) {
        return;
    }
    for (QTcpSocket *socket : d->buffers.keys()) {
        socket->disconnectFromHost();
        socket->deleteLater();
    }
    d->buffers.clear();
    d->pendingOps.clear();
    for (auto it = d->tabExecs.begin(); it != d->tabExecs.end(); ++it) {
        if (it.value().timer) {
            it.value().timer->stop();
            it.value().timer->deleteLater();
        }
    }
    d->tabExecs.clear();
    d->tabExecBusy.clear();
    // Stop parentless transfer workers (they own threads + connections).
    for (auto it = d->tabTransfers.begin(); it != d->tabTransfers.end(); ++it) {
        if (it.value().worker) {
            it.value().worker->cancelTransfer();
            it.value().worker->stop();
            it.value().worker->deleteLater();
        }
    }
    d->tabTransfers.clear();
    d->server->close();
    d->server->deleteLater();
    d->server = nullptr;
}

bool AgentHttpServer::isRunning() const
{
    return d->server != nullptr;
}

int AgentHttpServer::port() const
{
    return d->listenPort;
}

QString AgentHttpServer::url() const
{
    return QStringLiteral("http://127.0.0.1:%1").arg(d->listenPort);
}

QString AgentHttpServer::errorString() const
{
    return d->error;
}

void AgentHttpServer::setTabsInterface(AgentTabsInterface *tabs)
{
    d->tabs = tabs;
}

namespace {

// Minimal HTTP/1.1 request parser. Returns false until the request is
// complete; bodies are sized by Content-Length.
bool parseHttpRequest(const QByteArray &data, HttpRequest *request)
{
    const int headerEnd = data.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        return false;
    }
    const QByteArray header = data.left(headerEnd);
    const QList<QByteArray> lines = header.split('\n');
    if (lines.isEmpty()) {
        return false;
    }

    const QList<QByteArray> requestLine = lines.first().trimmed().split(' ');
    if (requestLine.size() < 2) {
        return false;
    }
    request->method = requestLine.at(0);
    const QString target = QString::fromUtf8(requestLine.at(1));
    const int queryStart = target.indexOf(QLatin1Char('?'));
    if (queryStart >= 0) {
        request->path = target.left(queryStart);
        request->query = target.mid(queryStart + 1);
    } else {
        request->path = target;
        request->query.clear();
    }

    int contentLength = 0;
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon < 0) {
            continue;
        }
        const QByteArray name = line.left(colon).trimmed().toLower();
        const QByteArray value = line.mid(colon + 1).trimmed();
        request->headers[name] = value;
        if (name == "content-length") {
            contentLength = value.toInt();
        }
    }

    if (data.size() < headerEnd + 4 + contentLength) {
        return false;
    }
    request->body = data.mid(headerEnd + 4, contentLength);
    return true;
}

QJsonObject jsonError(const QString &message)
{
    QJsonObject object;
    object[QStringLiteral("error")] = message;
    return object;
}

// Parses a JSON request body. A body that is not valid UTF-8 JSON (classic
// case: PowerShell 5.1 posting Chinese text without an explicit charset —
// the bytes are GBK) used to yield an EMPTY object, so a stored-session
// lookup surfaced as a misleading 404 "not found". Fail fast with guidance.
bool parseJsonBody(const QByteArray &body, QJsonObject *object, QString *error)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QStringLiteral("Invalid JSON body (%1). Send UTF-8 JSON with "
                                "Content-Type: application/json; charset=utf-8")
                     .arg(parseError.error != QJsonParseError::NoError
                              ? parseError.errorString()
                              : QStringLiteral("not a JSON object"));
        return false;
    }
    *object = doc.object();
    return true;
}

} // namespace

void AgentHttpServer::onNewConnection()
{
    while (QTcpSocket *socket = d->server->nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            onReadyRead(socket);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            d->buffers.remove(socket);
            for (auto it = d->pendingOps.begin(); it != d->pendingOps.end();) {
                if (it.value().socket == socket) {
                    // The client is gone (typically a client-side timeout):
                    // cancel the server-side work too.
                    if (it.value().action == QLatin1String("tab_exec")) {
                        finishTabExec(it.key());
                    } else if (it.value().action.startsWith(QLatin1String("tab_"))) {
                        // Tab transfer: cancel its dedicated session.
                        auto tt = d->tabTransfers.constFind(it.key());
                        if (tt != d->tabTransfers.constEnd() && tt.value().worker) {
                            tt.value().worker->cancelTransfer();
                        }
                    }
                    it = d->pendingOps.erase(it);
                } else {
                    ++it;
                }
            }
            socket->deleteLater();
        });
        d->buffers[socket] = QByteArray();
    }
}

void AgentHttpServer::onReadyRead(QTcpSocket *socket)
{
    d->buffers[socket].append(socket->readAll());

    HttpRequest request;
    if (!parseHttpRequest(d->buffers[socket], &request)) {
        return;
    }
    d->buffers[socket].clear();
    handleRequest(socket, request);
}

void AgentHttpServer::handleRequest(QTcpSocket *socket, HttpRequest request)
{
    const QByteArray token = Config::instance().stringValue(QStringLiteral("agent/token")).toUtf8();
    if (!token.isEmpty()) {
        const QByteArray expected = "Bearer " + token;
        const QByteArray given = request.headers.value("authorization");
        if (given != expected && request.path != "/api/v1/status") {
            respondError(socket, 401, "Unauthorized");
            return;
        }
    }

    // /api/v2/ mirrors /api/v1/, but bare top-level JSON arrays come wrapped
    // in an object: PowerShell 5.1 mangles bare arrays on deserialization
    // ($arr.id joins all ids into one space-separated string).
    bool wrapArrays = false;
    if (request.path.startsWith(QStringLiteral("/api/v2/"))) {
        wrapArrays = true;
        request.path = QStringLiteral("/api/v1/") + request.path.mid(8);
    }

    const QByteArray &method = request.method;

    if (method == "GET" && request.path == "/api/v1/keys") {
        const QByteArray pem = Crypto::agentPublicKeyPem();
        if (pem.isEmpty()) {
            respondError(socket, 503, "Agent keys unavailable (application locked)");
            return;
        }
        QJsonObject body;
        body[QStringLiteral("publicKeyPem")] = QString::fromUtf8(pem);
        body[QStringLiteral("fingerprint")] = Crypto::agentKeyFingerprint();
        body[QStringLiteral("cipher")] = QStringLiteral("RSA-OAEP-SHA256+base64");
        respond(socket, 200, QJsonDocument(body).toJson(QJsonDocument::Compact));
        return;
    }

    if (method == "GET" && request.path == "/api/v1/status") {
        QJsonObject body;
        body[QStringLiteral("status")] = QStringLiteral("ok");
        body[QStringLiteral("name")] = QStringLiteral("hssh");
        body[QStringLiteral("version")] = QStringLiteral(HSSH_VERSION_STRING);
        body[QStringLiteral("pid")] = QCoreApplication::applicationPid();
        body[QStringLiteral("tabs")] = d->tabs ? d->tabs->listTabs().size() : 0;
        respond(socket, 200, QJsonDocument(body).toJson(QJsonDocument::Compact));
        return;
    }

    // Liveness + quick facts for diagnosing "is the agent alive?".
    if (method == "GET" && request.path == QStringLiteral("/api/v1/health")) {
        QJsonObject body;
        body[QStringLiteral("ok")] = true;
        body[QStringLiteral("pid")] = QCoreApplication::applicationPid();
        body[QStringLiteral("version")] = QStringLiteral(HSSH_VERSION_STRING);
        body[QStringLiteral("pendingOps")] = d->pendingOps.size();
        body[QStringLiteral("tabs")] = d->tabs ? d->tabs->listTabs().size() : 0;
        respond(socket, 200, QJsonDocument(body).toJson(QJsonDocument::Compact));
        return;
    }

    // GUI terminal tabs (available when the agent runs inside the GUI).
    if (method == "GET" && request.path == QStringLiteral("/api/v1/tabs")) {
        if (!d->tabs) {
            respondError(socket, 501, "GUI tabs are only available in the GUI agent");
            return;
        }
        QVariantList tabsList = d->tabs->listTabs();
        annotateTabRefs(tabsList, d->tabs->listSavedSessions());
        const QJsonArray array = QJsonArray::fromVariantList(tabsList);
        if (wrapArrays) {
            QJsonObject body;
            body[QStringLiteral("tabs")] = array;
            respond(socket, 200, QJsonDocument(body).toJson(QJsonDocument::Compact));
            return;
        }
        respond(socket, 200, QJsonDocument(array).toJson(QJsonDocument::Compact));
        return;
    }

    // Saved sessions from the repository (names/hosts only, no secrets).
    if (method == "GET" && request.path == QStringLiteral("/api/v1/saved-sessions")) {
        if (!d->tabs) {
            respondError(socket, 501, "Saved sessions are only available in the GUI agent");
            return;
        }
        const QJsonArray array = QJsonArray::fromVariantList(d->tabs->listSavedSessions());
        if (wrapArrays) {
            QJsonObject body;
            body[QStringLiteral("savedSessions")] = array;
            respond(socket, 200, QJsonDocument(body).toJson(QJsonDocument::Compact));
            return;
        }
        respond(socket, 200, QJsonDocument(array).toJson(QJsonDocument::Compact));
        return;
    }

    if (method == "POST" && request.path == QStringLiteral("/api/v1/tabs")) {
        if (!d->tabs) {
            respondError(socket, 501, "GUI tabs are only available in the GUI agent");
            return;
        }
        QJsonObject body;
        QString bodyError;
        if (!parseJsonBody(request.body, &body, &bodyError)) {
            respondError(socket, 400, bodyError);
            return;
        }
        const QString sessionName = body.value(QStringLiteral("session")).toString();
        int index = -1;
        QString auditTarget;
        if (!sessionName.isEmpty()) {
            index = d->tabs->openSessionTab(sessionName);
            auditTarget = QStringLiteral("session=%1").arg(sessionName);
        } else if (body.contains(QStringLiteral("host"))) {
            // Ad-hoc SSH tab: plaintext passwords are rejected by policy;
            // ciphers are decrypted here so plaintext never crosses the API.
            for (const char *key : {"passwordCipher", "keyPassphraseCipher"}) {
                if (body.contains(QLatin1String(key))
                    && body.value(QLatin1String(key)).toString().isEmpty()) {
                    respondError(socket, 400,
                                 QStringLiteral("%1 is null/empty; omit the field or provide a cipher")
                                     .arg(QLatin1String(key)));
                    return;
                }
            }
            if (body.contains(QStringLiteral("password"))) {
                respondError(socket, 400,
                             "Plaintext password rejected; use passwordCipher or a saved session");
                return;
            }
            SessionConfig config;
            config.setName(body.value(QStringLiteral("name")).toString());
            config.setHost(body.value(QStringLiteral("host")).toString());
            config.setPort(body.value(QStringLiteral("port")).toInt(22));
            config.setUsername(body.value(QStringLiteral("username")).toString());
            config.setSessionType(SessionType::Ssh);
            const QString authMethod = body.value(QStringLiteral("authMethod")).toString();
            if (authMethod == QLatin1String("publickey")) {
                config.setAuthMethod(AuthMethod::PublicKey);
                config.setPrivateKeyPath(body.value(QStringLiteral("privateKeyPath")).toString());
                const QString cipher = body.value(QStringLiteral("keyPassphraseCipher")).toString();
                if (!cipher.isEmpty()) {
                    bool ok = false;
                    const QByteArray plain = Crypto::rsaDecrypt(cipher.toUtf8(), &ok);
                    if (!ok) {
                        respondError(socket, 400, "Failed to decrypt keyPassphraseCipher (locked or bad cipher)");
                        return;
                    }
                    config.setKeyPassphrase(SecureString(plain));
                }
            } else if (authMethod == QLatin1String("agent")) {
                config.setAuthMethod(AuthMethod::Agent);
            } else if (authMethod == QLatin1String("keyboard-interactive")) {
                config.setAuthMethod(AuthMethod::KeyboardInteractive);
            } else {
                config.setAuthMethod(AuthMethod::Password);
            }
            const QString cipher = body.value(QStringLiteral("passwordCipher")).toString();
            if (!cipher.isEmpty()) {
                bool ok = false;
                const QByteArray plain = Crypto::rsaDecrypt(cipher.toUtf8(), &ok);
                if (!ok) {
                    respondError(socket, 400, "Failed to decrypt passwordCipher (locked or bad cipher)");
                    return;
                }
                config.setPassword(SecureString(plain));
            }
            auditTarget = QStringLiteral("host=%1 user=%2").arg(config.host(), config.username());
            index = d->tabs->openSshTab(config);
        } else {
            const QString shellType = body.value(QStringLiteral("shellType")).toString();
            index = d->tabs->openLocalTab(shellType);
            auditTarget = QStringLiteral("local");
        }
        if (index < 0) {
            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("tab_open"),
                            auditTarget, QStringLiteral("error: not found"));
            respondError(socket, 404, sessionName.isEmpty() && !body.contains(QStringLiteral("host"))
                                           ? "Failed to open a local terminal tab"
                                           : "Session not found or invalid: " + auditTarget);
            return;
        }
        AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("tab_open"),
                        auditTarget, QStringLiteral("ok index=%1").arg(index));
        QJsonObject result;
        result[QStringLiteral("index")] = index;
        QVariantList tabsList = d->tabs->listTabs();
        annotateTabRefs(tabsList, d->tabs->listSavedSessions());
        for (const QVariant &v : tabsList) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("index")).toInt() == index) {
                result[QStringLiteral("name")] = m.value(QStringLiteral("name")).toString();
                result[QStringLiteral("ref")] = m.value(QStringLiteral("ref")).toString();
                result[QStringLiteral("target")] = m.value(QStringLiteral("target")).toString();
                // Surface a stale tab immediately at open time: the caller
                // asked for a session whose endpoint changed since.
                if (m.value(QStringLiteral("stale")).toBool()) {
                    result[QStringLiteral("stale")] = true;
                    result[QStringLiteral("savedTarget")] =
                        m.value(QStringLiteral("savedTarget")).toString();
                }
                break;
            }
        }
        respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
        return;
    }

    if (request.path.startsWith(QStringLiteral("/api/v1/tabs/"))) {
        if (!d->tabs) {
            respondError(socket, 501, "GUI tabs are only available in the GUI agent");
            return;
        }
        const QString prefix = QStringLiteral("/api/v1/tabs/");
        QString rest = request.path.mid(prefix.size());
        const int slash = rest.indexOf(QLatin1Char('/'));
        const QString indexText = slash < 0 ? rest : rest.left(slash);
        const QString sub = slash < 0 ? QString() : rest.mid(slash + 1);
        if (indexText.isEmpty()) {
            respondError(socket, 400, "Missing tab reference");
            return;
        }
        // Name-based refs ("<name>" or "<name>:<ordinal>") are resolved
        // against the live tab list; plain integers keep the legacy
        // positional meaning.
        QString refError;
        const int index = resolveTabRef(d->tabs->listTabs(), indexText, &refError);
        if (index < 0) {
            respondError(socket, 404, refError);
            return;
        }

        if (method == "GET" && sub == QStringLiteral("text")) {
            int maxLines = 0; // 0 = whole buffer
            int fromLine = 0;
            for (const QString &part : request.query.split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
                const int eq = part.indexOf(QLatin1Char('='));
                if (eq > 0 && part.left(eq) == QLatin1String("lines")) {
                    maxLines = part.mid(eq + 1).toInt();
                } else if (eq > 0 && part.left(eq) == QLatin1String("from")) {
                    fromLine = part.mid(eq + 1).toInt();
                }
            }
            QString text;
            if (!d->tabs->readTabRange(index, fromLine, maxLines, &text)) {
                respondError(socket, 404, "Unknown tab index: " + indexText);
                return;
            }
            QJsonObject result;
            result[QStringLiteral("index")] = index;
            result[QStringLiteral("from")] = fromLine;
            result[QStringLiteral("lines")] = text.isEmpty() ? 0 : static_cast<int>(text.count(QLatin1Char('\n'))) + 1;
            result[QStringLiteral("text")] = text;
            respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
            return;
        }

        if (method == "POST" && sub == QStringLiteral("send")) {
            QJsonObject body;
            QString bodyError;
            if (!parseJsonBody(request.body, &body, &bodyError)) {
                respondError(socket, 400, bodyError);
                return;
            }
            // Raw input mode: {"data":"..."} sends the bytes verbatim (no
            // auto-Enter) 鈥?use it for control characters like Ctrl+C
            // ("\u0003") and Ctrl+D ("\u0004"). {"command":"..."} keeps
            // the old "type command + Enter" semantic.
            const QString data = body.value(QStringLiteral("data")).toString();
            if (!data.isEmpty()) {
                if (!d->tabs->sendInputToTab(index, data)) {
                    respondError(socket, 404, "Unknown tab index: " + indexText);
                    return;
                }
                // Never log raw input: it may carry secrets or control bytes.
                AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("tab_input"),
                                QStringLiteral("tab=%1 <raw %2 bytes>").arg(index).arg(data.toUtf8().size()),
                                QStringLiteral("ok"));
                QJsonObject result;
                result[QStringLiteral("ok")] = true;
                respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
                return;
            }
            const QString command = body.value(QStringLiteral("command")).toString();
            if (command.isEmpty()) {
                respondError(socket, 400, "Missing command");
                return;
            }
            if (!d->tabs->sendToTab(index, command)) {
                respondError(socket, 404, "Unknown tab index: " + indexText);
                return;
            }
            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("tab_send"),
                            QStringLiteral("tab=%1 cmd=\"%2\"").arg(index).arg(command),
                            QStringLiteral("ok"));
            QJsonObject result;
            result[QStringLiteral("ok")] = true;
            respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
            return;
        }

        if (method == "POST" && sub == QStringLiteral("secure-input")) {
            QJsonObject body;
            QString bodyError;
            if (!parseJsonBody(request.body, &body, &bodyError)) {
                respondError(socket, 400, bodyError);
                return;
            }
            const QString cipher = body.value(QStringLiteral("cipher")).toString();
            if (cipher.isEmpty()) {
                respondError(socket, 400, "Missing cipher");
                return;
            }
            bool ok = false;
            QByteArray plain = Crypto::rsaDecrypt(cipher.toUtf8(), &ok);
            if (!ok) {
                AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("secure_input"),
                                QStringLiteral("tab=%1").arg(index),
                                QStringLiteral("error: decrypt failed"));
                respondError(socket, 503, "Failed to decrypt cipher (application locked or bad cipher)");
                return;
            }
            const QString text = QString::fromUtf8(plain);
            plain.fill('\0');
            if (!d->tabs->sendSecretToTab(index, text)) {
                respondError(socket, 404, "Unknown tab index: " + indexText);
                return;
            }
            // Never log the secret itself: only that a secret was delivered.
            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("secure_input"),
                            QStringLiteral("tab=%1 <cipher %2 bytes>").arg(index).arg(cipher.size()),
                            QStringLiteral("ok"));
            QJsonObject result;
            result[QStringLiteral("ok")] = true;
            respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
            return;
        }

        // File transfer for an SSH tab over a DEDICATED connection (never
        // shares the terminal's ssh_session: libssh sessions are not
        // thread-safe). `method`: "sftp" (default), "scp" (remote scp
        // binary), "shell" (base64 over an exec channel — works without
        // sftp-server), "auto" (sftp -> scp -> shell while zero bytes have
        // moved). Uploads and downloads verify content (md5) by default.
        if (method == "POST"
            && (sub == QStringLiteral("upload") || sub == QStringLiteral("download"))) {
            const bool isUpload = sub == QStringLiteral("upload");
            QJsonObject body;
            QString bodyError;
            if (!parseJsonBody(request.body, &body, &bodyError)) {
                respondError(socket, 400, bodyError);
                return;
            }
            const QString localPath = body.value(QStringLiteral("localPath")).toString();
            const QString remotePath = body.value(QStringLiteral("remotePath")).toString();
            if (localPath.isEmpty() || remotePath.isEmpty()) {
                respondError(socket, 400, "localPath and remotePath are required");
                return;
            }
            const QString transferMethod = body.value(QStringLiteral("method"))
                                               .toString(QStringLiteral("sftp"))
                                               .toLower();
            if (transferMethod != QLatin1String("sftp")
                && transferMethod != QLatin1String("scp")
                && transferMethod != QLatin1String("shell")
                && transferMethod != QLatin1String("auto")) {
                respondError(socket, 400, "Unknown transfer method: " + transferMethod
                                          + " (expected sftp, scp, shell or auto)");
                return;
            }
            const SessionConfig config = d->tabs->sessionConfigForTab(index);
            if (config.sessionType() != SessionType::Ssh || config.host().isEmpty()) {
                respondError(socket, 400, "Tab is not an SSH session: " + indexText);
                return;
            }
        // A cancelled transfer keeps its record until the worker's finish
        // signal arrives (resource cleanup stays signal-driven); it must
        // NOT hold the tab's transfer slot (2026-09-20: cancel took up to
        // tens of seconds to release the 409 lock while the worker was
        // still in its connect/stat phase).
        bool slotBusy = false;
        for (auto it = d->tabTransfers.cbegin(); it != d->tabTransfers.cend(); ++it) {
            if (it.value().tabIndex == index && !it.value().cancelled) {
                slotBusy = true;
                break;
            }
        }
        if (slotBusy) {
            respondError(socket, 409, "Another transfer is running on this tab");
            return;
        }

            QStringList chain;
            if (transferMethod == QLatin1String("auto")) {
                chain = {QStringLiteral("sftp"), QStringLiteral("scp"), QStringLiteral("shell")};
            } else {
                chain = {transferMethod};
            }
            // async:true returns immediately after starting the transfer;
            // progress/completion is then tracked via GET /tabs/<ref>/transfer
            // and aborted via DELETE /tabs/<ref>/transfer. Long transfers that
            // would outlive an MCP client's tool timeout should use this.
            const bool asyncStart = body.value(QStringLiteral("async")).toBool(false);

            const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            Impl::PendingOp op;
            op.socket = socket;
            op.action = isUpload ? QStringLiteral("tab_upload") : QStringLiteral("tab_download");
            op.sessionId = QStringLiteral("tab:%1").arg(index);
            op.detail = isUpload
                ? QStringLiteral("tab=%1 method=%2 local=%3 remote=%4")
                      .arg(index).arg(chain.first()).arg(localPath, remotePath)
                : QStringLiteral("tab=%1 method=%2 remote=%3 local=%4")
                      .arg(index).arg(chain.first()).arg(remotePath, localPath);
            op.startedMs = QDateTime::currentMSecsSinceEpoch();
            if (!asyncStart) {
                d->pendingOps[requestId] = op;
            }

            Impl::TabTransfer transfer;
            transfer.tabIndex = index;
            transfer.config = config;
            transfer.isUpload = isUpload;
            transfer.localPath = localPath;
            transfer.remotePath = remotePath;
            transfer.verify = body.value(QStringLiteral("verify")).toBool(true);
            transfer.remainingMethods = chain.mid(1);
            transfer.currentMethod = chain.first();
            transfer.action = op.action;
            transfer.auditDetail = op.detail;
            transfer.async = asyncStart;
            transfer.startedMs = op.startedMs;
            d->tabTransfers[requestId] = transfer;
            startTabTransferWorker(requestId, chain.first());

            if (asyncStart) {
                AgentAudit::log(AgentAudit::Source::Rest, op.action, op.detail,
                                QStringLiteral("started (async)"));
                QJsonObject result;
                result[QStringLiteral("ok")] = true;
                result[QStringLiteral("started")] = true;
                result[QStringLiteral("async")] = true;
                result[QStringLiteral("method")] = chain.first();
                result[QStringLiteral("direction")] = isUpload ? QStringLiteral("upload")
                                                                : QStringLiteral("download");
                result[QStringLiteral("target")] = tabTarget(index);
                result[QStringLiteral("statusPath")] =
                    QStringLiteral("/api/v1/tabs/%1/transfer").arg(indexText);
                respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
            }
            return;
        }

        // Transfer status: progress of the (single) transfer on this tab.
        if (method == "GET" && sub == QStringLiteral("transfer")) {
            QJsonObject result;
            result[QStringLiteral("active")] = false;
            for (auto it = d->tabTransfers.cbegin(); it != d->tabTransfers.cend(); ++it) {
                if (it.value().tabIndex != index) {
                    continue;
                }
                const Impl::TabTransfer &t = it.value();
                // A cancelled transfer is logically over even while the
                // worker winds down in the background.
                if (t.cancelled) {
                    result[QStringLiteral("cancelled")] = true;
                    break;
                }
                result[QStringLiteral("active")] = true;
                result[QStringLiteral("direction")] = t.isUpload
                    ? QStringLiteral("upload") : QStringLiteral("download");
                result[QStringLiteral("method")] = t.currentMethod;
                result[QStringLiteral("localPath")] = t.localPath;
                result[QStringLiteral("remotePath")] = t.remotePath;
                result[QStringLiteral("bytesDone")] = t.bytesDone;
                result[QStringLiteral("bytesTotal")] = t.bytesTotal;
                result[QStringLiteral("startedMs")] = t.startedMs;
                result[QStringLiteral("elapsedMs")] =
                    QDateTime::currentMSecsSinceEpoch() - t.startedMs;
                break;
            }
            respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
            return;
        }

        // Transfer cancel: stops the transfer on this tab. The local .part
        // file is KEPT so an sftp retry can resume from it.
        if (method == "DELETE" && sub == QStringLiteral("transfer")) {
            for (auto it = d->tabTransfers.begin(); it != d->tabTransfers.end(); ++it) {
                if (it.value().tabIndex != index) {
                    continue;
                }
                if (it.value().worker) {
                    it.value().worker->cancelTransfer();
                }
                // Immediate logical release: the record stays only until the
                // worker's finish signal (background cleanup); the tab slot
                // and the status endpoint are free right now.
                it.value().cancelled = true;
                AgentAudit::log(AgentAudit::Source::Rest, it.value().action,
                                it.value().auditDetail, QStringLiteral("cancelled by client"));
                QJsonObject result;
                result[QStringLiteral("ok")] = true;
                result[QStringLiteral("cancelled")] = true;
                respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
                return;
            }
            respondError(socket, 404, "No transfer is running on this tab");
            return;
        }

        // Sudo with user consent. PH-fix 2026-09-20: fully asynchronous —
        // the old synchronous dialog ran a NESTED event loop for up to 30 s,
        // racing the MCP client's own 30 s tool timeout (the client
        // occasionally saw an empty reply = "executed=None"). The request
        // hangs off PendingOp like exec/transfers; the callback responds.
        if (method == "POST" && sub == QStringLiteral("sudo")) {
            QJsonObject body;
            QString bodyError;
            if (!parseJsonBody(request.body, &body, &bodyError)) {
                respondError(socket, 400, bodyError);
                return;
            }
            const QString command = body.value(QStringLiteral("command")).toString();
            if (command.isEmpty()) {
                respondError(socket, 400, "Missing command");
                return;
            }
            // Password is optional: cipher, stored credential, or none.
            QString secret;
            const QString cipher = body.value(QStringLiteral("passwordCipher")).toString();
            if (!cipher.isEmpty()) {
                bool cipherOk = false;
                const QByteArray plain = Crypto::rsaDecrypt(cipher.toUtf8(), &cipherOk);
                if (!cipherOk) {
                    respondError(socket, 503,
                                 "Failed to decrypt passwordCipher (locked or bad cipher)");
                    return;
                }
                secret = QString::fromUtf8(plain);
            }
            const bool useStored = body.value(QStringLiteral("useStoredCredential")).toBool(true);
            const int timeoutMs = body.value(QStringLiteral("timeout")).toInt(60000);

            const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            Impl::PendingOp op;
            op.socket = socket;
            op.action = QStringLiteral("sudo");
            op.detail = QStringLiteral("tab=%1 cmd=\"%2\"").arg(index).arg(command);
            op.startedMs = QDateTime::currentMSecsSinceEpoch();
            d->pendingOps[requestId] = op;
            AgentAudit::log(AgentAudit::Source::Rest, op.action, op.detail,
                            QStringLiteral("invoked (async consent)"));

            d->tabs->sudoAsync(index, command, secret, useStored, timeoutMs,
                [this, requestId, index, indexText](
                    const AgentTabsInterface::SudoAsyncResult &r) {
                const auto opIt = d->pendingOps.find(requestId);
                if (opIt == d->pendingOps.end()) {
                    return; // client went away; nothing to answer
                }
                const Impl::PendingOp pend = opIt.value();
                d->pendingOps.erase(opIt);

                if (!r.confirmed) {
                    AgentAudit::log(AgentAudit::Source::Rest, pend.action, pend.detail,
                                    QStringLiteral("rejected: %1").arg(r.reason));
                    if (r.reason == QLatin1String("unknown_tab")) {
                        respondError(pend.socket, 404,
                                     "Unknown tab (index may have drifted; use the "
                                     "drift-safe ref from GET /api/v1/tabs): " + indexText);
                        return;
                    }
                    QJsonObject result;
                    result[QStringLiteral("error")] =
                        r.reason == QLatin1String("timeout")
                            ? QStringLiteral("Sudo confirmation timed out after 30 s: the "
                                             "dialog was not answered (it may be hidden or "
                                             "on another desktop). Retry; check the hssh "
                                             "window/taskbar.")
                            : QStringLiteral("User rejected the sudo request");
                    result[QStringLiteral("reason")] = r.reason;
                    respond(pend.socket, 403,
                            QJsonDocument(result).toJson(QJsonDocument::Compact));
                    return;
                }
                if (!r.ran) {
                    AgentAudit::log(AgentAudit::Source::Rest, pend.action, pend.detail,
                                    QStringLiteral("error: %1").arg(r.errorMessage));
                    const int status =
                        r.errorMessage == QLatin1String("passwordRequired") ? 428 : 500;
                    QJsonObject result;
                    result[QStringLiteral("error")] = r.errorMessage;
                    if (status == 428) {
                        result[QStringLiteral("passwordRequired")] = true;
                    }
                    respond(pend.socket, status,
                            QJsonDocument(result).toJson(QJsonDocument::Compact));
                    return;
                }
                AgentAudit::log(AgentAudit::Source::Rest, pend.action, pend.detail,
                                r.timedOut ? QStringLiteral("ok (timed out)")
                                           : QStringLiteral("ok"));
                QJsonObject result;
                result[QStringLiteral("output")] = r.output;
                result[QStringLiteral("timedOut")] = r.timedOut;
                // executed=false means the completion sentinel never appeared:
                // the command was NOT confirmed to have run. Always verify
                // executed + exitCode before trusting a sudo result.
                result[QStringLiteral("executed")] = !r.timedOut;
                result[QStringLiteral("exitCode")] = r.exitCode;
                const QString sudoTarget = tabTarget(index);
                if (!sudoTarget.isEmpty()) {
                    result[QStringLiteral("target")] = sudoTarget;
                }
                respond(pend.socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
            });
            return;
        }

        // Visible exec: types the command into the terminal bracketed by
        // unique begin/end markers, then polls the buffer until the end
        // marker (carrying the real exit code) appears. The user watches
        // the command run in the tab. One exec per tab at a time; the tab
        // must sit at a shell prompt (input goes to the foreground program).
        // Options: timeout (ms, default 120 s, 0 = no limit), reset (send
        // Ctrl+C first and wait 400 ms — clears a stuck continuation prompt
        // or half-typed line; the foreground program is interrupted).
        if (method == "POST" && sub == QStringLiteral("exec")) {
            QJsonObject body;
            QString bodyError;
            if (!parseJsonBody(request.body, &body, &bodyError)) {
                respondError(socket, 400, bodyError);
                return;
            }
            const QString command = body.value(QStringLiteral("command")).toString();
            if (command.isEmpty()) {
                respondError(socket, 400, "Missing command");
                return;
            }
            if (d->tabExecBusy.contains(index)) {
                respondError(socket, 409, "Another exec is running on this tab");
                return;
            }
            // timeout in ms; default 120 s; explicit 0 disables the limit.
            const int timeoutMs = body.contains(QStringLiteral("timeout"))
                                      ? body.value(QStringLiteral("timeout")).toInt(0)
                                      : 120000;
            const bool reset = body.value(QStringLiteral("reset")).toBool(false);

            const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            Impl::PendingOp op;
            op.socket = socket;
            op.action = QStringLiteral("tab_exec");
            op.detail = QStringLiteral("tab=%1 cmd=\"%2\"").arg(index).arg(command);
            op.startedMs = QDateTime::currentMSecsSinceEpoch();
            d->pendingOps[requestId] = op;

            Impl::TabExec exec;
            exec.socket = socket;
            exec.tabIndex = index;
            exec.command = command;
            exec.target = tabTarget(index);
            // A previous timed-out exec on this tab may still be running:
            // flag the pollution risk on THIS response, then clear.
            exec.polluted = d->tabExecPolluted.contains(index);
            d->tabExecPolluted.remove(index);
            exec.detail = op.detail;
            exec.deadlineMs = timeoutMs > 0
                ? QDateTime::currentMSecsSinceEpoch() + timeoutMs : 0;
            d->tabExecs[requestId] = exec;
            d->tabExecBusy.insert(index);

            if (reset) {
                // Clear a stuck continuation prompt / half-typed line first;
                // the marker sequence is typed once the shell recovered.
                d->tabs->sendInputToTab(index, QStringLiteral("\u0003"));
                QTimer::singleShot(400, this, [this, requestId]() {
                    beginTabExec(requestId);
                });
            } else {
                beginTabExec(requestId);
            }

            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("tab_exec"),
                            op.detail, QStringLiteral("invoked"));
            return;
        }

        // PH2-10: ZMODEM send (local file -> remote rz). The engine types
        // "rz" itself and reports progress on the terminal.
        if (method == "POST" && sub == QStringLiteral("zsend")) {
            QJsonObject body;
            QString bodyError;
            if (!parseJsonBody(request.body, &body, &bodyError)) {
                respondError(socket, 400, bodyError);
                return;
            }
            const QString localPath = body.value(QStringLiteral("localPath")).toString();
            if (localPath.isEmpty()) {
                respondError(socket, 400, "localPath is required");
                return;
            }
            if (!d->tabs->zmodemSendToTab(index, localPath)) {
                respondError(socket, 409,
                             "Cannot start ZMODEM send (a transfer is active, or the "
                             "file is unreadable/empty): " + localPath);
                return;
            }
            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("tab_zsend"),
                            QStringLiteral("tab=%1 local=%2").arg(index).arg(localPath),
                            QStringLiteral("invoked"));
            QJsonObject result;
            result[QStringLiteral("ok")] = true;
            result[QStringLiteral("started")] = true;
            result[QStringLiteral("target")] = tabTarget(index);
            respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
            return;
        }

        // Reconnect a disconnected SSH tab (Enter-to-reconnect equivalent).
        if (method == "POST" && sub == QStringLiteral("reconnect")) {
            if (!d->tabs->reconnectTab(index)) {
                respondError(socket, 400, "Tab is not a disconnected SSH tab: " + indexText);
                return;
            }
            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("tab_reconnect"),
                            QStringLiteral("tab=%1").arg(index), QStringLiteral("invoked"));
            QJsonObject result;
            result[QStringLiteral("ok")] = true;
            result[QStringLiteral("target")] = tabTarget(index);
            respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
            return;
        }

        if (method == "DELETE" && sub.isEmpty()) {
            if (!d->tabs->closeTab(index)) {
                respondError(socket, 404, "Unknown tab index: " + indexText);
                return;
            }
            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("tab_close"),
                            QStringLiteral("tab=%1").arg(index), QStringLiteral("ok"));
            QJsonObject result;
            result[QStringLiteral("ok")] = true;
            respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
            return;
        }

        respondError(socket, 404, "Not found: " + request.path.toUtf8());
        return;
    }

    respondError(socket, 404, "Not found: " + request.path.toUtf8());
}


// Creates the transfer worker for `method` and starts it. The request's
// TabTransfer bookkeeping (paths, verify, fallback chain) must already be
// in d->tabTransfers.
void AgentHttpServer::startTabTransferWorker(const QString &requestId, const QString &method)
{
    auto it = d->tabTransfers.find(requestId);
    if (it == d->tabTransfers.end()) {
        return;
    }
    Impl::TabTransfer &transfer = it.value();

    TransferSession *worker = nullptr;
    // CRITICAL: workers must be parentless. SftpSession/ChannelCopySession
    // moveToThread their worker thread in the constructor — a parented
    // QObject refuses the move, silently leaving the transfer running on
    // the GUI thread (freezing every agent call for the whole transfer —
    // the 2026-09-18 exec-blocked-during-download report). Cleanup is
    // deleteLater on finish plus AgentHttpServer::stop().
    if (method == QLatin1String("scp")) {
        worker = new ChannelCopySession(transfer.config, ChannelCopySession::Mode::Scp);
    } else if (method == QLatin1String("shell")) {
        worker = new ChannelCopySession(transfer.config, ChannelCopySession::Mode::Base64);
    } else {
        worker = new SftpSession(transfer.config);
    }
    transfer.worker = worker;
    transfer.currentMethod = method;

    // Keep the audit detail in sync with the method actually running
    // (fallbacks rewrite it).
    {
        static const QRegularExpression methodRe(QStringLiteral("method=\\S+"));
        transfer.auditDetail.replace(methodRe, QStringLiteral("method=") + method);
        auto opIt = d->pendingOps.find(requestId);
        if (opIt != d->pendingOps.end()) {
            opIt.value().detail = transfer.auditDetail;
        }
    }

    // NB: when the initial connect fails, errorOccurred fires AND the queued
    // transfer then fails with transferFinished — the first signal wins; the
    // second finds no worker anymore and is ignored.
    connect(worker, &TransferSession::transferFinished, this,
            [this, requestId](const QString &path, bool ok, const QString &message) {
                onTabTransferFinished(requestId, path, ok, message);
            });
    connect(worker, &TransferSession::errorOccurred, this,
            [this, requestId](const QString &message) {
                onTabTransferFinished(requestId, QString(), false, message);
            });
    connect(worker, &TransferSession::transferProgress, this,
            [this, requestId](const QString &, qint64 bytesDone, qint64 bytesTotal) {
                auto tt = d->tabTransfers.find(requestId);
                if (tt != d->tabTransfers.end()) {
                    tt.value().bytesDone = bytesDone;
                    if (bytesTotal > 0) {
                        tt.value().bytesTotal = bytesTotal;
                    }
                }
            });
    worker->start();
    if (transfer.isUpload) {
        worker->upload(transfer.localPath, transfer.remotePath, transfer.verify);
    } else {
        worker->download(transfer.remotePath, transfer.localPath, transfer.verify);
    }
}

void AgentHttpServer::onTabTransferFinished(const QString &requestId, const QString &path,
                                            bool ok, const QString &message)
{
    const auto tt = d->tabTransfers.find(requestId);
    if (tt == d->tabTransfers.end()) {
        return; // already handled (errorOccurred + transferFinished pair)
    }

    // "auto" fallback: walk the remaining chain while the failing method
    // never got a single byte across (subsystem/channel-level failure).
    // A failure after bytes moved is a real error and is reported as-is.
    // A user-cancelled transfer never falls back.
    if (!ok && !tt.value().cancelled && !tt.value().remainingMethods.isEmpty()
        && tt.value().bytesDone == 0
        && (d->pendingOps.contains(requestId) || tt.value().async)) {
        TransferSession *old = tt.value().worker;
        const QString failedMethod = old ? old->metaObject()->className() : QString();
        AgentAudit::log(AgentAudit::Source::Rest,
                        tt.value().isUpload ? QStringLiteral("tab_upload")
                                            : QStringLiteral("tab_download"),
                        QStringLiteral("method fallback after %1 failed with 0 bytes: %2")
                            .arg(failedMethod, message),
                        QStringLiteral("retry with %1").arg(tt.value().remainingMethods.first()));
        if (old) {
            old->stop();
            old->deleteLater();
        }
        const QString next = tt.value().remainingMethods.takeFirst();
        startTabTransferWorker(requestId, next);
        return;
    }

    TransferSession *worker = tt.value().worker;
    const bool wasAsync = tt.value().async;
    const QString auditAction = tt.value().action;
    const QString auditDetail = tt.value().auditDetail;
    d->tabTransfers.erase(tt);

    const auto opIt = d->pendingOps.find(requestId);
    if (opIt != d->pendingOps.end()) {
        const Impl::PendingOp op = opIt.value();
        d->pendingOps.erase(opIt);
        AgentAudit::log(AgentAudit::Source::Rest, op.action, op.detail,
                        ok ? (message.isEmpty() ? QStringLiteral("ok") : QStringLiteral("ok %1").arg(message))
                           : QStringLiteral("error: %1").arg(message));
        if (!ok) {
            respondError(op.socket, 500, message);
        } else {
            QJsonObject result;
            result[QStringLiteral("ok")] = true;
            result[QStringLiteral("path")] = path;
            // Success note carries the method and md5 ("downloaded via scp,
            // md5 verified: ...").
            if (!message.isEmpty()) {
                result[QStringLiteral("message")] = message;
            }
            respond(op.socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
        }
    } else if (wasAsync) {
        // Async transfers outlive their starting request: the completion is
        // only visible through the audit log (and the file itself).
        AgentAudit::log(AgentAudit::Source::Rest, auditAction, auditDetail,
                        ok ? (message.isEmpty() ? QStringLiteral("ok (async)") : QStringLiteral("ok (async) %1").arg(message))
                           : QStringLiteral("error (async): %1").arg(message));
    }
    if (worker) {
        worker->stop();
        worker->deleteLater();
    }
}

// Types the marker-bracketed command into the tab and starts the poll
// timer. Called directly by the exec route, or ~400 ms after a reset
// Ctrl+C. The leading bare newline submits any half-typed line first, so
// the begin marker lands on a fresh prompt (first-char-eaten protection).
void AgentHttpServer::beginTabExec(const QString &requestId)
{
    const auto it = d->tabExecs.find(requestId);
    if (it == d->tabExecs.end()) {
        return;
    }
    Impl::TabExec &exec = it.value();

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    exec.token = token;
    // Echo-removal anchor: the echoed command's trailing text.
    for (const QString &l : exec.command.split(QLatin1Char('\n'))) {
        if (!l.trimmed().isEmpty()) {
            exec.echoStrip = l.trimmed();
        }
    }
    const QString input = QStringLiteral("\necho __HSSH_EXEC_B_%1__\n").arg(token)
                          + exec.command + QLatin1Char('\n')
                          + QStringLiteral("echo __HSSH_EXEC_E_%1__$?\n").arg(token);
    if (!d->tabs->sendInputToTab(exec.tabIndex, input)) {
        const auto opIt = d->pendingOps.find(requestId);
        if (opIt != d->pendingOps.end()) {
            const Impl::PendingOp op = opIt.value();
            d->pendingOps.erase(opIt);
            AgentAudit::log(AgentAudit::Source::Rest, op.action, op.detail,
                            QStringLiteral("error: tab gone"));
            respondError(op.socket, 404, "Tab closed before the exec could start");
        }
        finishTabExec(requestId);
        return;
    }

    exec.timer = new QTimer(this);
    exec.timer->setInterval(150);
    connect(exec.timer, &QTimer::timeout, this, [this, requestId]() {
        pollTabExec(requestId);
    });
    exec.timer->start();
}

// "user@host:port" for a tab index, preferring the live peer address over
// the configured host (empty when the tab cannot be found).
QString AgentHttpServer::tabTarget(int index) const
{
    if (!d->tabs) {
        return {};
    }
    QVariantList tabsList = d->tabs->listTabs();
    annotateTabRefs(tabsList, d->tabs->listSavedSessions());
    for (const QVariant &v : tabsList) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("index")).toInt() == index) {
            return m.value(QStringLiteral("target")).toString();
        }
    }
    return {};
}

void AgentHttpServer::finishTabExec(const QString &requestId)
{
    const auto it = d->tabExecs.find(requestId);
    if (it == d->tabExecs.end()) {
        return;
    }
    if (it.value().timer) {
        it.value().timer->stop();
        it.value().timer->deleteLater();
    }
    d->tabExecBusy.remove(it.value().tabIndex);
    d->tabExecs.erase(it);
}

void AgentHttpServer::pollTabExec(const QString &requestId)
{
    const auto it = d->tabExecs.find(requestId);
    if (it == d->tabExecs.end()) {
        return;
    }
    const Impl::TabExec exec = it.value();

    // Connection-drop detection: a dead transport can never produce the end
    // marker, so the exec would sit on the busy lock until its deadline
    // (forever with timeout:0 — the 2026-09-11 "409 锁死"). Fail fast and
    // release the tab instead.
    bool tabGone = true;
    bool tabDisconnected = false;
    if (d->tabs) {
        for (const QVariant &v : d->tabs->listTabs()) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("index")).toInt() == exec.tabIndex) {
                tabGone = false;
                tabDisconnected =
                    m.value(QStringLiteral("type")).toString() == QLatin1String("ssh")
                    && !m.value(QStringLiteral("connected")).toBool();
                break;
            }
        }
    }
    if (tabGone || tabDisconnected) {
        const auto opIt = d->pendingOps.find(requestId);
        if (opIt != d->pendingOps.end()) {
            const Impl::PendingOp op = opIt.value();
            d->pendingOps.erase(opIt);
            AgentAudit::log(AgentAudit::Source::Rest, op.action, op.detail,
                            tabGone ? QStringLiteral("error: tab closed")
                                    : QStringLiteral("error: connection lost"));
            respondError(op.socket, tabGone ? 404 : 500,
                         tabGone ? "Tab closed while the exec was running"
                                 : "Connection lost while the exec was running "
                                   "(the tab is disconnected; POST /tabs/<ref>/reconnect "
                                   "or reconnect it in the GUI)");
        }
        finishTabExec(requestId);
        return;
    }

    QString text;
    if (!d->tabs || !d->tabs->readTabRange(exec.tabIndex, 0, 0, &text)) {
        const auto opIt = d->pendingOps.find(requestId);
        if (opIt != d->pendingOps.end()) {
            const Impl::PendingOp op = opIt.value();
            d->pendingOps.erase(opIt);
            AgentAudit::log(AgentAudit::Source::Rest, op.action, op.detail,
                            QStringLiteral("error: tab closed"));
            respondError(op.socket, 404, "Tab closed while the exec was running");
        }
        finishTabExec(requestId);
        return;
    }

    const QString beginMarker = QStringLiteral("__HSSH_EXEC_B_%1__").arg(exec.token);
    const QString endPrefix = QStringLiteral("__HSSH_EXEC_E_%1__").arg(exec.token);
    // The end command is `echo __HSSH_EXEC_E_<token>__$?`: the shell expands
    // $? right after the trailing "__", so the output line is the prefix
    // immediately followed by the exit code digits.
    const QRegularExpression endRe(QStringLiteral("^%1(\\d+)$").arg(endPrefix));

    const QStringList lines = text.split(QLatin1Char('\n'));
    int endLine = -1;
    int exitCode = -1;
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QRegularExpressionMatch m = endRe.match(lines.at(i).trimmed());
        if (m.hasMatch()) {
            endLine = i;
            exitCode = m.captured(1).toInt();
            break;
        }
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool timedOut = endLine < 0 && exec.deadlineMs > 0 && now > exec.deadlineMs;
    if (endLine < 0 && !timedOut) {
        return; // keep polling
    }

    // The begin marker locates the start of THIS exec's output; when it has
    // scrolled out of the buffer the capture is necessarily partial.
    int beginLine = -1;
    const int stop = endLine >= 0 ? endLine : lines.size();
    for (int i = stop - 1; i >= 0; --i) {
        if (lines.at(i).trimmed() == beginMarker) {
            beginLine = i;
            break;
        }
    }

    QStringList region;
    for (int i = beginLine + 1; i < stop; ++i) {
        // Marker echo lines (typed input echo immediately, even while the
        // command runs) carry the token and pollute the region; the actual
        // marker OUTPUT lines are excluded by position (beginLine/endLine).
        const QString trimmed = lines.at(i).trimmed();
        if (trimmed.contains(endPrefix) || trimmed.contains(beginMarker)) {
            continue;
        }
        region.append(lines.at(i));
    }

    // Strip the echoed command from the front: accumulate rows until the
    // running concatenation ends with the command's last line (the prompt
    // prefix and line wrapping make row-wise matching unreliable).
    if (!exec.echoStrip.isEmpty()) {
        QString acc;
        int drop = 0;
        for (; drop < region.size() && drop < 30; ++drop) {
            acc += region.at(drop).trimmed();
            if (acc.endsWith(exec.echoStrip)) {
                region = region.mid(drop + 1);
                break;
            }
        }
    }

    // Terminal rows are space-padded to full width: trim right (leading
    // whitespace is meaningful output indentation and stays).
    for (QString &line : region) {
        while (line.endsWith(QLatin1Char(' '))) {
            line.chop(1);
        }
    }
    while (!region.isEmpty() && region.last().isEmpty()) {
        region.removeLast();
    }

    const auto opIt = d->pendingOps.find(requestId);
    if (opIt != d->pendingOps.end()) {
        const Impl::PendingOp op = opIt.value();
        d->pendingOps.erase(opIt);
        AgentAudit::log(AgentAudit::Source::Rest, op.action, op.detail,
                        timedOut ? QStringLiteral("timed out")
                                 : QStringLiteral("ok exit=%1").arg(exitCode));
        QJsonObject result;
        result[QStringLiteral("output")] = region.join(QLatin1Char('\n'));
        result[QStringLiteral("exitCode")] = exitCode;
        result[QStringLiteral("timedOut")] = timedOut;
        if (!exec.target.isEmpty()) {
            result[QStringLiteral("target")] = exec.target;
        }
        if (exec.polluted) {
            // The previous exec on this tab timed out and kept running; its
            // late output may sit inside this capture. Verify side effects
            // independently (read back files/state) before trusting them.
            result[QStringLiteral("warning")] = QStringLiteral(
                "previous exec on this tab timed out; its late output may have "
                "polluted this capture — verify side effects independently");
        }
        if (beginLine < 0) {
            // Begin marker scrolled out of the 10000-row buffer.
            result[QStringLiteral("truncated")] = true;
        }
        if (timedOut) {
            result[QStringLiteral("hint")] = QStringLiteral(
                "the command may still be running, or the tab may be stuck at a "
                "continuation prompt (>) — check the tab buffer (GET text), then "
                "send Ctrl+C (send {\"data\":\"\\u0003\"}) or retry with reset:true");
        }
        respond(op.socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
    }
    if (timedOut) {
        // The command keeps running in the terminal; flag the tab so the
        // NEXT exec carries a pollution warning.
        d->tabExecPolluted.insert(exec.tabIndex);
    }
    finishTabExec(requestId);
}

void AgentHttpServer::respond(QTcpSocket *socket, int status, const QByteArray &body)
{
    const QByteArray statusText = status == 200 ? "OK"
                                  : status == 400 ? "Bad Request"
                                  : status == 401 ? "Unauthorized"
                                  : status == 404 ? "Not Found"
                                                  : "Internal Server Error";
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " " + statusText + "\r\n";
    response += "Content-Type: application/json; charset=utf-8\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Connection: close\r\n";
    response += "Access-Control-Allow-Origin: *\r\n";
    response += "\r\n";
    response += body;
    socket->write(response);
    socket->disconnectFromHost();
}

void AgentHttpServer::respondError(QTcpSocket *socket, int status, const QString &message)
{
    respond(socket, status, QJsonDocument(jsonError(message)).toJson(QJsonDocument::Compact));
}

} // namespace hssh
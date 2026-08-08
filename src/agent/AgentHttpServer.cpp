#include "AgentHttpServer.h"

#include "agent/AgentAudit.h"
#include "agent/AgentSessionRegistry.h"
#include "hssh/Version.h"
#include "utils/Config.h"
#include "utils/Crypto.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QTcpServer>
#include <QTcpSocket>
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

class AgentHttpServer::Impl {
public:
    // One in-flight async operation per request id; detail carries the
    // command/paths for the audit trail (never secrets).
    struct PendingOp {
        QTcpSocket *socket = nullptr;
        QString action;
        QString sessionId;
        QString detail;
    };

    AgentSessionRegistry registry;
    QTcpServer *server = nullptr;
    QHash<QTcpSocket *, QByteArray> buffers;
    QHash<QString, PendingOp> pendingOps; // requestId -> in-flight operation
    AgentTabsInterface *tabs = nullptr;   // GUI tabs provider (GUI mode only)
    int listenPort = 8222;
    QString error;
};

AgentHttpServer::AgentHttpServer(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    connect(&d->registry, &AgentSessionRegistry::execFinished,
            this, &AgentHttpServer::onExecFinished);
    connect(&d->registry, &AgentSessionRegistry::transferFinished,
            this, &AgentHttpServer::onTransferFinished);
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
    d->server->close();
    d->server->deleteLater();
    d->server = nullptr;
    d->registry.closeAll();
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

AgentSessionRegistry *AgentHttpServer::registry() const
{
    return &d->registry;
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

void AgentHttpServer::handleRequest(QTcpSocket *socket, const HttpRequest &request)
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
        body[QStringLiteral("sessions")] = QJsonArray::fromVariantList(d->registry.listSessions());
        respond(socket, 200, QJsonDocument(body).toJson(QJsonDocument::Compact));
        return;
    }

    if (method == "GET" && request.path == "/api/v1/sessions") {
        QJsonArray array = QJsonArray::fromVariantList(d->registry.listSessions());
        respond(socket, 200, QJsonDocument(array).toJson(QJsonDocument::Compact));
        return;
    }

    if (method == "POST" && request.path == "/api/v1/sessions") {
        const QJsonObject body = QJsonDocument::fromJson(request.body).object();

        // Stored-session reference: credentials never cross the API.
        QString sessionRef = body.value(QStringLiteral("sessionName")).toString();
        if (sessionRef.isEmpty()) {
            sessionRef = body.value(QStringLiteral("sessionId")).toString();
        }
        if (!sessionRef.isEmpty()) {
            const QString id = d->registry.createSessionFromStored(sessionRef);
            if (id.isEmpty()) {
                AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("connect"),
                                QStringLiteral("stored=%1").arg(sessionRef),
                                QStringLiteral("error: %1").arg(d->registry.lastError()));
                respondError(socket, 404, d->registry.lastError());
                return;
            }
            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("connect"),
                            QStringLiteral("stored=%1").arg(sessionRef),
                            QStringLiteral("pending id=%1").arg(id));
            QJsonObject result;
            result[QStringLiteral("sessionId")] = id;
            respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
            return;
        }

        // Plaintext passwords are rejected by policy.
        if (body.contains(QStringLiteral("password"))) {
            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("connect"),
                            QStringLiteral("host=%1").arg(body.value(QStringLiteral("host")).toString()),
                            QStringLiteral("rejected: plaintext password"));
            respondError(socket, 400,
                         "Plaintext password rejected; use passwordCipher or sessionName");
            return;
        }

        SessionConfig config;
        config.setName(body.value(QStringLiteral("name")).toString());
        config.setHost(body.value(QStringLiteral("host")).toString());
        config.setPort(body.value(QStringLiteral("port")).toInt(22));
        config.setUsername(body.value(QStringLiteral("username")).toString());

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

        // Password is optional: key/agent auth needs none.
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

        const QString id = d->registry.createSession(config);
        if (id.isEmpty()) {
            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("connect"),
                            QStringLiteral("host=%1 user=%2").arg(config.host(), config.username()),
                            QStringLiteral("error: %1").arg(d->registry.lastError()));
            respondError(socket, 400, d->registry.lastError());
            return;
        }

        AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("connect"),
                        QStringLiteral("host=%1 user=%2 auth=%3 cipher=%4")
                            .arg(config.host(), config.username())
                            .arg(static_cast<int>(config.authMethod()))
                            .arg(cipher.isEmpty() ? QStringLiteral("none")
                                                  : QStringLiteral("<cipher %1 bytes>").arg(cipher.size())),
                        QStringLiteral("pending id=%1").arg(id));
        QJsonObject result;
        result[QStringLiteral("sessionId")] = id;
        respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
        return;
    }

    if (method == "DELETE" && request.path.startsWith(QStringLiteral("/api/v1/sessions/"))) {
        const QString id = request.path.mid(QStringLiteral("/api/v1/sessions/").size());
        if (id.isEmpty() || !d->registry.closeSession(id)) {
            respondError(socket, 404, "Unknown session: " + id);
            return;
        }
        AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("disconnect"),
                        QStringLiteral("session=%1").arg(id), QStringLiteral("ok"));
        QJsonObject result;
        result[QStringLiteral("ok")] = true;
        respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
        return;
    }

    if (method == "POST" && request.path.startsWith(QStringLiteral("/api/v1/sessions/"))
        && request.path.endsWith(QStringLiteral("/exec"))) {
        const QString prefix = QStringLiteral("/api/v1/sessions/");
        const QString suffix = QStringLiteral("/exec");
        const QString id = request.path.mid(prefix.size(), request.path.size() - prefix.size() - suffix.size());
        if (id.isEmpty() || !d->registry.hasSession(id)) {
            respondError(socket, 404, "Unknown session: " + id);
            return;
        }
        const QJsonObject body = QJsonDocument::fromJson(request.body).object();
        const QString command = body.value(QStringLiteral("command")).toString();
        if (command.isEmpty()) {
            respondError(socket, 400, "Missing command");
            return;
        }

        const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        Impl::PendingOp op;
        op.socket = socket;
        op.action = QStringLiteral("exec");
        op.sessionId = id;
        op.detail = QStringLiteral("cmd=\"%1\"").arg(command);
        d->pendingOps[requestId] = op;

        if (!d->registry.exec(id, requestId, command)) {
            d->pendingOps.remove(requestId);
            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("exec"),
                            QStringLiteral("session=%1 cmd=\"%2\"").arg(id, command),
                            QStringLiteral("error: %1").arg(d->registry.lastError()));
            respondError(socket, 500, d->registry.lastError());
        }
        return;
    }

    if (method == "POST"
        && (request.path.endsWith(QStringLiteral("/upload")) || request.path.endsWith(QStringLiteral("/download")))) {
        const QString prefix = QStringLiteral("/api/v1/sessions/");
        const bool isUpload = request.path.endsWith(QStringLiteral("/upload"));
        const QString suffix = isUpload ? QStringLiteral("/upload") : QStringLiteral("/download");
        const QString id = request.path.mid(prefix.size(), request.path.size() - prefix.size() - suffix.size());
        if (id.isEmpty() || !d->registry.hasSession(id)) {
            respondError(socket, 404, "Unknown session: " + id);
            return;
        }
        const QJsonObject body = QJsonDocument::fromJson(request.body).object();
        const QString localPath = body.value(QStringLiteral("localPath")).toString();
        const QString remotePath = body.value(QStringLiteral("remotePath")).toString();
        if (localPath.isEmpty() || remotePath.isEmpty()) {
            respondError(socket, 400, "localPath and remotePath are required");
            return;
        }

        const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        Impl::PendingOp op;
        op.socket = socket;
        op.action = isUpload ? QStringLiteral("upload") : QStringLiteral("download");
        op.sessionId = id;
        op.detail = isUpload ? QStringLiteral("local=%1 remote=%2").arg(localPath, remotePath)
                             : QStringLiteral("remote=%1 local=%2").arg(remotePath, localPath);
        d->pendingOps[requestId] = op;

        const bool ok = isUpload
            ? d->registry.upload(id, requestId, localPath, remotePath)
            : d->registry.download(id, requestId, remotePath, localPath);
        if (!ok) {
            d->pendingOps.remove(requestId);
            AgentAudit::log(AgentAudit::Source::Rest, op.action,
                            QStringLiteral("session=%1 %2").arg(id, op.detail),
                            QStringLiteral("error: %1").arg(d->registry.lastError()));
            respondError(socket, 500, d->registry.lastError());
        }
        return;
    }

    // GUI terminal tabs (available when the agent runs inside the GUI).
    if (method == "GET" && request.path == QStringLiteral("/api/v1/tabs")) {
        if (!d->tabs) {
            respondError(socket, 501, "GUI tabs are only available in the GUI agent");
            return;
        }
        respond(socket, 200, QJsonDocument(QJsonArray::fromVariantList(d->tabs->listTabs())).toJson(QJsonDocument::Compact));
        return;
    }

    if (method == "POST" && request.path == QStringLiteral("/api/v1/tabs")) {
        if (!d->tabs) {
            respondError(socket, 501, "GUI tabs are only available in the GUI agent");
            return;
        }
        const QJsonObject body = QJsonDocument::fromJson(request.body).object();
        const QString sessionName = body.value(QStringLiteral("session")).toString();
        int index = -1;
        if (!sessionName.isEmpty()) {
            index = d->tabs->openSessionTab(sessionName);
        } else {
            const QString shellType = body.value(QStringLiteral("shellType")).toString();
            index = d->tabs->openLocalTab(shellType);
        }
        if (index < 0) {
            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("tab_open"),
                            sessionName.isEmpty() ? QStringLiteral("local")
                                                  : QStringLiteral("session=%1").arg(sessionName),
                            QStringLiteral("error: not found"));
            respondError(socket, 404, sessionName.isEmpty()
                                           ? "Failed to open a local terminal tab"
                                           : "Session not found: " + sessionName);
            return;
        }
        AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("tab_open"),
                        sessionName.isEmpty() ? QStringLiteral("local")
                                              : QStringLiteral("session=%1").arg(sessionName),
                        QStringLiteral("ok index=%1").arg(index));
        QJsonObject result;
        result[QStringLiteral("index")] = index;
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
        bool indexOk = false;
        const int index = indexText.toInt(&indexOk);
        if (!indexOk) {
            respondError(socket, 400, "Invalid tab index: " + indexText);
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
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
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
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
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

        if (method == "POST" && sub == QStringLiteral("sudo")) {
            const QJsonObject body = QJsonDocument::fromJson(request.body).object();
            const QString command = body.value(QStringLiteral("command")).toString();
            if (command.isEmpty()) {
                respondError(socket, 400, "Missing command");
                return;
            }

            // User confirmation gate (per-tab, once per tab lifetime).
            if (!d->tabs->confirmSudo(index, command)) {
                AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("sudo"),
                                QStringLiteral("tab=%1 cmd=\"%2\"").arg(index).arg(command),
                                QStringLiteral("rejected"));
                respondError(socket, 403, "User rejected the sudo request");
                return;
            }

            // Password is optional: cipher, stored credential, or none.
            QString secret;
            const QString cipher = body.value(QStringLiteral("passwordCipher")).toString();
            if (!cipher.isEmpty()) {
                bool ok = false;
                const QByteArray plain = Crypto::rsaDecrypt(cipher.toUtf8(), &ok);
                if (!ok) {
                    respondError(socket, 503, "Failed to decrypt passwordCipher (locked or bad cipher)");
                    return;
                }
                secret = QString::fromUtf8(plain);
            }
            const bool useStored = body.value(QStringLiteral("useStoredCredential")).toBool();
            const int timeoutMs = body.value(QStringLiteral("timeout")).toInt(60000);

            QString output;
            QString errorMessage;
            bool timedOut = false;
            const bool ok = d->tabs->sudoExec(index, command, secret, useStored, timeoutMs,
                                               &output, &timedOut, &errorMessage);
            secret.fill(QLatin1Char('\0'));
            if (!ok) {
                AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("sudo"),
                                QStringLiteral("tab=%1 cmd=\"%2\"").arg(index).arg(command),
                                QStringLiteral("error: %1").arg(errorMessage));
                const int status = errorMessage == QLatin1String("passwordRequired") ? 428 : 500;
                QJsonObject result;
                result[QStringLiteral("error")] = errorMessage;
                if (status == 428) {
                    result[QStringLiteral("passwordRequired")] = true;
                }
                respond(socket, status, QJsonDocument(result).toJson(QJsonDocument::Compact));
                return;
            }
            AgentAudit::log(AgentAudit::Source::Rest, QStringLiteral("sudo"),
                            QStringLiteral("tab=%1 cmd=\"%2\"").arg(index).arg(command),
                            timedOut ? QStringLiteral("ok (timed out)")
                                     : QStringLiteral("ok"));
            QJsonObject result;
            result[QStringLiteral("output")] = output;
            result[QStringLiteral("timedOut")] = timedOut;
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

void AgentHttpServer::onExecFinished(const QString &sessionId, const QString &requestId,
                                     const QString &output, int exitCode, const QString &error)
{
    const auto it = d->pendingOps.find(requestId);
    if (it == d->pendingOps.end()) {
        return;
    }
    const Impl::PendingOp op = it.value();
    d->pendingOps.erase(it);
    QTcpSocket *socket = op.socket;

    AgentAudit::log(AgentAudit::Source::Rest, op.action,
                    QStringLiteral("session=%1 %2").arg(sessionId, op.detail),
                    error.isEmpty() ? QStringLiteral("ok exit=%1").arg(exitCode)
                                    : QStringLiteral("error: %1").arg(error));

    if (!error.isEmpty()) {
        respondError(socket, 500, error);
        return;
    }
    QJsonObject result;
    result[QStringLiteral("sessionId")] = sessionId;
    result[QStringLiteral("output")] = output;
    result[QStringLiteral("exitCode")] = exitCode;
    respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
}

void AgentHttpServer::onTransferFinished(const QString &sessionId, const QString &requestId,
                                         const QString &path, bool ok, const QString &message)
{
    const auto it = d->pendingOps.find(requestId);
    if (it == d->pendingOps.end()) {
        return;
    }
    const Impl::PendingOp op = it.value();
    d->pendingOps.erase(it);
    QTcpSocket *socket = op.socket;

    AgentAudit::log(AgentAudit::Source::Rest, op.action,
                    QStringLiteral("session=%1 %2").arg(sessionId, op.detail),
                    ok ? QStringLiteral("ok") : QStringLiteral("error: %1").arg(message));

    if (!ok) {
        respondError(socket, 500, message);
        return;
    }
    QJsonObject result;
    result[QStringLiteral("sessionId")] = sessionId;
    result[QStringLiteral("path")] = path;
    result[QStringLiteral("ok")] = true;
    respond(socket, 200, QJsonDocument(result).toJson(QJsonDocument::Compact));
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

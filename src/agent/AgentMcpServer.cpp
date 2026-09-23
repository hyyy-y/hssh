#include "AgentMcpServer.h"

#include "agent/AgentAudit.h"
#include "hssh/Version.h"
#include "utils/Config.h"
#include "utils/Crypto.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>

namespace hssh {

namespace {

constexpr const char *kProtocolVersion = "2024-11-05";

QJsonObject makeTextContent(const QString &text)
{
    QJsonObject content;
    content[QStringLiteral("type")] = QStringLiteral("text");
    content[QStringLiteral("text")] = text;
    return content;
}

// Reads newline-delimited JSON from stdin on a dedicated thread (QSocketNotifier
// cannot watch stdin on Windows).
class StdinReader : public QObject {
    Q_OBJECT

public slots:
    void run()
    {
        QTextStream in(stdin);
        QString line;
        while (in.readLineInto(&line)) {
            emit lineRead(line);
        }
        emit eof();
    }

signals:
    void lineRead(const QString &line);
    void eof();
};

} // namespace

class AgentMcpServer::Impl {
public:
    QNetworkAccessManager nam;
    QString baseUrl = QStringLiteral("http://127.0.0.1:8222"); // HSSH_AGENT_URL overrides (test instances)
    bool guiLaunching = false; // one auto-launch at a time
    QThread stdinThread;
    StdinReader *reader = nullptr;
    // B5-3: Http-transport response sink (empty in Stdio mode).
    std::function<void(const QJsonObject &)> messageSink;
};

AgentMcpServer::AgentMcpServer(QObject *parent, Transport transport)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
    // Test isolation: HSSH_AGENT_URL points the bridge at another agent
    // (e.g. a scratch GUI on 8224) instead of the production 8222.
    const QByteArray envUrl = qgetenv("HSSH_AGENT_URL");
    if (!envUrl.isEmpty()) {
        d->baseUrl = QString::fromUtf8(envUrl);
    }
    if (transport == Transport::Http) {
        return; // no stdio: messages arrive via handleIncoming()
    }
    d->reader = new StdinReader();
    d->reader->moveToThread(&d->stdinThread);
    connect(&d->stdinThread, &QThread::started, d->reader, &StdinReader::run);
    connect(d->reader, &StdinReader::lineRead, this, [this](const QString &line) {
        handleMessage(line.toUtf8());
    });
    connect(d->reader, &StdinReader::eof, this, []() {
        qApp->quit();
    });
    connect(&d->stdinThread, &QThread::finished, d->reader, &QObject::deleteLater);
    d->stdinThread.start();
}

AgentMcpServer::~AgentMcpServer()
{
    if (d->stdinThread.isRunning()) {
        d->stdinThread.quit();
        d->stdinThread.wait();
    }
}

void AgentMcpServer::setMessageSink(const std::function<void(const QJsonObject &)> &sink)
{
    d->messageSink = sink;
}

void AgentMcpServer::handleIncoming(const QByteArray &json)
{
    handleMessage(json);
}

// ---------------------------------------------------------------------------
// REST bridge helpers
// ---------------------------------------------------------------------------

// One HTTP call to the GUI agent. cb receives (httpStatus, parsedBody);
// status 0 means the agent was unreachable / the request never completed.
void AgentMcpServer::restCall(const QByteArray &verb, const QString &path,
                              const QJsonObject &body, int timeoutMs,
                              const std::function<void(int, const QJsonObject &)> &cb)
{
    QNetworkRequest request(QUrl(d->baseUrl + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json; charset=utf-8"));
    const QString token = Config::instance().stringValue(QStringLiteral("agent/token"));
    if (!token.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    }
    if (timeoutMs > 0) {
        request.setTransferTimeout(timeoutMs);
    }
    const QByteArray payload = body.isEmpty()
        ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply *reply = nullptr;
    if (verb == "GET") {
        reply = d->nam.get(request);
    } else if (verb == "DELETE") {
        reply = d->nam.deleteResource(request);
    } else {
        reply = d->nam.sendCustomRequest(request, verb, payload);
    }
    connect(reply, &QNetworkReply::finished, reply, [reply, cb]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        reply->deleteLater();
        QJsonObject obj = QJsonDocument::fromJson(data).object();
        if (obj.isEmpty() && !data.trimmed().isEmpty()) {
            obj[QStringLiteral("raw")] = QString::fromUtf8(data);
        }
        cb(status, obj);
    });
}

// Ensures the GUI agent is reachable; auto-launches the GUI with --agent
// when it is not. cb(true) when the agent answers.
void AgentMcpServer::ensureGui(const std::function<void(bool, const QString &)> &cb)
{
    restCall("GET", QStringLiteral("/api/v1/health"), {}, 2000,
             [this, cb](int status, const QJsonObject &) {
        if (status > 0) {
            cb(true, QString());
            return;
        }
        // Any HTTP status counts as alive; 0 = connection refused: launch.
        if (!d->guiLaunching) {
            d->guiLaunching = true;
            AgentAudit::log(AgentAudit::Source::Mcp, QStringLiteral("gui_autolaunch"),
                            QString(), QStringLiteral("starting hssh --agent"));
            QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                    {QStringLiteral("--agent")});
        }
        const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 45000;
        const auto poll = std::make_shared<std::function<void()>>();
        *poll = [this, cb, deadline, poll]() {
            restCall("GET", QStringLiteral("/api/v1/health"), {}, 1500,
                     [this, cb, deadline, poll](int status, const QJsonObject &) {
                if (status > 0) {
                    d->guiLaunching = false;
                    cb(true, QString());
                    return;
                }
                if (QDateTime::currentMSecsSinceEpoch() > deadline) {
                    d->guiLaunching = false;
                    cb(false, QStringLiteral(
                        "The hssh GUI agent did not come up within 45 s. The MCP bridge "
                        "auto-launched 'hssh --agent'; check the hssh window (a master-"
                        "password prompt or a crash would block it)."));
                    return;
                }
                QTimer::singleShot(700, this, *poll);
            });
        };
        QTimer::singleShot(700, this, *poll);
    });
}

// Resolves the "session" tool argument to an OPEN tab ref, auto-opening a
// saved-session tab when nothing matches. On failure the tool error has
// already been answered and cb is not called.
void AgentMcpServer::withTab(const QJsonValue &rpcId, const QJsonObject &arguments,
                             const QString &tool, const QString &auditDetail,
                             bool autoOpen,
                             const std::function<void(const QString &)> &cb)
{
    QString session = arguments.value(QStringLiteral("session")).toString();
    if (session.isEmpty()) {
        // Legacy argument name from the headless-pool days.
        session = arguments.value(QStringLiteral("session_id")).toString();
    }
    if (session.isEmpty()) {
        respondErrorId(rpcId, -32602, QStringLiteral("Missing required argument: session"));
        return;
    }

    const auto findRef = [](const QJsonArray &tabs, const QString &wanted) -> QString {
        for (const QJsonValue &v : tabs) {
            const QJsonObject t = v.toObject();
            const QString ref = t.value(QStringLiteral("ref")).toString();
            if (ref == wanted
                || t.value(QStringLiteral("name")).toString() == wanted
                || t.value(QStringLiteral("host")).toString() == wanted) {
                return ref;
            }
        }
        return QString();
    };

    restCall("GET", QStringLiteral("/api/v2/tabs"), {}, 10000,
             [=, this](int status, const QJsonObject &body) {
        if (status == 0) {
            respondErrorId(rpcId, -32000, QStringLiteral("GUI agent unreachable"));
            return;
        }
        const QJsonArray tabs = body.value(QStringLiteral("tabs")).toArray();
        const QString ref = findRef(tabs, session);
        if (!ref.isEmpty()) {
            // Name-based resolution landing on a STALE tab is the 2026-09-11
            // wrong-machine accident vector: the session was edited after the
            // tab opened, so the name now means a different machine than the
            // tab is connected to. Refuse loudly; an explicit <name>:<ordinal>
            // ref still works (informed consent).
            for (const QJsonValue &v : tabs) {
                const QJsonObject t = v.toObject();
                if (t.value(QStringLiteral("ref")).toString() != ref) {
                    continue;
                }
                if (t.value(QStringLiteral("stale")).toBool() && session == t.value(QStringLiteral("name")).toString()) {
                    respondErrorId(rpcId, -32000,
                                   QStringLiteral("Tab '%1' is STALE: saved session '%2' now "
                                                  "points to %3 but this tab is connected to %4. "
                                                  "Close the tab (ssh_disconnect) and retry, or use "
                                                  "the explicit ref '%1' if the OLD machine is really "
                                                  "what you want.")
                                       .arg(ref, session,
                                            t.value(QStringLiteral("savedTarget")).toString(),
                                            t.value(QStringLiteral("target")).toString()));
                    return;
                }
                // Disconnected tab: heal it instead of failing the tool call.
                if (t.value(QStringLiteral("type")).toString() == QLatin1String("ssh")
                    && !t.value(QStringLiteral("connected")).toBool()) {
                    restCall("POST", QStringLiteral("/api/v1/tabs/%1/reconnect").arg(ref),
                             {}, 15000, [=, this](int rcStatus, const QJsonObject &) {
                        if (rcStatus != 200) {
                            respondErrorId(rpcId, -32000,
                                           QStringLiteral("Tab '%1' is disconnected and the "
                                                          "reconnect was rejected — check the tab "
                                                          "in the hssh window.")
                                               .arg(ref));
                            return;
                        }
                        waitTabConnected(rpcId, ref, 45000,
                                         [cb](const QString &readyRef) { cb(readyRef); });
                    });
                    return;
                }
                break;
            }
            cb(ref);
            return;
        }
        if (!autoOpen) {
            respondErrorId(rpcId, -32000,
                           QStringLiteral("No open tab for '%1' (open tabs: %2).")
                               .arg(session, tabs.isEmpty()
                                    ? QStringLiteral("none")
                                    : QStringLiteral("%1 tabs").arg(tabs.size())));
            return;
        }
        // Auto-open a saved-session tab, then wait for the SSH connect.
        restCall("POST", QStringLiteral("/api/v2/tabs"),
                 QJsonObject{{QStringLiteral("session"), session}}, 15000,
                 [=, this](int openStatus, const QJsonObject &openBody) {
            if (openStatus != 200) {
                respondErrorId(rpcId, -32000,
                               QStringLiteral("Unknown tab '%1' and no saved session with that "
                                              "name (%2). Use list_sessions to see both.")
                                   .arg(session, openBody.value(QStringLiteral("error"))
                                                     .toString(QStringLiteral("HTTP %1").arg(openStatus))));
                return;
            }
            const QString newRef = openBody.value(QStringLiteral("ref")).toString();
            waitTabConnected(rpcId, newRef, 45000,
                             [cb](const QString &readyRef) { cb(readyRef); });
        });
    });
}

// Polls GET /tabs until the tab reports connected (or the deadline passes).
void AgentMcpServer::waitTabConnected(const QJsonValue &rpcId, const QString &ref,
                                      int timeoutMs,
                                      const std::function<void(const QString &)> &cb)
{
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    const auto poll = std::make_shared<std::function<void()>>();
    *poll = [this, rpcId, ref, deadline, timeoutMs, cb, poll]() {
        restCall("GET", QStringLiteral("/api/v2/tabs"), {}, 10000,
                 [=, this](int status, const QJsonObject &body) {
            if (status > 0) {
                for (const QJsonValue &v : body.value(QStringLiteral("tabs")).toArray()) {
                    const QJsonObject t = v.toObject();
                    if (t.value(QStringLiteral("ref")).toString() == ref) {
                        if (t.value(QStringLiteral("connected")).toBool()) {
                            cb(ref);
                            return;
                        }
                        break;
                    }
                    // Tab closed while waiting: fall through to timeout/error.
                }
            }
            if (QDateTime::currentMSecsSinceEpoch() > deadline) {
                respondErrorId(rpcId, -32000,
                               QStringLiteral("Tab '%1' opened but the SSH connect did not "
                                              "finish within %2 s 鈥?check the hssh window "
                                              "(target down, auth failure, or a dialog).")
                                   .arg(ref).arg(timeoutMs / 1000));
                return;
            }
            QTimer::singleShot(600, this, *poll);
        });
    };
    QTimer::singleShot(500, this, *poll);
}

// Maps a REST error body to a JSON-RPC tool error, preserving the exec
// contract fields (partial output/exitCode/timedOut) in error.data.
void AgentMcpServer::respondRestError(const QJsonValue &rpcId, int status,
                                      const QJsonObject &body)
{
    QString message = body.value(QStringLiteral("error")).toString();
    if (message.isEmpty()) {
        message = status == 0
            ? QStringLiteral("GUI agent unreachable / request timed out")
            : QStringLiteral("HTTP %1").arg(status);
    }
    QJsonObject data;
    for (const char *key : {"output", "exitCode", "timedOut", "reason", "passwordRequired"}) {
        if (body.contains(QLatin1String(key))) {
            data[QLatin1String(key)] = body.value(QLatin1String(key));
        }
    }
    respondErrorId(rpcId, -32000, message, data.isEmpty() ? QJsonValue() : QJsonValue(data));
}

// ---------------------------------------------------------------------------
// MCP protocol
// ---------------------------------------------------------------------------

void AgentMcpServer::handleMessage(const QByteArray &line)
{
    const QJsonDocument doc = QJsonDocument::fromJson(line);
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject request = doc.object();

    const QString method = request.value(QStringLiteral("method")).toString();
    const QJsonObject params = request.value(QStringLiteral("params")).toObject();

    if (method == QStringLiteral("initialize")) {
        QJsonObject capabilities;
        QJsonObject tools;
        tools[QStringLiteral("listChanged")] = false;
        capabilities[QStringLiteral("tools")] = tools;

        QJsonObject serverInfo;
        serverInfo[QStringLiteral("name")] = QStringLiteral("hssh");
        serverInfo[QStringLiteral("version")] = QStringLiteral(HSSH_VERSION_STRING);
        serverInfo[QStringLiteral("pid")] = QCoreApplication::applicationPid();

        QJsonObject result;
        result[QStringLiteral("protocolVersion")] = QString::fromLatin1(kProtocolVersion);
        result[QStringLiteral("capabilities")] = capabilities;
        result[QStringLiteral("serverInfo")] = serverInfo;
        respond(request, result);
        return;
    }

    if (method == QStringLiteral("notifications/initialized")
        || method == QStringLiteral("notifications/cancelled")) {
        return; // No response for notifications.
    }

    if (method == QStringLiteral("ping")) {
        respond(request, QJsonObject());
        return;
    }

    if (method == QStringLiteral("tools/list")) {
        QJsonArray tools;
        const auto addTool = [&tools](const QString &name, const QString &description,
                                      const QJsonObject &schema) {
            QJsonObject tool;
            tool[QStringLiteral("name")] = name;
            tool[QStringLiteral("description")] = description;
            tool[QStringLiteral("inputSchema")] = schema;
            tools.append(tool);
        };

        QJsonObject emptySchema;
        emptySchema[QStringLiteral("type")] = QStringLiteral("object");
        emptySchema[QStringLiteral("properties")] = QJsonObject();
        emptySchema[QStringLiteral("additionalProperties")] = false;

        addTool(QStringLiteral("list_sessions"),
                QStringLiteral("List saved sessions and currently open terminal tabs. Everything "
                               "in hssh runs in VISIBLE GUI tabs 鈥?this shows what can be "
                               "connected (savedSessions) and what is already open (tabs, with "
                               "drift-safe refs)."),
                emptySchema);

        QJsonObject connectProperties;
        connectProperties[QStringLiteral("host")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        connectProperties[QStringLiteral("port")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
        connectProperties[QStringLiteral("username")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        connectProperties[QStringLiteral("authMethod")] =
            QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                        {QStringLiteral("enum"), QJsonArray{QStringLiteral("password"),
                                                            QStringLiteral("publickey"),
                                                            QStringLiteral("keyboard-interactive"),
                                                            QStringLiteral("agent")}}};
        connectProperties[QStringLiteral("passwordCipher")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        connectProperties[QStringLiteral("privateKeyPath")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        connectProperties[QStringLiteral("keyPassphraseCipher")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        connectProperties[QStringLiteral("sessionName")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        connectProperties[QStringLiteral("timeout")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
        QJsonObject connectSchema;
        connectSchema[QStringLiteral("type")] = QStringLiteral("object");
        connectSchema[QStringLiteral("properties")] = connectProperties;

        addTool(QStringLiteral("ssh_connect"),
                QStringLiteral("Open a VISIBLE terminal tab in the hssh GUI (by sessionName for a "
                               "saved session, or host+passwordCipher for an ad-hoc connection) "
                               "and return its drift-safe ref. The user watches everything that "
                               "happens in the tab. If the hssh GUI is not running it is "
                               "launched automatically. Safe to retry after timeouts."),
                connectSchema);

        const auto sessionProp = [](QJsonObject &props) {
            props[QStringLiteral("session")] =
                QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                            {QStringLiteral("description"),
                             QStringLiteral("Tab ref or saved-session name (auto-opens when no "
                                            "matching tab is open)")}};
        };

        QJsonObject execProperties;
        sessionProp(execProperties);
        execProperties[QStringLiteral("command")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        execProperties[QStringLiteral("timeout")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
        execProperties[QStringLiteral("reset")] =
            QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
                        {QStringLiteral("description"),
                         QStringLiteral("Send Ctrl+C first and wait 400 ms before typing — "
                                        "clears a stuck continuation prompt (>) or half-typed "
                                        "line; interrupts the foreground program")}};
        QJsonObject execSchema;
        execSchema[QStringLiteral("type")] = QStringLiteral("object");
        execSchema[QStringLiteral("properties")] = execProperties;
        execSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session"),
                                                           QStringLiteral("command")};

        addTool(QStringLiteral("ssh_exec"),
                QStringLiteral("Type a command into the session's VISIBLE terminal tab and return "
                               "its output (timeout in ms, default 120000, 0 = no limit). The "
                               "command runs in the tab's shell exactly as if the user typed it, "
                               "captured between unique markers; on timeout the partial output is "
                               "returned with timedOut:true (the command keeps running 鈥?the user "
                               "can see it). One exec per tab at a time. The tab must sit at a "
                               "shell prompt: input goes to whatever program is in the foreground. "
                               "The response carries `target` (user@host:port of the REAL peer) — "
                               "always check it before destructive operations; a `warning` field "
                               "means the previous exec timed out and this capture may be polluted "
                               "(verify side effects independently)."),
                execSchema);

        QJsonObject disconnectProperties;
        sessionProp(disconnectProperties);
        QJsonObject disconnectSchema;
        disconnectSchema[QStringLiteral("type")] = QStringLiteral("object");
        disconnectSchema[QStringLiteral("properties")] = disconnectProperties;
        disconnectSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session")};

        addTool(QStringLiteral("ssh_disconnect"),
                QStringLiteral("Close the session's terminal tab in the GUI"),
                disconnectSchema);

        QJsonObject forwardProperties;
        sessionProp(forwardProperties);
        forwardProperties[QStringLiteral("type")] =
            QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                        {QStringLiteral("enum"), QJsonArray{QStringLiteral("local"),
                                                            QStringLiteral("remote"),
                                                            QStringLiteral("dynamic")}},
                        {QStringLiteral("description"),
                         QStringLiteral("local: listen locally, forward over SSH; remote: server "
                                        "listens; dynamic: SOCKS5 proxy")}};
        forwardProperties[QStringLiteral("bindAddress")] =
            QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                        {QStringLiteral("description"), QStringLiteral("default 127.0.0.1")}};
        forwardProperties[QStringLiteral("bindPort")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
        forwardProperties[QStringLiteral("targetHost")] =
            QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                        {QStringLiteral("description"),
                         QStringLiteral("required for local/remote (not dynamic)")}};
        forwardProperties[QStringLiteral("targetPort")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
        QJsonObject forwardSchema;
        forwardSchema[QStringLiteral("type")] = QStringLiteral("object");
        forwardSchema[QStringLiteral("properties")] = forwardProperties;
        forwardSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session"),
                                                               QStringLiteral("bindPort")};

        addTool(QStringLiteral("ssh_forward"),
                QStringLiteral("Add a port forward on the session's SSH connection. local: this "
                               "machine listens on bindPort and tunnels to targetHost:targetPort "
                               "through the server; remote: the server listens on bindPort; "
                               "dynamic: SOCKS5 proxy on bindPort. Returns the listen address and "
                               "the full forward list. Use ssh_list_forwards / ssh_remove_forward "
                               "to manage them."),
                forwardSchema);

        QJsonObject listForwardsProperties;
        sessionProp(listForwardsProperties);
        QJsonObject listForwardsSchema;
        listForwardsSchema[QStringLiteral("type")] = QStringLiteral("object");
        listForwardsSchema[QStringLiteral("properties")] = listForwardsProperties;
        listForwardsSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session")};

        addTool(QStringLiteral("ssh_list_forwards"),
                QStringLiteral("List the session's active port forwards "
                               "([{index,type,bindAddress,bindPort,target,active,status}])"),
                listForwardsSchema);

        QJsonObject removeForwardProperties;
        sessionProp(removeForwardProperties);
        removeForwardProperties[QStringLiteral("index")] =
            QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")},
                        {QStringLiteral("description"),
                         QStringLiteral("forward index from ssh_list_forwards")}};
        QJsonObject removeForwardSchema;
        removeForwardSchema[QStringLiteral("type")] = QStringLiteral("object");
        removeForwardSchema[QStringLiteral("properties")] = removeForwardProperties;
        removeForwardSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session"),
                                                                     QStringLiteral("index")};

        addTool(QStringLiteral("ssh_remove_forward"),
                QStringLiteral("Remove one port forward (index from ssh_list_forwards)"),
                removeForwardSchema);

        addTool(QStringLiteral("get_public_key"),
                QStringLiteral("Get the hssh agent RSA public key for encrypting passwords"),
                emptySchema);

        QJsonObject sudoProperties;
        sessionProp(sudoProperties);
        sudoProperties[QStringLiteral("command")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        sudoProperties[QStringLiteral("timeout")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
        QJsonObject sudoSchema;
        sudoSchema[QStringLiteral("type")] = QStringLiteral("object");
        sudoSchema[QStringLiteral("properties")] = sudoProperties;
        sudoSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session"),
                                                           QStringLiteral("command")};

        addTool(QStringLiteral("ssh_sudo"),
                QStringLiteral("Run a command with sudo in the session's visible tab. A dialog in "
                               "the hssh window asks the human user to approve the command (per "
                               "tab, or permanently with 'Always allow'); the sudo password comes "
                               "from the stored credential or is typed by the user into the dialog "
                               "- it is never exposed to the MCP client. timeout in ms, default "
                               "60000, 0 = no limit. Always verify executed + exitCode. IMPORTANT: "
                               "sudo applies to ONE command — `sudo a && b` runs only `a` as root; "
                               "compound commands must be wrapped as sudo bash -c '...' with "
                               "absolute paths, and $PWD / ~ are expanded by the interactive shell "
                               "BEFORE sudo runs (never rely on them inside the command)."),
                sudoSchema);

        QJsonObject uploadProperties;
        sessionProp(uploadProperties);
        uploadProperties[QStringLiteral("local_path")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        uploadProperties[QStringLiteral("remote_path")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        uploadProperties[QStringLiteral("verify")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
        uploadProperties[QStringLiteral("method")] =
            QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                        {QStringLiteral("enum"), QJsonArray{QStringLiteral("sftp"),
                                                            QStringLiteral("scp"),
                                                            QStringLiteral("shell"),
                                                            QStringLiteral("auto")}},
                        {QStringLiteral("description"),
                         QStringLiteral("Transfer method: sftp (default), scp (remote scp "
                                        "binary), shell (base64 over exec channel — works "
                                        "without sftp-server), auto (sftp -> scp -> shell "
                                        "fallback)")}};
        // Shared: return immediately after starting, poll ssh_transfer_status.
        const auto waitProp = [](QJsonObject &props) {
            props[QStringLiteral("wait")] =
                QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")},
                            {QStringLiteral("description"),
                             QStringLiteral("true (default): the tool call returns when the "
                                            "transfer finished. false: returns immediately "
                                            "after starting — use this for large files that "
                                            "would outlive your client's tool timeout, then "
                                            "poll ssh_transfer_status")}};
        };
        waitProp(uploadProperties);

        QJsonObject uploadSchema;
        uploadSchema[QStringLiteral("type")] = QStringLiteral("object");
        uploadSchema[QStringLiteral("properties")] = uploadProperties;
        uploadSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session"),
                                                              QStringLiteral("local_path"),
                                                              QStringLiteral("remote_path")};

        addTool(QStringLiteral("ssh_upload"),
                QStringLiteral("Upload a local file to the session's host over a dedicated "
                               "connection (md5 content verification on by default). method: "
                               "sftp (default) / scp / shell (base64, works without "
                               "sftp-server) / auto (fallback chain). Large files: pass "
                               "wait:false and poll ssh_transfer_status — a client-side tool "
                               "timeout does NOT stop the transfer, it keeps running and the "
                               "local/remote result is only visible via the status tool"),
                uploadSchema);

        QJsonObject downloadProperties;
        sessionProp(downloadProperties);
        downloadProperties[QStringLiteral("remote_path")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        downloadProperties[QStringLiteral("local_path")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        downloadProperties[QStringLiteral("verify")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}};
        downloadProperties[QStringLiteral("method")] = uploadProperties[QStringLiteral("method")];
        waitProp(downloadProperties);
        QJsonObject downloadSchema;
        downloadSchema[QStringLiteral("type")] = QStringLiteral("object");
        downloadSchema[QStringLiteral("properties")] = downloadProperties;
        downloadSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session"),
                                                                {QStringLiteral("remote_path")},
                                                                {QStringLiteral("local_path")}};

        addTool(QStringLiteral("ssh_download"),
                QStringLiteral("Download a remote file from the session's host (md5 content "
                               "verification on by default; a mismatch retries once with a "
                               "full re-download over a fresh connection). method: sftp "
                               "(default) / scp / shell / auto. Bytes stream into "
                               "<local>.part and are renamed atomically on success — a "
                               "timeout/kill never leaves a half file under the real name. "
                               "Large files: pass wait:false and poll ssh_transfer_status; "
                               "a client-side tool timeout does NOT stop the transfer"),
                downloadSchema);

        QJsonObject transferQueryProperties;
        sessionProp(transferQueryProperties);
        QJsonObject transferQuerySchema;
        transferQuerySchema[QStringLiteral("type")] = QStringLiteral("object");
        transferQuerySchema[QStringLiteral("properties")] = transferQueryProperties;
        transferQuerySchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session")};

        addTool(QStringLiteral("ssh_transfer_status"),
                QStringLiteral("Progress of the transfer running on the session's tab: "
                               "{active, direction, method, bytesDone, bytesTotal, elapsedMs}. "
                               "active:false means no transfer (the last one finished — its "
                               "result went to the audit log; on success the file is at its "
                               "final path with md5 verified)"),
                transferQuerySchema);

        addTool(QStringLiteral("ssh_transfer_cancel"),
                QStringLiteral("Cancel the transfer running on the session's tab (keeps the "
                               "local .part file so an sftp retry can resume)"),
                transferQuerySchema);

        QJsonObject sendProperties;
        sessionProp(sendProperties);
        sendProperties[QStringLiteral("data")] =
            QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                        {QStringLiteral("description"),
                         QStringLiteral("Raw input bytes (no auto-Enter); control characters like "
                                        "Ctrl+C are \"\\u0003\"")}};
        QJsonObject sendSchema;
        sendSchema[QStringLiteral("type")] = QStringLiteral("object");
        sendSchema[QStringLiteral("properties")] = sendProperties;
        sendSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session"),
                                                           QStringLiteral("data")};

        addTool(QStringLiteral("ssh_send"),
                QStringLiteral("Send raw input to the session's terminal tab (keystrokes, "
                               "half-typed commands, Ctrl+C ...). Visible on screen."),
                sendSchema);

        QJsonObject readProperties;
        sessionProp(readProperties);
        readProperties[QStringLiteral("lines")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
        readProperties[QStringLiteral("from")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}};
        QJsonObject readSchema;
        readSchema[QStringLiteral("type")] = QStringLiteral("object");
        readSchema[QStringLiteral("properties")] = readProperties;
        readSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session")};

        addTool(QStringLiteral("ssh_read"),
                QStringLiteral("Read the session's terminal buffer (what the user currently sees). "
                               "lines caps the dump (0 = whole buffer), from starts at a content "
                               "row for cursor-style draining."),
                readSchema);

        QJsonObject result;
        result[QStringLiteral("tools")] = tools;
        respond(request, result);
        return;
    }

    if (method == QStringLiteral("tools/call")) {
        const QString name = params.value(QStringLiteral("name")).toString();
        const QJsonObject arguments = params.value(QStringLiteral("arguments")).toObject();
        handleToolCall(name, arguments, request);
        return;
    }

    respondError(request, -32601, QStringLiteral("Method not found: ") + method);
}

void AgentMcpServer::handleToolCall(const QString &name, const QJsonObject &arguments,
                                    const QJsonObject &request)
{
    const QJsonValue rpcId = request.value(QStringLiteral("id"));

    // Audit every invocation; cipher/password values are stripped.
    QString detail;
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (it.key().contains(QLatin1String("cipher"), Qt::CaseInsensitive)
            || it.key() == QLatin1String("password")) {
            detail += QStringLiteral("%1=<cipher> ").arg(it.key());
        } else {
            detail += QStringLiteral("%1=%2 ").arg(it.key(), it.value().toVariant().toString());
        }
    }
    detail = detail.trimmed();
    AgentAudit::log(AgentAudit::Source::Mcp, name, detail, QStringLiteral("invoked"));

    // Local-only tool: the agent keypair is process-local knowledge.
    if (name == QStringLiteral("get_public_key")) {
        const QByteArray pem = Crypto::agentPublicKeyPem();
        if (pem.isEmpty()) {
            respondErrorId(rpcId, -32000, QStringLiteral("Agent keys unavailable (application locked)"));
            return;
        }
        QJsonObject result;
        result[QStringLiteral("publicKeyPem")] = QString::fromUtf8(pem);
        result[QStringLiteral("fingerprint")] = Crypto::agentKeyFingerprint();
        result[QStringLiteral("cipher")] = QStringLiteral("RSA-OAEP-SHA256+base64");
        QJsonArray content;
        content.append(makeTextContent(QStringLiteral("fingerprint: %1\n%2")
                                           .arg(Crypto::agentKeyFingerprint(),
                                                QString::fromUtf8(pem))));
        result[QStringLiteral("content")] = content;
        respondId(rpcId, result);
        return;
    }

    const QJsonObject known = {
        {QStringLiteral("list_sessions"), true}, {QStringLiteral("ssh_connect"), true},
        {QStringLiteral("ssh_exec"), true},     {QStringLiteral("ssh_disconnect"), true},
        {QStringLiteral("ssh_sudo"), true},     {QStringLiteral("ssh_upload"), true},
        {QStringLiteral("ssh_download"), true}, {QStringLiteral("ssh_send"), true},
        {QStringLiteral("ssh_read"), true},     {QStringLiteral("ssh_transfer_status"), true},
        {QStringLiteral("ssh_transfer_cancel"), true},
        {QStringLiteral("ssh_forward"), true},  {QStringLiteral("ssh_list_forwards"), true},
        {QStringLiteral("ssh_remove_forward"), true},
        {QStringLiteral("get_public_key"), true},
    };
    if (!known.contains(name)) {
        respondErrorId(rpcId, -32602, QStringLiteral("Unknown tool: ") + name);
        return;
    }

    // Every other tool bridges to the GUI agent; make sure it is up first.
    ensureGui([this, name, arguments, rpcId, detail](bool up, const QString &error) {
        if (!up) {
            respondErrorId(rpcId, -32000, error);
            return;
        }
        handleBridgedTool(name, arguments, rpcId, detail);
    });
}

void AgentMcpServer::handleBridgedTool(const QString &name, const QJsonObject &arguments,
                                       const QJsonValue &rpcId, const QString &detail)
{
    if (name == QStringLiteral("list_sessions")) {
        restCall("GET", QStringLiteral("/api/v2/saved-sessions"), {}, 10000,
                 [=, this](int s1, const QJsonObject &saved) {
            restCall("GET", QStringLiteral("/api/v2/tabs"), {}, 10000,
                     [=, this](int s2, const QJsonObject &tabs) {
                if (s1 == 0 || s2 == 0) {
                    respondErrorId(rpcId, -32000, QStringLiteral("GUI agent unreachable"));
                    return;
                }
                QJsonObject combined;
                combined[QStringLiteral("savedSessions")] = saved.value(QStringLiteral("savedSessions"));
                combined[QStringLiteral("tabs")] = tabs.value(QStringLiteral("tabs"));
                QJsonArray content;
                content.append(makeTextContent(QString::fromUtf8(
                    QJsonDocument(combined).toJson(QJsonDocument::Compact))));
                QJsonObject result;
                result[QStringLiteral("content")] = content;
                respondId(rpcId, result);
            });
        });
        return;
    }

    if (name == QStringLiteral("ssh_connect")) {
        QString sessionRef = arguments.value(QStringLiteral("sessionName")).toString();
        if (sessionRef.isEmpty()) {
            sessionRef = arguments.value(QStringLiteral("sessionId")).toString();
        }
        QJsonObject body;
        if (!sessionRef.isEmpty()) {
            body[QStringLiteral("session")] = sessionRef;
        } else {
            if (arguments.contains(QStringLiteral("password"))) {
                respondErrorId(rpcId, -32602,
                               QStringLiteral("Plaintext password rejected; use passwordCipher or sessionName"));
                return;
            }
            for (const char *key : {"passwordCipher", "keyPassphraseCipher"}) {
                if (arguments.contains(QLatin1String(key))
                    && arguments.value(QLatin1String(key)).toString().isEmpty()) {
                    respondErrorId(rpcId, -32602,
                                   QStringLiteral("%1 is null/empty; omit the field or provide a cipher")
                                       .arg(QLatin1String(key)));
                    return;
                }
            }
            const QString host = arguments.value(QStringLiteral("host")).toString();
            if (host.isEmpty()) {
                respondErrorId(rpcId, -32602,
                               QStringLiteral("Missing required argument: host or sessionName"));
                return;
            }
            body[QStringLiteral("host")] = host;
            if (arguments.contains(QStringLiteral("port"))) {
                body[QStringLiteral("port")] = arguments.value(QStringLiteral("port")).toInt(22);
            }
            if (!arguments.value(QStringLiteral("username")).toString().isEmpty()) {
                body[QStringLiteral("username")] = arguments.value(QStringLiteral("username")).toString();
            }
            if (!arguments.value(QStringLiteral("authMethod")).toString().isEmpty()) {
                body[QStringLiteral("authMethod")] = arguments.value(QStringLiteral("authMethod")).toString();
            }
            for (const char *key : {"passwordCipher", "privateKeyPath", "keyPassphraseCipher"}) {
                if (!arguments.value(QLatin1String(key)).toString().isEmpty()) {
                    body[QLatin1String(key)] = arguments.value(QLatin1String(key)).toString();
                }
            }
        }
        // Ciphers pass through untouched: the GUI process decrypts them.
        restCall("POST", QStringLiteral("/api/v2/tabs"), body, 15000,
                 [=, this](int status, const QJsonObject &openBody) {
            if (status != 200) {
                respondRestError(rpcId, status, openBody);
                return;
            }
            const QString ref = openBody.value(QStringLiteral("ref")).toString();
            const int timeoutMs = arguments.value(QStringLiteral("timeout")).toInt(45000);
            waitTabConnected(rpcId, ref, timeoutMs, [=, this](const QString &readyRef) {
                AgentAudit::log(AgentAudit::Source::Mcp, name, detail,
                                QStringLiteral("ok ref=%1").arg(readyRef));
                QJsonObject result;
                result[QStringLiteral("ref")] = readyRef;
                result[QStringLiteral("connected")] = true;
                result[QStringLiteral("visible")] = true;
                // Which machine the tab really points at (live peer when
                // known) — the caller's first chance to catch a wrong target.
                if (!openBody.value(QStringLiteral("target")).toString().isEmpty()) {
                    result[QStringLiteral("target")] = openBody.value(QStringLiteral("target"));
                }
                // The saved session was edited since a same-named tab
                // existed: surface the drift immediately.
                if (openBody.value(QStringLiteral("stale")).toBool()) {
                    result[QStringLiteral("stale")] = true;
                    result[QStringLiteral("savedTarget")] =
                        openBody.value(QStringLiteral("savedTarget"));
                }
                QString text = QStringLiteral("connected: %1 (%2), visible tab")
                                   .arg(readyRef,
                                        openBody.value(QStringLiteral("target")).toString());
                if (result.contains(QStringLiteral("stale"))) {
                    text += QStringLiteral("\nWARNING: saved session now points to %1")
                                .arg(openBody.value(QStringLiteral("savedTarget")).toString());
                }
                QJsonArray content;
                content.append(makeTextContent(text));
                result[QStringLiteral("content")] = content;
                respondId(rpcId, result);
            });
        });
        return;
    }

    // Tab-scoped tools: resolve the session argument to an open tab ref
    // (auto-opening saved sessions), then call the tab endpoint.
    const bool autoOpen = name != QStringLiteral("ssh_disconnect");
    withTab(rpcId, arguments, name, detail, autoOpen,
            [=, this](const QString &ref) {
        const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(ref));
        if (name == QStringLiteral("ssh_exec")) {
            const QString command = arguments.value(QStringLiteral("command")).toString();
            if (command.isEmpty()) {
                respondErrorId(rpcId, -32602, QStringLiteral("Missing required argument: command"));
                return;
            }
            const int timeoutMs = arguments.contains(QStringLiteral("timeout"))
                                      ? arguments.value(QStringLiteral("timeout")).toInt(0)
                                      : 120000;
            QJsonObject body{{QStringLiteral("command"), command},
                             {QStringLiteral("timeout"), timeoutMs}};
            if (arguments.value(QStringLiteral("reset")).toBool(false)) {
                body[QStringLiteral("reset")] = true;
            }
            // The REST answer arrives when the command finishes; the HTTP
            // transfer timeout must outlive the tool timeout.
            const int httpTimeout = timeoutMs <= 0 ? 24 * 3600 * 1000 : timeoutMs + 20000;
            restCall("POST", QStringLiteral("/api/v1/tabs/%1/exec").arg(encoded), body,
                     httpTimeout, [=, this](int status, const QJsonObject &execBody) {
                if (status != 200) {
                    respondRestError(rpcId, status, execBody);
                    return;
                }
                AgentAudit::log(AgentAudit::Source::Mcp, name, detail,
                                QStringLiteral("ok exit=%1")
                                    .arg(execBody.value(QStringLiteral("exitCode")).toInt()));
                QJsonObject result;
                const QString output = execBody.value(QStringLiteral("output")).toString();
                const int exitCode = execBody.value(QStringLiteral("exitCode")).toInt();
                const bool timedOut = execBody.value(QStringLiteral("timedOut")).toBool();
                result[QStringLiteral("output")] = output;
                result[QStringLiteral("exitCode")] = exitCode;
                result[QStringLiteral("timedOut")] = timedOut;
                if (!execBody.value(QStringLiteral("target")).toString().isEmpty()) {
                    // Which machine actually ran it — verify this on any
                    // destructive operation (2026-09-11 wrong-machine lesson).
                    result[QStringLiteral("target")] = execBody.value(QStringLiteral("target"));
                }
                if (execBody.value(QStringLiteral("truncated")).toBool()) {
                    result[QStringLiteral("truncated")] = true;
                }
                if (execBody.contains(QStringLiteral("warning"))) {
                    result[QStringLiteral("warning")] = execBody.value(QStringLiteral("warning"));
                }
                if (execBody.contains(QStringLiteral("hint"))) {
                    result[QStringLiteral("hint")] = execBody.value(QStringLiteral("hint"));
                }
                // MCP clients read the content array as the tool's text
                // output — without it the result looks empty (2026-09-14).
                QString text = output;
                if (timedOut) {
                    text = QStringLiteral("[timed out — partial output]\n") + text;
                }
                if (!execBody.value(QStringLiteral("target")).toString().isEmpty()) {
                    text += QStringLiteral("\n[exit %1 @ %2]")
                                .arg(exitCode)
                                .arg(execBody.value(QStringLiteral("target")).toString());
                } else {
                    text += QStringLiteral("\n[exit %1]").arg(exitCode);
                }
                if (result.contains(QStringLiteral("warning"))) {
                    text += QStringLiteral("\n[warning] %1")
                                .arg(result.value(QStringLiteral("warning")).toString());
                }
                QJsonArray content;
                content.append(makeTextContent(text));
                result[QStringLiteral("content")] = content;
                respondId(rpcId, result);
            });
            return;
        }

        if (name == QStringLiteral("ssh_sudo")) {
            const QString command = arguments.value(QStringLiteral("command")).toString();
            if (command.isEmpty()) {
                respondErrorId(rpcId, -32602, QStringLiteral("Missing required argument: command"));
                return;
            }
            const int timeoutMs = arguments.contains(QStringLiteral("timeout"))
                                      ? arguments.value(QStringLiteral("timeout")).toInt(0)
                                      : 60000;
            // The GUI consent dialog auto-rejects after ~30 s of no answer;
            // the HTTP wait must cover dialog + execution.
            const int httpTimeout = timeoutMs <= 0 ? 24 * 3600 * 1000 : timeoutMs + 45000;
            restCall("POST", QStringLiteral("/api/v1/tabs/%1/sudo").arg(encoded),
                     QJsonObject{{QStringLiteral("command"), command},
                                 {QStringLiteral("timeout"), timeoutMs},
                                 {QStringLiteral("useStoredCredential"), true}},
                     httpTimeout, [=, this](int status, const QJsonObject &sudoBody) {
                if (status != 200) {
                    respondRestError(rpcId, status, sudoBody);
                    return;
                }
                const int exitCode = sudoBody.value(QStringLiteral("exitCode")).toInt(-1);
                const QString output = sudoBody.value(QStringLiteral("output")).toString();
                AgentAudit::log(AgentAudit::Source::Mcp, name, detail,
                                QStringLiteral("ok exit=%1").arg(exitCode));
                QJsonArray content;
                content.append(makeTextContent(QStringLiteral("exit %1\n%2").arg(exitCode).arg(output)));
                QJsonObject result;
                result[QStringLiteral("content")] = content;
                result[QStringLiteral("output")] = output;
                result[QStringLiteral("exitCode")] = exitCode;
                result[QStringLiteral("executed")] = sudoBody.value(QStringLiteral("executed"));
                result[QStringLiteral("timedOut")] = sudoBody.value(QStringLiteral("timedOut"));
                if (!sudoBody.value(QStringLiteral("target")).toString().isEmpty()) {
                    result[QStringLiteral("target")] = sudoBody.value(QStringLiteral("target"));
                }
                respondId(rpcId, result);
            });
            return;
        }

        if (name == QStringLiteral("ssh_forward")) {
            const QString type = arguments.value(QStringLiteral("type")).toString(
                QStringLiteral("local"));
            QJsonObject body{{QStringLiteral("type"), type},
                             {QStringLiteral("bindPort"),
                              arguments.value(QStringLiteral("bindPort")).toInt()}};
            if (!arguments.value(QStringLiteral("bindAddress")).toString().isEmpty()) {
                body[QStringLiteral("bindAddress")] =
                    arguments.value(QStringLiteral("bindAddress")).toString();
            }
            if (!arguments.value(QStringLiteral("targetHost")).toString().isEmpty()) {
                body[QStringLiteral("targetHost")] =
                    arguments.value(QStringLiteral("targetHost")).toString();
            }
            if (arguments.contains(QStringLiteral("targetPort"))) {
                body[QStringLiteral("targetPort")] =
                    arguments.value(QStringLiteral("targetPort")).toInt();
            }
            restCall("POST", QStringLiteral("/api/v1/tabs/%1/forward").arg(encoded), body,
                     15000, [=, this](int status, const QJsonObject &fwdBody) {
                if (status != 200) {
                    respondRestError(rpcId, status, fwdBody);
                    return;
                }
                AgentAudit::log(AgentAudit::Source::Mcp, name, detail,
                                QStringLiteral("ok listen=%1")
                                    .arg(fwdBody.value(QStringLiteral("listen")).toString()));
                QJsonObject result;
                result[QStringLiteral("ok")] = true;
                result[QStringLiteral("listen")] = fwdBody.value(QStringLiteral("listen"));
                result[QStringLiteral("forwards")] = fwdBody.value(QStringLiteral("forwards"));
                if (!fwdBody.value(QStringLiteral("target")).toString().isEmpty()) {
                    result[QStringLiteral("target")] = fwdBody.value(QStringLiteral("target"));
                }
                const QString text = QStringLiteral(
                    "forwarding %1 (%2) on %3")
                    .arg(fwdBody.value(QStringLiteral("listen")).toString(), type,
                         fwdBody.value(QStringLiteral("target")).toString());
                QJsonArray content;
                content.append(makeTextContent(text));
                result[QStringLiteral("content")] = content;
                respondId(rpcId, result);
            });
            return;
        }

        if (name == QStringLiteral("ssh_list_forwards")) {
            restCall("GET", QStringLiteral("/api/v1/tabs/%1/forward").arg(encoded), QJsonObject(),
                     15000, [=, this](int status, const QJsonObject &fwdBody) {
                if (status != 200) {
                    respondRestError(rpcId, status, fwdBody);
                    return;
                }
                const QJsonArray forwards = fwdBody.value(QStringLiteral("forwards")).toArray();
                AgentAudit::log(AgentAudit::Source::Mcp, name, detail,
                                QStringLiteral("ok count=%1").arg(forwards.size()));
                QStringList lines;
                for (const QJsonValue &v : forwards) {
                    const QJsonObject f = v.toObject();
                    lines.append(QStringLiteral("#%1 %2 %3:%4 -> %5 [%6]")
                                     .arg(f.value(QStringLiteral("index")).toInt())
                                     .arg(f.value(QStringLiteral("type")).toString(),
                                          f.value(QStringLiteral("bindAddress")).toString())
                                     .arg(f.value(QStringLiteral("bindPort")).toInt())
                                     .arg(f.value(QStringLiteral("target")).toString(
                                          QStringLiteral("(socks)")))
                                     .arg(f.value(QStringLiteral("status")).toString()));
                }
                const QString text = forwards.isEmpty()
                    ? QStringLiteral("no forwards on %1")
                          .arg(fwdBody.value(QStringLiteral("target")).toString())
                    : lines.join(QLatin1Char('\n'));
                QJsonObject result;
                result[QStringLiteral("forwards")] = forwards;
                QJsonArray content;
                content.append(makeTextContent(text));
                result[QStringLiteral("content")] = content;
                respondId(rpcId, result);
            });
            return;
        }

        if (name == QStringLiteral("ssh_remove_forward")) {
            const int forwardIndex = arguments.value(QStringLiteral("index")).toInt(-1);
            if (forwardIndex < 0) {
                respondErrorId(rpcId, -32602,
                               QStringLiteral("Missing required argument: index (>= 0)"));
                return;
            }
            restCall("DELETE",
                     QStringLiteral("/api/v1/tabs/%1/forward?index=%2").arg(encoded).arg(forwardIndex),
                     QJsonObject(), 15000, [=, this](int status, const QJsonObject &fwdBody) {
                if (status != 200) {
                    respondRestError(rpcId, status, fwdBody);
                    return;
                }
                AgentAudit::log(AgentAudit::Source::Mcp, name, detail, QStringLiteral("ok"));
                QJsonObject result;
                result[QStringLiteral("ok")] = true;
                QJsonArray content;
                content.append(makeTextContent(QStringLiteral("removed forward #%1").arg(forwardIndex)));
                result[QStringLiteral("content")] = content;
                respondId(rpcId, result);
            });
            return;
        }

        if (name == QStringLiteral("ssh_upload") || name == QStringLiteral("ssh_download")) {
            const bool isUpload = name == QStringLiteral("ssh_upload");
            const QString localPath = arguments.value(QStringLiteral("local_path")).toString();
            const QString remotePath = arguments.value(QStringLiteral("remote_path")).toString();
            if (localPath.isEmpty() || remotePath.isEmpty()) {
                respondErrorId(rpcId, -32602,
                               QStringLiteral("Missing required argument: local_path or remote_path"));
                return;
            }
            QJsonObject body{{QStringLiteral("localPath"), localPath},
                             {QStringLiteral("remotePath"), remotePath}};
            // verify applies to both directions (download verifies too).
            if (arguments.contains(QStringLiteral("verify"))) {
                body[QStringLiteral("verify")] = arguments.value(QStringLiteral("verify")).toBool(true);
            }
            const QString transferMethod = arguments.value(QStringLiteral("method")).toString();
            if (!transferMethod.isEmpty()) {
                body[QStringLiteral("method")] = transferMethod;
            }
            // wait:false -> start the transfer and return immediately; the
            // caller then polls ssh_transfer_status. The default (wait) holds
            // this call until completion — fine unless the MCP client imposes
            // a shorter tool timeout, in which case the transfer keeps
            // running server-side anyway (2026-09-18 zombie lesson).
            const bool wait = arguments.value(QStringLiteral("wait")).toBool(true);
            const int httpTimeout = wait ? 24 * 3600 * 1000 : 30000;
            if (!wait) {
                body[QStringLiteral("async")] = true;
            }
            restCall("POST", QStringLiteral("/api/v1/tabs/%1/%2").arg(encoded, isUpload ? QStringLiteral("upload")
                                                                                        : QStringLiteral("download")),
                     body, httpTimeout, [=, this](int status, const QJsonObject &transferBody) {
                if (status != 200) {
                    respondRestError(rpcId, status, transferBody);
                    return;
                }
                QJsonObject result;
                QJsonArray content;
                if (transferBody.value(QStringLiteral("async")).toBool()) {
                    AgentAudit::log(AgentAudit::Source::Mcp, name, detail,
                                    QStringLiteral("started async"));
                    result[QStringLiteral("started")] = true;
                    result[QStringLiteral("method")] = transferBody.value(QStringLiteral("method"));
                    result[QStringLiteral("direction")] =
                        transferBody.value(QStringLiteral("direction"));
                    result[QStringLiteral("target")] = transferBody.value(QStringLiteral("target"));
                    content.append(makeTextContent(
                        QStringLiteral("transfer started (%1 %2, method %3) — poll "
                                       "ssh_transfer_status; the file lands at its final "
                                       "path only after md5-verified success")
                            .arg(transferBody.value(QStringLiteral("direction")).toString(),
                                 transferBody.value(QStringLiteral("target")).toString(),
                                 transferBody.value(QStringLiteral("method")).toString())));
                } else {
                    AgentAudit::log(AgentAudit::Source::Mcp, name, detail, QStringLiteral("ok"));
                    result[QStringLiteral("path")] = transferBody.value(QStringLiteral("path"));
                    if (!transferBody.value(QStringLiteral("message")).toString().isEmpty()) {
                        result[QStringLiteral("message")] =
                            transferBody.value(QStringLiteral("message"));
                    }
                    content.append(makeTextContent(
                        QStringLiteral("transferred: %1")
                            .arg(transferBody.value(QStringLiteral("path")).toString())));
                }
                result[QStringLiteral("content")] = content;
                respondId(rpcId, result);
            });
            return;
        }

        if (name == QStringLiteral("ssh_transfer_status")) {
            restCall("GET", QStringLiteral("/api/v1/tabs/%1/transfer").arg(encoded), {}, 10000,
                     [=, this](int status, const QJsonObject &st) {
                if (status != 200) {
                    respondRestError(rpcId, status, st);
                    return;
                }
                QJsonObject result = st;
                QString text;
                if (!st.value(QStringLiteral("active")).toBool()) {
                    text = QStringLiteral("no transfer running on this tab "
                                          "(the last one finished; success = file at final path)");
                } else {
                    const qint64 done = st.value(QStringLiteral("bytesDone")).toInteger();
                    const qint64 total = st.value(QStringLiteral("bytesTotal")).toInteger();
                    const qint64 elapsed = st.value(QStringLiteral("elapsedMs")).toInteger();
                    text = QStringLiteral("%1 via %2: %3 of %4 bytes (%5 s elapsed)")
                               .arg(st.value(QStringLiteral("direction")).toString(),
                                    st.value(QStringLiteral("method")).toString())
                               .arg(done).arg(total).arg(elapsed / 1000);
                }
                QJsonArray content;
                content.append(makeTextContent(text));
                result[QStringLiteral("content")] = content;
                respondId(rpcId, result);
            });
            return;
        }

        if (name == QStringLiteral("ssh_transfer_cancel")) {
            restCall("DELETE", QStringLiteral("/api/v1/tabs/%1/transfer").arg(encoded), {}, 10000,
                     [=, this](int status, const QJsonObject &del) {
                if (status != 200) {
                    respondRestError(rpcId, status, del);
                    return;
                }
                AgentAudit::log(AgentAudit::Source::Mcp, name, detail, QStringLiteral("ok"));
                QJsonObject result;
                result[QStringLiteral("cancelled")] = true;
                QJsonArray content;
                content.append(makeTextContent(
                    QStringLiteral("transfer cancelled; the local .part file is kept "
                                   "for resume (sftp method)")));
                result[QStringLiteral("content")] = content;
                respondId(rpcId, result);
            });
            return;
        }

        if (name == QStringLiteral("ssh_send")) {
            const QString data = arguments.value(QStringLiteral("data")).toString();
            if (data.isEmpty()) {
                respondErrorId(rpcId, -32602, QStringLiteral("Missing required argument: data"));
                return;
            }
            restCall("POST", QStringLiteral("/api/v1/tabs/%1/send").arg(encoded),
                     QJsonObject{{QStringLiteral("data"), data}}, 10000,
                     [=, this](int status, const QJsonObject &sendBody) {
                if (status != 200) {
                    respondRestError(rpcId, status, sendBody);
                    return;
                }
                QJsonObject result;
                result[QStringLiteral("ok")] = true;
                QJsonArray content;
                content.append(makeTextContent(QStringLiteral("sent %1 bytes")
                                                   .arg(data.toUtf8().size())));
                result[QStringLiteral("content")] = content;
                respondId(rpcId, result);
            });
            return;
        }

        if (name == QStringLiteral("ssh_read")) {
            QString path = QStringLiteral("/api/v1/tabs/%1/text").arg(encoded);
            QStringList query;
            if (arguments.contains(QStringLiteral("lines"))) {
                query.append(QStringLiteral("lines=%1")
                                 .arg(arguments.value(QStringLiteral("lines")).toInt()));
            }
            if (arguments.contains(QStringLiteral("from"))) {
                query.append(QStringLiteral("from=%1")
                                 .arg(arguments.value(QStringLiteral("from")).toInt()));
            }
            if (!query.isEmpty()) {
                path += QLatin1Char('?') + query.join(QLatin1Char('&'));
            }
            restCall("GET", path, {}, 10000, [=, this](int status, const QJsonObject &textBody) {
                if (status != 200) {
                    respondRestError(rpcId, status, textBody);
                    return;
                }
                QJsonObject result;
                result[QStringLiteral("text")] = textBody.value(QStringLiteral("text"));
                result[QStringLiteral("lines")] = textBody.value(QStringLiteral("lines"));
                QJsonArray content;
                content.append(makeTextContent(textBody.value(QStringLiteral("text")).toString()));
                result[QStringLiteral("content")] = content;
                respondId(rpcId, result);
            });
            return;
        }

        if (name == QStringLiteral("ssh_disconnect")) {
            restCall("DELETE", QStringLiteral("/api/v1/tabs/%1").arg(encoded), {}, 10000,
                     [=, this](int status, const QJsonObject &delBody) {
                if (status != 200) {
                    respondRestError(rpcId, status, delBody);
                    return;
                }
                AgentAudit::log(AgentAudit::Source::Mcp, name, detail, QStringLiteral("ok"));
                QJsonArray content;
                content.append(makeTextContent(QStringLiteral("tab closed")));
                QJsonObject result;
                result[QStringLiteral("content")] = content;
                respondId(rpcId, result);
            });
            return;
        }
    });
}

void AgentMcpServer::respond(const QJsonObject &request, const QJsonValue &result)
{
    respondId(request.value(QStringLiteral("id")), result);
}

void AgentMcpServer::respondError(const QJsonObject &request, int code, const QString &message)
{
    respondErrorId(request.value(QStringLiteral("id")), code, message);
}

void AgentMcpServer::respondId(const QJsonValue &id, const QJsonValue &result)
{
    QJsonObject message;
    message[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    message[QStringLiteral("id")] = id;
    message[QStringLiteral("result")] = result;
    sendMessage(message);
}

void AgentMcpServer::respondErrorId(const QJsonValue &id, int code, const QString &message,
                                    const QJsonValue &data)
{
    QJsonObject error;
    error[QStringLiteral("code")] = code;
    error[QStringLiteral("message")] = message;
    if (!data.isUndefined()) {
        error[QStringLiteral("data")] = data;
    }
    QJsonObject response;
    response[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    response[QStringLiteral("id")] = id;
    response[QStringLiteral("error")] = error;
    sendMessage(response);
}

void AgentMcpServer::sendMessage(const QJsonObject &message)
{
    if (d->messageSink) {
        d->messageSink(message);
        return;
    }
    QTextStream out(stdout);
    out << QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)) << '\n';
    out.flush();
}

} // namespace hssh

#include "AgentMcpServer.moc"

#include "AgentMcpServer.h"

#include "agent/AgentAudit.h"
#include "agent/AgentSessionRegistry.h"
#include "hssh/Version.h"
#include "utils/Crypto.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTextStream>
#include <QThread>

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
    AgentSessionRegistry registry;
    QThread stdinThread;
    StdinReader *reader = nullptr;
    // requestId -> tool name, for asynchronous tool results.
    QHash<QString, QString> pendingTools;
    // requestId -> audit detail (command/paths; never secrets).
    QHash<QString, QString> pendingDetails;
};

AgentMcpServer::AgentMcpServer(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Impl>())
{
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

    connect(&d->registry, &AgentSessionRegistry::connected, this, [this](const QString &sessionId) {
        for (auto it = d->pendingTools.begin(); it != d->pendingTools.end();) {
            if (it.value() != QStringLiteral("ssh_connect")) {
                ++it;
                continue;
            }
            const QString requestId = it.key();
            it = d->pendingTools.erase(it);
            AgentAudit::log(AgentAudit::Source::Mcp, QStringLiteral("ssh_connect"),
                            d->pendingDetails.take(requestId),
                            QStringLiteral("ok id=%1").arg(sessionId));

            QJsonObject result;
            result[QStringLiteral("sessionId")] = sessionId;
            QJsonObject request;
            request[QStringLiteral("id")] = requestId;
            respond(request, result);
        }
    });

    connect(&d->registry, &AgentSessionRegistry::connectFailed, this, [this](const QString &sessionId,
                                                                             const QString &error) {
        for (auto it = d->pendingTools.begin(); it != d->pendingTools.end();) {
            if (it.value() != QStringLiteral("ssh_connect")) {
                ++it;
                continue;
            }
            const QString requestId = it.key();
            it = d->pendingTools.erase(it);
            AgentAudit::log(AgentAudit::Source::Mcp, QStringLiteral("ssh_connect"),
                            d->pendingDetails.take(requestId),
                            QStringLiteral("error: %1").arg(error));
            QJsonObject request;
            request[QStringLiteral("id")] = requestId;
            respondError(request, -32000, error);
        }
        Q_UNUSED(sessionId)
    });

    connect(&d->registry, &AgentSessionRegistry::execFinished, this,
            [this](const QString &sessionId, const QString &requestId,
                   const QString &output, int exitCode, const QString &error) {
                Q_UNUSED(sessionId)
                if (!d->pendingTools.contains(requestId)) {
                    return;
                }
                d->pendingTools.remove(requestId);
                AgentAudit::log(AgentAudit::Source::Mcp, QStringLiteral("ssh_exec"),
                                d->pendingDetails.take(requestId),
                                error.isEmpty() ? QStringLiteral("ok exit=%1").arg(exitCode)
                                                : QStringLiteral("error: %1").arg(error));
                QJsonObject request;
                request[QStringLiteral("id")] = requestId;
                if (!error.isEmpty()) {
                    respondError(request, -32000, error);
                    return;
                }
                QJsonObject result;
                result[QStringLiteral("output")] = output;
                result[QStringLiteral("exitCode")] = exitCode;
                respond(request, result);
            });

    connect(&d->registry, &AgentSessionRegistry::transferFinished, this,
            [this](const QString &sessionId, const QString &requestId,
                   const QString &path, bool ok, const QString &message) {
                Q_UNUSED(sessionId)
                if (!d->pendingTools.contains(requestId)) {
                    return;
                }
                const QString tool = d->pendingTools.take(requestId);
                AgentAudit::log(AgentAudit::Source::Mcp, tool,
                                d->pendingDetails.take(requestId),
                                ok ? QStringLiteral("ok") : QStringLiteral("error: %1").arg(message));
                QJsonObject request;
                request[QStringLiteral("id")] = requestId;
                if (!ok) {
                    respondError(request, -32000, message);
                    return;
                }
                QJsonArray content;
                content.append(makeTextContent(QStringLiteral("transferred: %1").arg(path)));
                QJsonObject result;
                result[QStringLiteral("content")] = content;
                result[QStringLiteral("path")] = path;
                respond(request, result);
            });
}

AgentMcpServer::~AgentMcpServer()
{
    d->stdinThread.quit();
    d->stdinThread.wait();
}

AgentSessionRegistry *AgentMcpServer::registry() const
{
    return &d->registry;
}

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

        QJsonObject sessionSchema;
        sessionSchema[QStringLiteral("type")] = QStringLiteral("object");
        sessionSchema[QStringLiteral("properties")] = QJsonObject();
        sessionSchema[QStringLiteral("additionalProperties")] = false;

        addTool(QStringLiteral("list_sessions"),
                QStringLiteral("List the SSH sessions currently managed by hssh"),
                sessionSchema);

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
        connectProperties[QStringLiteral("sessionId")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        QJsonObject connectSchema;
        connectSchema[QStringLiteral("type")] = QStringLiteral("object");
        connectSchema[QStringLiteral("properties")] = connectProperties;

        addTool(QStringLiteral("ssh_connect"),
                QStringLiteral("Connect to a host (by sessionName/sessionId for stored credentials, "
                               "or host+passwordCipher) and return a session id"),
                connectSchema);

        QJsonObject execProperties;
        execProperties[QStringLiteral("session_id")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        execProperties[QStringLiteral("command")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        QJsonObject execSchema;
        execSchema[QStringLiteral("type")] = QStringLiteral("object");
        execSchema[QStringLiteral("properties")] = execProperties;
        execSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session_id"),
                                                           QStringLiteral("command")};

        addTool(QStringLiteral("ssh_exec"),
                QStringLiteral("Execute a command on a connected session and return its output"),
                execSchema);

        QJsonObject disconnectProperties;
        disconnectProperties[QStringLiteral("session_id")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        QJsonObject disconnectSchema;
        disconnectSchema[QStringLiteral("type")] = QStringLiteral("object");
        disconnectSchema[QStringLiteral("properties")] = disconnectProperties;
        disconnectSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session_id")};

        addTool(QStringLiteral("ssh_disconnect"),
                QStringLiteral("Disconnect and release an SSH session"),
                disconnectSchema);

        QJsonObject publicKeySchema;
        publicKeySchema[QStringLiteral("type")] = QStringLiteral("object");
        publicKeySchema[QStringLiteral("properties")] = QJsonObject();
        publicKeySchema[QStringLiteral("additionalProperties")] = false;
        addTool(QStringLiteral("get_public_key"),
                QStringLiteral("Get the hssh agent RSA public key for encrypting passwords"),
                publicKeySchema);

        QJsonObject sudoProperties;
        sudoProperties[QStringLiteral("session_id")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        sudoProperties[QStringLiteral("command")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        sudoProperties[QStringLiteral("passwordCipher")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        QJsonObject sudoSchema;
        sudoSchema[QStringLiteral("type")] = QStringLiteral("object");
        sudoSchema[QStringLiteral("properties")] = sudoProperties;
        sudoSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session_id"),
                                                           QStringLiteral("command")};

        addTool(QStringLiteral("ssh_sudo"),
                QStringLiteral("Run a sudo command (requires GUI user confirmation)"),
                sudoSchema);

        QJsonObject uploadProperties;
        uploadProperties[QStringLiteral("session_id")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        uploadProperties[QStringLiteral("local_path")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        uploadProperties[QStringLiteral("remote_path")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        QJsonObject uploadSchema;
        uploadSchema[QStringLiteral("type")] = QStringLiteral("object");
        uploadSchema[QStringLiteral("properties")] = uploadProperties;
        uploadSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session_id"),
                                                              QStringLiteral("local_path"),
                                                              QStringLiteral("remote_path")};

        addTool(QStringLiteral("ssh_upload"),
                QStringLiteral("Upload a local file to the remote host (SFTP)"),
                uploadSchema);

        QJsonObject downloadProperties;
        downloadProperties[QStringLiteral("session_id")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        downloadProperties[QStringLiteral("remote_path")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        downloadProperties[QStringLiteral("local_path")] = QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}};
        QJsonObject downloadSchema;
        downloadSchema[QStringLiteral("type")] = QStringLiteral("object");
        downloadSchema[QStringLiteral("properties")] = downloadProperties;
        downloadSchema[QStringLiteral("required")] = QJsonArray{QStringLiteral("session_id"),
                                                                QStringLiteral("remote_path"),
                                                                QStringLiteral("local_path")};

        addTool(QStringLiteral("ssh_download"),
                QStringLiteral("Download a remote file to the local machine (SFTP)"),
                downloadSchema);

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

    if (name == QStringLiteral("list_sessions")) {
        QJsonArray content;
        content.append(makeTextContent(
            QString::fromUtf8(QJsonDocument(QJsonArray::fromVariantList(d->registry.listSessions()))
                                  .toJson(QJsonDocument::Compact))));
        QJsonObject result;
        result[QStringLiteral("content")] = content;
        respond(request, result);
        return;
    }

    if (name == QStringLiteral("ssh_connect")) {
        // Stored-session reference: credentials never leave the process.
        QString sessionRef = arguments.value(QStringLiteral("sessionName")).toString();
        if (sessionRef.isEmpty()) {
            sessionRef = arguments.value(QStringLiteral("sessionId")).toString();
        }
        if (!sessionRef.isEmpty()) {
            const QString id = d->registry.createSessionFromStored(sessionRef);
            if (id.isEmpty()) {
                respondError(request, -32000, d->registry.lastError());
                return;
            }
            d->pendingTools[request.value(QStringLiteral("id")).toString()] = QStringLiteral("ssh_connect");
        d->pendingDetails[request.value(QStringLiteral("id")).toString()] = detail;
            return;
        }

        if (arguments.contains(QStringLiteral("password"))) {
            respondError(request, -32602,
                         QStringLiteral("Plaintext password rejected; use passwordCipher or sessionName"));
            return;
        }

        const QString host = arguments.value(QStringLiteral("host")).toString();
        if (host.isEmpty()) {
            respondError(request, -32602, QStringLiteral("Missing required argument: host or sessionName"));
            return;
        }
        SessionConfig config;
        config.setHost(host);
        config.setPort(arguments.value(QStringLiteral("port")).toInt(22));
        config.setUsername(arguments.value(QStringLiteral("username")).toString());
        const QString authMethod = arguments.value(QStringLiteral("authMethod")).toString();
        if (authMethod == QLatin1String("publickey")) {
            config.setAuthMethod(AuthMethod::PublicKey);
            config.setPrivateKeyPath(arguments.value(QStringLiteral("privateKeyPath")).toString());
            const QString cipher = arguments.value(QStringLiteral("keyPassphraseCipher")).toString();
            if (!cipher.isEmpty()) {
                bool ok = false;
                const QByteArray plain = Crypto::rsaDecrypt(cipher.toUtf8(), &ok);
                if (!ok) {
                    respondError(request, -32000, QStringLiteral("Failed to decrypt keyPassphraseCipher"));
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

        const QString cipher = arguments.value(QStringLiteral("passwordCipher")).toString();
        if (!cipher.isEmpty()) {
            bool ok = false;
            const QByteArray plain = Crypto::rsaDecrypt(cipher.toUtf8(), &ok);
            if (!ok) {
                respondError(request, -32000, QStringLiteral("Failed to decrypt passwordCipher"));
                return;
            }
            config.setPassword(SecureString(plain));
        }

        const QString id = d->registry.createSession(config);
        if (id.isEmpty()) {
            respondError(request, -32000, d->registry.lastError());
            return;
        }
        // The result is sent when the connect attempt finishes.
        d->pendingTools[request.value(QStringLiteral("id")).toString()] = QStringLiteral("ssh_connect");
        d->pendingDetails[request.value(QStringLiteral("id")).toString()] = detail;
        return;
    }

    if (name == QStringLiteral("ssh_exec")) {
        const QString sessionId = arguments.value(QStringLiteral("session_id")).toString();
        const QString command = arguments.value(QStringLiteral("command")).toString();
        if (sessionId.isEmpty() || command.isEmpty()) {
            respondError(request, -32602, QStringLiteral("Missing required argument: session_id or command"));
            return;
        }
        const QString requestId = request.value(QStringLiteral("id")).toString();
        if (!d->registry.exec(sessionId, requestId, command)) {
            respondError(request, -32000, d->registry.lastError());
            return;
        }
        d->pendingTools[requestId] = QStringLiteral("ssh_exec");
        d->pendingDetails[requestId] = detail;
        return;
    }

    if (name == QStringLiteral("ssh_disconnect")) {
        const QString sessionId = arguments.value(QStringLiteral("session_id")).toString();
        if (sessionId.isEmpty() || !d->registry.closeSession(sessionId)) {
            respondError(request, -32000, d->registry.lastError().isEmpty()
                                              ? QStringLiteral("Unknown session")
                                              : d->registry.lastError());
            return;
        }
        QJsonArray content;
        content.append(makeTextContent(QStringLiteral("disconnected")));
        QJsonObject result;
        result[QStringLiteral("content")] = content;
        respond(request, result);
        return;
    }

    if (name == QStringLiteral("get_public_key")) {
        const QByteArray pem = Crypto::agentPublicKeyPem();
        if (pem.isEmpty()) {
            respondError(request, -32000, QStringLiteral("Agent keys unavailable (application locked)"));
            return;
        }
        QJsonObject result;
        result[QStringLiteral("publicKeyPem")] = QString::fromUtf8(pem);
        result[QStringLiteral("fingerprint")] = Crypto::agentKeyFingerprint();
        result[QStringLiteral("cipher")] = QStringLiteral("RSA-OAEP-SHA256+base64");
        respond(request, result);
        return;
    }

    if (name == QStringLiteral("ssh_sudo")) {
        // sudo requires a GUI confirmation dialog; headless MCP cannot show it.
        respondError(request, -32000,
                     QStringLiteral("sudo requires GUI user confirmation (headless mode unsupported); "
                                    "use the REST API with the hssh GUI running"));
        return;
    }

    const bool isUpload = name == QStringLiteral("ssh_upload");
    const bool isDownload = name == QStringLiteral("ssh_download");
    if (isUpload || isDownload) {
        const QString sessionId = arguments.value(QStringLiteral("session_id")).toString();
        const QString localPath = arguments.value(QStringLiteral("local_path")).toString();
        const QString remotePath = arguments.value(QStringLiteral("remote_path")).toString();
        if (sessionId.isEmpty() || localPath.isEmpty() || remotePath.isEmpty()) {
            respondError(request, -32602,
                         QStringLiteral("Missing required argument: session_id, local_path or remote_path"));
            return;
        }
        const QString requestId = request.value(QStringLiteral("id")).toString();
        const bool ok = isUpload
            ? d->registry.upload(sessionId, requestId, localPath, remotePath)
            : d->registry.download(sessionId, requestId, remotePath, localPath);
        if (!ok) {
            respondError(request, -32000, d->registry.lastError());
            return;
        }
        d->pendingTools[requestId] = name;
        d->pendingDetails[requestId] = detail;
        return;
    }

    respondError(request, -32602, QStringLiteral("Unknown tool: ") + name);
}

void AgentMcpServer::respond(const QJsonObject &request, const QJsonValue &result)
{
    QJsonObject message;
    message[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    message[QStringLiteral("id")] = request.value(QStringLiteral("id"));
    message[QStringLiteral("result")] = result;
    sendMessage(message);
}

void AgentMcpServer::respondError(const QJsonObject &request, int code, const QString &message)
{
    QJsonObject error;
    error[QStringLiteral("code")] = code;
    error[QStringLiteral("message")] = message;
    QJsonObject response;
    response[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    response[QStringLiteral("id")] = request.value(QStringLiteral("id"));
    response[QStringLiteral("error")] = error;
    sendMessage(response);
}

void AgentMcpServer::sendMessage(const QJsonObject &message)
{
    QTextStream out(stdout);
    out << QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)) << '\n';
    out.flush();
}

} // namespace hssh

#include "AgentMcpServer.moc"

// Tests the agent REST API tab addressing and the visible tab exec:
// name-based refs ("<name>[:<ordinal>]") must keep pointing at the same
// machine when global tab indices drift, and /tabs/<ref>/exec must capture
// output between its begin/end markers with the real exit code.
//
// The tabs provider is a fake AgentTabsInterface whose entry order mirrors
// GUI tab positions and whose per-tab buffer simulates a shell: input sent
// via sendInputToTab is echoed and "executed" (markers recognized).

#include "agent/AgentHttpServer.h"
#include "agent/AgentPolicy.h"
#include "agent/AgentSudoAuth.h"
#include "utils/Config.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

using namespace hssh;

namespace {

QVariantMap sshTab(const QString &name, const QString &host, const QString &title)
{
    QVariantMap m;
    m[QStringLiteral("title")] = title;
    m[QStringLiteral("type")] = QStringLiteral("ssh");
    if (!name.isEmpty()) {
        m[QStringLiteral("sessionName")] = name;
    }
    m[QStringLiteral("host")] = host;
    m[QStringLiteral("port")] = 22;
    m[QStringLiteral("username")] = QStringLiteral("root");
    m[QStringLiteral("connected")] = true;
    return m;
}

QVariantMap localTab(const QString &title)
{
    QVariantMap m;
    m[QStringLiteral("title")] = title;
    m[QStringLiteral("type")] = QStringLiteral("local");
    m[QStringLiteral("connected")] = true;
    return m;
}

class FakeTabs : public AgentTabsInterface {
public:
    QList<QVariantMap> entries;          // ordered like GUI tab positions
    QStringList sentCommands;            // "title|command" audit of sendToTab
    QStringList rawInputs;               // every sendInputToTab payload, in order
    QHash<int, QStringList> buffers;     // simulated terminal buffer per tab
    QVariantList savedSessions;          // returned by listSavedSessions

    QVariantList listTabs() const override
    {
        QVariantList out;
        for (int i = 0; i < entries.size(); ++i) {
            QVariantMap m = entries.at(i);
            m[QStringLiteral("index")] = i;
            out.append(m);
        }
        return out;
    }
    bool sendToTab(int index, const QString &text) override
    {
        if (index < 0 || index >= entries.size()) {
            return false;
        }
        sentCommands.append(entries.at(index).value(QStringLiteral("title")).toString()
                            + QLatin1Char('|') + text);
        return true;
    }
    // Simulates a shell well enough for the visible-exec markers: typed
    // lines echo at a "$ " prompt; "echo hello" prints hello; "false" exits
    // 1; "slow*" never emits the end marker (drives the timeout path).
    bool sendInputToTab(int index, const QString &data) override
    {
        if (index < 0 || index >= entries.size()) {
            return false;
        }
        rawInputs.append(data);        QStringList &buf = buffers[index];
        static const QRegularExpression beginRe(
            QStringLiteral("^echo __HSSH_EXEC_B_(\\w+)__$"));
        const QStringList inputLines = data.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QString token;
        int beginIdx = -1;
        for (int i = 0; i < inputLines.size(); ++i) {
            const QRegularExpressionMatch m = beginRe.match(inputLines.at(i));
            if (m.hasMatch()) {
                token = m.captured(1);
                beginIdx = i;
                break;
            }
        }
        if (token.isEmpty()) {
            buf.append(QStringLiteral("$ ") + data);
            return true;
        }
        const QString command = inputLines.value(beginIdx + 1);
        buf.append(QStringLiteral("$ echo __HSSH_EXEC_B_%1__").arg(token));
        buf.append(QStringLiteral("__HSSH_EXEC_B_%1__").arg(token));
        buf.append(QStringLiteral("$ ") + command);
        int exitCode = 0;
        if (command == QLatin1String("echo hello")) {
            buf.append(QStringLiteral("hello"));
        } else if (command == QLatin1String("false")) {
            exitCode = 1;
        } else if (command.startsWith(QLatin1String("slow"))) {
            return true; // no end marker: the command "runs" forever
        }
        buf.append(QStringLiteral("$ echo __HSSH_EXEC_E_%1__$?").arg(token));
        buf.append(QStringLiteral("__HSSH_EXEC_E_%1__%2").arg(token).arg(exitCode));
        return true;
    }
    bool readTab(int index, int, QString *text) const override
    {
        return readTabRange(index, 0, 0, text);
    }
    bool readTabRange(int index, int, int, QString *text) const override
    {
        if (index < 0 || index >= entries.size() || !text) {
            return false;
        }
        const QStringList buf = buffers.value(index);
        if (!buf.isEmpty()) {
            *text = buf.join(QLatin1Char('\n'));
        } else {
            *text = QStringLiteral("content-of-%1")
                        .arg(entries.at(index).value(QStringLiteral("title")).toString());
        }
        return true;
    }
    int openLocalTab(const QString &) override { return -1; }
    int openSessionTab(const QString &) override { return -1; }
    int openSshTab(const SessionConfig &config) override
    {
        if (config.host().isEmpty()) {
            return -1;
        }
        entries.append(sshTab(config.name(), config.host(),
                              config.name().isEmpty() ? config.host() : config.name()));
        return entries.size() - 1;
    }
    QVariantList listSavedSessions() const override { return savedSessions; }
    bool closeTab(int index) override
    {
        if (index < 0 || index >= entries.size()) {
            return false;
        }
        entries.removeAt(index);
        // Re-key buffers: later tabs shift down one position.
        QHash<int, QStringList> shifted;
        for (auto it = buffers.cbegin(); it != buffers.cend(); ++it) {
            const int key = it.key() > index ? it.key() - 1 : it.key();
            if (it.key() != index) {
                shifted[key] = it.value();
            }
        }
        buffers = shifted;
        return true;
    }
    int reconnectCalls = 0; // test: reconnect route must reach the tab
    QStringList zmodemSends; // test: zsend route must reach the tab
    bool zmodemSendToTab(int index, const QString &localPath) override
    {
        if (index < 0 || index >= entries.size()) {
            return false;
        }
        zmodemSends.append(localPath);
        return true;
    }
    bool reconnectTab(int index) override
    {
        if (index < 0 || index >= entries.size()) {
            return false;
        }
        QVariantMap &m = entries[index];
        if (m.value(QStringLiteral("type")).toString() != QLatin1String("ssh")
            || m.value(QStringLiteral("connected")).toBool()) {
            return false; // only disconnected SSH tabs may reconnect
        }
        ++reconnectCalls;
        m[QStringLiteral("connected")] = true;
        return true;
    }
    bool sendSecretToTab(int, const QString &) override { return false; }
    SessionConfig sessionConfigForTab(int index) const override
    {
        if (index < 0 || index >= entries.size()) {
            return {};
        }
        const QVariantMap &m = entries.at(index);
        // STABLE id derived from the name: SessionConfig() would mint a
        // fresh UUID per call, so AgentPolicy/AgentSudoAuth identities
        // (which key on the id for saved sessions) would never match.
        SessionConfig config(QStringLiteral("fake-") + m.value(QStringLiteral("sessionName")).toString());
        config.setName(m.value(QStringLiteral("sessionName")).toString());
        config.setHost(m.value(QStringLiteral("host")).toString());
        const int port = m.value(QStringLiteral("port")).toInt();
        config.setPort(port > 0 ? port : 22);
        config.setUsername(m.value(QStringLiteral("username")).toString());
        config.setSessionType(m.value(QStringLiteral("type")).toString() == QLatin1String("ssh")
                                  ? SessionType::Ssh
                                  : SessionType::Local);
        return config;
    }
    bool confirmSudo(int, const QString &, QString *reason) override
    {
        if (reason) {
            *reason = QStringLiteral("user_rejected");
        }
        return false;
    }
    bool sudoExec(int, const QString &, const QString &, bool, int,
                  QString *, bool *, int *, QString *) override { return false; }
    // Async sudo: synchronous rejection (test surface has no dialogs).
    void sudoAsync(int, const QString &, const QString &, bool, int,
                   const AgentTabsInterface::SudoAsyncCallback &cb) override
    {
        AgentTabsInterface::SudoAsyncResult r;
        r.confirmed = false;
        r.reason = QStringLiteral("user_rejected");
        cb(r);
    }

    // B5-1: in-memory forward bookkeeping mirroring MainWindow's validation
    // (the /forward routes are what these tests exercise).
    QHash<int, QList<QVariantMap>> forwards; // tab index -> forward list
    bool addForwardToTab(int index, const QVariantMap &spec, QString *errorMessage) override
    {
        const auto fail = [errorMessage](const QString &what) {
            if (errorMessage) {
                *errorMessage = what;
            }
            return false;
        };
        if (index < 0 || index >= entries.size()) {
            return fail(QStringLiteral("unknown_tab"));
        }
        if (entries.at(index).value(QStringLiteral("type")).toString() != QLatin1String("ssh")) {
            return fail(QStringLiteral("not_an_ssh_tab"));
        }
        const QString type = spec.value(QStringLiteral("type")).toString();
        if (!type.isEmpty() && type != QLatin1String("local") && type != QLatin1String("remote")
            && type != QLatin1String("dynamic")) {
            return fail(QStringLiteral("type must be local, remote or dynamic"));
        }
        const int bindPort = spec.value(QStringLiteral("bindPort")).toInt();
        if (bindPort < 1 || bindPort > 65535) {
            return fail(QStringLiteral("bindPort must be 1-65535"));
        }
        const QString effectiveType = type.isEmpty() ? QStringLiteral("local") : type;
        if (effectiveType != QLatin1String("dynamic")) {
            if (spec.value(QStringLiteral("targetHost")).toString().isEmpty()
                || spec.value(QStringLiteral("targetPort")).toInt() < 1) {
                return fail(QStringLiteral("targetHost and targetPort are required"));
            }
        }
        QVariantMap f;
        f[QStringLiteral("type")] = effectiveType;
        f[QStringLiteral("bindAddress")] =
            spec.value(QStringLiteral("bindAddress"), QStringLiteral("127.0.0.1"));
        f[QStringLiteral("bindPort")] = bindPort;
        f[QStringLiteral("target")] = effectiveType == QLatin1String("dynamic")
            ? QString()
            : QStringLiteral("%1:%2")
                  .arg(spec.value(QStringLiteral("targetHost")).toString())
                  .arg(spec.value(QStringLiteral("targetPort")).toInt());
        f[QStringLiteral("active")] = true;
        f[QStringLiteral("status")] = QStringLiteral("listening");
        forwards[index].append(f);
        return true;
    }
    QVariantList listForwardsForTab(int index) const override
    {
        QVariantList result;
        if (index < 0 || index >= entries.size()
            || entries.at(index).value(QStringLiteral("type")).toString() != QLatin1String("ssh")) {
            return result;
        }
        const QList<QVariantMap> list = forwards.value(index);
        for (int i = 0; i < list.size(); ++i) {
            QVariantMap f = list.at(i);
            f[QStringLiteral("index")] = i;
            result.append(f);
        }
        return result;
    }
    bool removeForwardFromTab(int index, int forwardIndex, QString *errorMessage) override
    {
        if (index < 0 || index >= entries.size()
            || entries.at(index).value(QStringLiteral("type")).toString() != QLatin1String("ssh")) {
            if (errorMessage) *errorMessage = QStringLiteral("not_an_ssh_tab");
            return false;
        }
        if (forwardIndex < 0 || forwardIndex >= forwards.value(index).size()) {
            if (errorMessage) *errorMessage = QStringLiteral("forward index out of range");
            return false;
        }
        forwards[index].removeAt(forwardIndex);
        return true;
    }

    // B5-2: scripted policy answers. Default AllowOnce keeps the pre-policy
    // test semantics (requests pass; policyAsks counts the dialogs).
    enum class PolicyAnswerKind { Reject, AllowOnce, AllowAlways, Timeout };
    PolicyAnswerKind policyAnswer = PolicyAnswerKind::AllowOnce;
    int policyAsks = 0;
    void confirmPolicyAsync(int index, const QString &, const QString &,
                            const AgentTabsInterface::PolicyCallback &cb) override
    {
        ++policyAsks;
        AgentTabsInterface::PolicyAnswer a;
        if (index < 0 || index >= entries.size()
            || entries.at(index).value(QStringLiteral("type")).toString() != QLatin1String("ssh")) {
            a.reason = QStringLiteral("unknown_tab");
        } else {
            switch (policyAnswer) {
            case PolicyAnswerKind::AllowOnce: a.allowed = true; break;
            case PolicyAnswerKind::AllowAlways: a.allowed = true; a.always = true; break;
            case PolicyAnswerKind::Timeout: a.reason = QStringLiteral("timeout"); break;
            case PolicyAnswerKind::Reject: a.reason = QStringLiteral("user_rejected"); break;
            }
        }
        cb(a);
    }
};

struct HttpResult {
    int status = 0;
    QByteArray body;
};

// Event-driven single-shot request. MUST NOT use waitForReadyRead: the
// server under test runs in this same thread's event loop, so blocking
// socket waits would starve it (deadlock until timeout).
HttpResult httpRequest(int port, const QByteArray &method, const QString &path,
                       const QJsonObject &body = QJsonObject())
{
    QByteArray payload;
    if (!body.isEmpty()) {
        payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    }
    QByteArray raw = method + ' ' + path.toUtf8() + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!payload.isEmpty()) {
        raw += "Content-Type: application/json\r\nContent-Length: "
               + QByteArray::number(payload.size()) + "\r\n";
    }
    raw += "Connection: close\r\n\r\n" + payload;

    HttpResult result;
    QTcpSocket socket;
    QByteArray response;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&socket, &QTcpSocket::connected, &loop, [&]() {
        socket.write(raw);
    });
    QObject::connect(&socket, &QTcpSocket::readyRead, &loop, [&]() {
        response += socket.readAll();
    });
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(10000);
    socket.connectToHost(QHostAddress::LocalHost, port);
    loop.exec();

    const int headEnd = response.indexOf("\r\n\r\n");
    if (headEnd < 0) {
        return result;
    }
    result.status = QString::fromLatin1(response.left(headEnd).split('\n').first()
                                            .split(' ').value(1))
                        .toInt();
    result.body = response.mid(headEnd + 4);
    return result;
}

} // namespace

class TestAgentHttp : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void tabRefsAnnotated();
    void nameResolution();
    void driftResilience();
    void sendByRef();
    void healthAndStatus();
    void v2Wrapping();
    void headlessRoutesGone();
    void tabTransferValidation();
    void transferStatusAndCancel();
    void sudoAsyncRejected();
    void adHocTabOpen();
    void visibleExec();
    void visibleExecTimeoutAndBusy();
    void staleAndPeerAnnotation();
    void disconnectUnlocksExec();
    void reconnectRoute();
    void execResetOption();
    void forwardRoutes();
    void policyGate();
    void mcpHttpEndpoint();

private:
    AgentHttpServer *m_server = nullptr;
    FakeTabs m_tabs;
};

void TestAgentHttp::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true); // keep audit/config out of the real profile

    m_tabs.entries = {
        sshTab(QStringLiteral("board-a"), QStringLiteral("10.0.0.1"), QStringLiteral("board-a")),
        sshTab(QStringLiteral("board-b"), QStringLiteral("10.0.0.2"), QStringLiteral("board-b#1")),
        sshTab(QStringLiteral("board-b"), QStringLiteral("10.0.0.2"), QStringLiteral("board-b#2")),
        localTab(QStringLiteral("My Pi")),
    };
    // The live peer of board-b#2 disagrees with its configured host (IP
    // literal vs IP literal: a real mismatch the agent must surface).
    m_tabs.entries[2][QStringLiteral("peer")] = QStringLiteral("10.0.0.99");
    // Saved session "board-b" now points elsewhere: the two open board-b
    // tabs are STALE (edited after opening — wrong-machine accident setup).
    QVariantMap saved;
    saved[QStringLiteral("name")] = QStringLiteral("board-b");
    saved[QStringLiteral("displayName")] = QStringLiteral("board-b");
    saved[QStringLiteral("host")] = QStringLiteral("10.9.9.9");
    saved[QStringLiteral("port")] = 22;
    saved[QStringLiteral("username")] = QStringLiteral("root");
    saved[QStringLiteral("locked")] = false;
    m_tabs.savedSessions = {saved};
    m_server = new AgentHttpServer(this);
    m_server->setTabsInterface(&m_tabs);
    QVERIFY2(m_server->start(0), qPrintable(m_server->errorString())); // ephemeral port
    QVERIFY(m_server->port() > 0);
}

void TestAgentHttp::cleanupTestCase()
{
    m_server->stop();
}

void TestAgentHttp::tabRefsAnnotated()
{
    const HttpResult r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/tabs"));
    QCOMPARE(r.status, 200);
    const QJsonArray tabs = QJsonDocument::fromJson(r.body).array();
    QCOMPARE(tabs.size(), 4);

    QCOMPARE(tabs.at(0).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("board-a"));
    QCOMPARE(tabs.at(0).toObject().value(QStringLiteral("ref")).toString(), QStringLiteral("board-a"));
    // Duplicate names get explicit ordinals on every entry.
    QCOMPARE(tabs.at(1).toObject().value(QStringLiteral("ref")).toString(), QStringLiteral("board-b:1"));
    QCOMPARE(tabs.at(2).toObject().value(QStringLiteral("ref")).toString(), QStringLiteral("board-b:2"));
    QCOMPARE(tabs.at(3).toObject().value(QStringLiteral("ref")).toString(), QStringLiteral("My Pi"));
}

void TestAgentHttp::nameResolution()
{
    // By session name.
    HttpResult r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/tabs/board-a/text"));
    QCOMPARE(r.status, 200);
    QCOMPARE(QJsonDocument::fromJson(r.body).object().value(QStringLiteral("text")).toString(),
             QStringLiteral("content-of-board-a"));

    // Name + ordinal picks the 2nd same-name tab.
    r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/tabs/board-b:2/text"));
    QCOMPARE(r.status, 200);
    QCOMPARE(QJsonDocument::fromJson(r.body).object().value(QStringLiteral("text")).toString(),
             QStringLiteral("content-of-board-b#2"));

    // By host.
    r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/tabs/10.0.0.1/text"));
    QCOMPARE(r.status, 200);
    QCOMPARE(QJsonDocument::fromJson(r.body).object().value(QStringLiteral("text")).toString(),
             QStringLiteral("content-of-board-a"));

    // URL-encoded title with a space (local tab).
    r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/tabs/My%20Pi/text"));
    QCOMPARE(r.status, 200);
    QCOMPARE(QJsonDocument::fromJson(r.body).object().value(QStringLiteral("text")).toString(),
             QStringLiteral("content-of-My Pi"));

    // Legacy positional index still works.
    r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/tabs/1/text"));
    QCOMPARE(r.status, 200);
    QCOMPARE(QJsonDocument::fromJson(r.body).object().value(QStringLiteral("text")).toString(),
             QStringLiteral("content-of-board-b#1"));

    // Unknown name -> 404.
    r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/tabs/no-such-board/text"));
    QCOMPARE(r.status, 404);
}

void TestAgentHttp::driftResilience()
{
    // Close the first tab (legacy index): every later tab shifts down one
    // position. Name refs must still hit the same machine.
    const HttpResult del = httpRequest(m_server->port(), "DELETE", QStringLiteral("/api/v1/tabs/0"));
    QCOMPARE(del.status, 200);
    QCOMPARE(m_tabs.entries.size(), 3);

    const HttpResult r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/tabs/board-b:2/text"));
    QCOMPARE(r.status, 200);
    QCOMPARE(QJsonDocument::fromJson(r.body).object().value(QStringLiteral("text")).toString(),
             QStringLiteral("content-of-board-b#2"));

    // Sanity: the bare name now resolves to what was board-b:1 before.
    const HttpResult r2 = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/tabs/board-b/text"));
    QCOMPARE(r2.status, 200);
    QCOMPARE(QJsonDocument::fromJson(r2.body).object().value(QStringLiteral("text")).toString(),
             QStringLiteral("content-of-board-b#1"));
}

void TestAgentHttp::sendByRef()
{
    const HttpResult r = httpRequest(m_server->port(), "POST",
                                     QStringLiteral("/api/v1/tabs/My%20Pi/send"),
                                     QJsonObject{{QStringLiteral("command"), QStringLiteral("uname -a")}});
    QCOMPARE(r.status, 200);
    QCOMPARE(m_tabs.sentCommands, QStringList{QStringLiteral("My Pi|uname -a")});
}

void TestAgentHttp::healthAndStatus()
{
    HttpResult r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/health"));
    QCOMPARE(r.status, 200);
    const QJsonObject health = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(health.value(QStringLiteral("ok")).toBool(), true);
    QVERIFY(health.value(QStringLiteral("pid")).toInteger() > 0);
    QVERIFY(health.contains(QStringLiteral("tabs")));
    QCOMPARE(health.value(QStringLiteral("tabs")).toInt(), 3); // one closed in driftResilience

    r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/status"));
    QCOMPARE(r.status, 200);
    QVERIFY(QJsonDocument::fromJson(r.body).object().value(QStringLiteral("pid")).toInteger() > 0);
}

void TestAgentHttp::v2Wrapping()
{
    HttpResult r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v2/tabs"));
    QCOMPARE(r.status, 200);
    const QJsonObject obj = QJsonDocument::fromJson(r.body).object();
    const QJsonArray tabs = obj.value(QStringLiteral("tabs")).toArray();
    QCOMPARE(tabs.size(), 3);
    QCOMPARE(tabs.at(0).toObject().value(QStringLiteral("ref")).toString(),
             QStringLiteral("board-b:1"));

    r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v2/saved-sessions"));
    QCOMPARE(r.status, 200);
    QVERIFY(QJsonDocument::fromJson(r.body).object().contains(QStringLiteral("savedSessions")));

    // v1 stays a bare array for compatibility.
    r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/tabs"));
    QCOMPARE(r.status, 200);
    QVERIFY(QJsonDocument::fromJson(r.body).isArray());
}

void TestAgentHttp::headlessRoutesGone()
{
    // The headless session pool is deleted: every /sessions route 404s.
    HttpResult r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/sessions"));
    QCOMPARE(r.status, 404);

    r = httpRequest(m_server->port(), "POST", QStringLiteral("/api/v1/sessions"),
                    QJsonObject{{QStringLiteral("host"), QStringLiteral("10.0.0.9")}});
    QCOMPARE(r.status, 404);

    r = httpRequest(m_server->port(), "POST", QStringLiteral("/api/v1/sessions/any-id/exec"),
                    QJsonObject{{QStringLiteral("command"), QStringLiteral("true")}});
    QCOMPARE(r.status, 404);

    r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v1/agent-peers"));
    QCOMPARE(r.status, 404);
}

void TestAgentHttp::tabTransferValidation()
{
    // Missing paths -> 400.
    HttpResult r = httpRequest(m_server->port(), "POST",
                               QStringLiteral("/api/v1/tabs/board-b:1/upload"),
                               QJsonObject{{QStringLiteral("localPath"), QStringLiteral("x")}});
    QVERIFY2(r.status == 400, qPrintable(QStringLiteral("status=%1 body=%2")
                                             .arg(r.status).arg(QString::fromUtf8(r.body))));

    // Unknown transfer method -> 400 (validated before any connection).
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/board-b:1/upload"),
                    QJsonObject{{QStringLiteral("localPath"), QStringLiteral("a")},
                                {QStringLiteral("remotePath"), QStringLiteral("b")},
                                {QStringLiteral("method"), QStringLiteral("carrier-pigeon")}});
    QCOMPARE(r.status, 400);

    // Local tab is not an SSH session -> 400.
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/My%20Pi/upload"),
                    QJsonObject{{QStringLiteral("localPath"), QStringLiteral("a")},
                                {QStringLiteral("remotePath"), QStringLiteral("b")}});
    QCOMPARE(r.status, 400);

    // Unknown tab -> 404 (ref resolution happens before the transfer checks).
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/no-such-board/upload"),
                    QJsonObject{{QStringLiteral("localPath"), QStringLiteral("a")},
                                {QStringLiteral("remotePath"), QStringLiteral("b")}});
    QCOMPARE(r.status, 404);
}

void TestAgentHttp::transferStatusAndCancel()
{
    // No transfer: status reports inactive, cancel 404s.
    HttpResult r = httpRequest(m_server->port(), "GET",
                               QStringLiteral("/api/v1/tabs/board-b:1/transfer"));
    QCOMPARE(r.status, 200);
    QCOMPARE(QJsonDocument::fromJson(r.body).object()
                 .value(QStringLiteral("active")).toBool(), false);
    r = httpRequest(m_server->port(), "DELETE",
                    QStringLiteral("/api/v1/tabs/board-b:1/transfer"));
    QCOMPARE(r.status, 404);

    // A black-hole host: the worker sits in its 10 s connect timeout, so
    // the status window is stable (no fast "unreachable" failure).
    const HttpResult open = httpRequest(m_server->port(), "POST",
                                        QStringLiteral("/api/v1/tabs"),
                                        QJsonObject{{QStringLiteral("host"), QStringLiteral("10.255.255.1")},
                                                    {QStringLiteral("username"), QStringLiteral("u")}});
    QCOMPARE(open.status, 200);
    const QString blackRef = QJsonDocument::fromJson(open.body).object()
                                 .value(QStringLiteral("ref")).toString();
    QCOMPARE(blackRef, QStringLiteral("10.255.255.1"));

    // async:true starts the transfer and answers immediately.
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/10.255.255.1/upload"),
                    QJsonObject{{QStringLiteral("localPath"), QStringLiteral("a")},
                                {QStringLiteral("remotePath"), QStringLiteral("b")},
                                {QStringLiteral("async"), true}});
    QCOMPARE(r.status, 200);
    QJsonObject body = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(body.value(QStringLiteral("started")).toBool(), true);
    QVERIFY(body.contains(QStringLiteral("statusPath")));

    // The transfer is now visible in the status endpoint.
    r = httpRequest(m_server->port(), "GET",
                    QStringLiteral("/api/v1/tabs/10.255.255.1/transfer"));
    QCOMPARE(r.status, 200);
    body = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(body.value(QStringLiteral("active")).toBool(), true);
    QCOMPARE(body.value(QStringLiteral("direction")).toString(), QStringLiteral("upload"));

    // Cancel releases the tab again. The worker may sit in its 10 s connect
    // timeout when cancelled (the flag is checked in the transfer loop), so
    // poll for the release instead of a fixed wait.
    r = httpRequest(m_server->port(), "DELETE",
                    QStringLiteral("/api/v1/tabs/10.255.255.1/transfer"));
    QCOMPARE(r.status, 200);
    QCOMPARE(QJsonDocument::fromJson(r.body).object()
                 .value(QStringLiteral("cancelled")).toBool(), true);
    bool released = false;
    for (int i = 0; i < 40 && !released; ++i) {
        QTest::qWait(500);
        const HttpResult s = httpRequest(m_server->port(), "GET",
                                         QStringLiteral("/api/v1/tabs/10.255.255.1/transfer"));
        released = !QJsonDocument::fromJson(s.body).object()
                        .value(QStringLiteral("active")).toBool();
    }
    QVERIFY2(released, "transfer lock not released after cancel");

    // The tab accepts a new transfer afterwards (no stuck 409) — and clean
    // up the black-hole tab so later tests see the original tab list.
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/10.255.255.1/upload"),
                    QJsonObject{{QStringLiteral("localPath"), QStringLiteral("a")},
                                {QStringLiteral("remotePath"), QStringLiteral("b")},
                                {QStringLiteral("async"), true}});
    QCOMPARE(r.status, 200);
    httpRequest(m_server->port(), "DELETE",
                QStringLiteral("/api/v1/tabs/10.255.255.1/transfer"));
    for (int i = 0; i < 40; ++i) {
        QTest::qWait(500);
        const HttpResult s = httpRequest(m_server->port(), "GET",
                                         QStringLiteral("/api/v1/tabs/10.255.255.1/transfer"));
        if (!QJsonDocument::fromJson(s.body).object()
                 .value(QStringLiteral("active")).toBool()) {
            break;
        }
    }
    httpRequest(m_server->port(), "DELETE",
                QStringLiteral("/api/v1/tabs/10.255.255.1"));
}

void TestAgentHttp::sudoAsyncRejected()
{
    // The async sudo path: FakeTabs rejects synchronously -> 403 with a
    // reason code, and the response carries no executed/exitCode fields.
    HttpResult r = httpRequest(m_server->port(), "POST",
                               QStringLiteral("/api/v1/tabs/board-b:1/sudo"),
                               QJsonObject{{QStringLiteral("command"), QStringLiteral("true")}});
    QCOMPARE(r.status, 403);
    const QJsonObject body = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(body.value(QStringLiteral("reason")).toString(), QStringLiteral("user_rejected"));
    QVERIFY(!body.contains(QStringLiteral("executed")));

    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/board-b:1/sudo"),
                    QJsonObject{{QStringLiteral("command"), QString()}});
    QCOMPARE(r.status, 400);
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/no-such-tab/sudo"),
                    QJsonObject{{QStringLiteral("command"), QStringLiteral("true")}});
    QCOMPARE(r.status, 404);
}

void TestAgentHttp::adHocTabOpen()
{
    // Plaintext password is rejected by policy.
    HttpResult r = httpRequest(m_server->port(), "POST", QStringLiteral("/api/v1/tabs"),
                               QJsonObject{{QStringLiteral("host"), QStringLiteral("10.9.9.9")},
                                           {QStringLiteral("password"), QStringLiteral("secret")}});
    QCOMPARE(r.status, 400);

    // Empty cipher is a client bug, called out explicitly.
    r = httpRequest(m_server->port(), "POST", QStringLiteral("/api/v1/tabs"),
                    QJsonObject{{QStringLiteral("host"), QStringLiteral("10.9.9.9")},
                                {QStringLiteral("passwordCipher"), QStringLiteral("")}});
    QCOMPARE(r.status, 400);

    // Ad-hoc SSH tab opens and reports a drift-safe ref.
    r = httpRequest(m_server->port(), "POST", QStringLiteral("/api/v1/tabs"),
                    QJsonObject{{QStringLiteral("host"), QStringLiteral("10.9.9.9")},
                                {QStringLiteral("username"), QStringLiteral("root")}});
    QCOMPARE(r.status, 200);
    const QJsonObject body = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(body.value(QStringLiteral("ref")).toString(), QStringLiteral("10.9.9.9"));
    QCOMPARE(m_tabs.entries.last().value(QStringLiteral("host")).toString(),
             QStringLiteral("10.9.9.9"));

    // Clean up: close the ad-hoc tab again (its buffer is empty; later exec
    // tests rely on the named tabs).
    const HttpResult del = httpRequest(m_server->port(), "DELETE",
                                       QStringLiteral("/api/v1/tabs/10.9.9.9"));
    QCOMPARE(del.status, 200);
}

void TestAgentHttp::visibleExec()
{
    // Simple command: output captured between markers, echo stripped.
    HttpResult r = httpRequest(m_server->port(), "POST",
                               QStringLiteral("/api/v1/tabs/board-b:1/exec"),
                               QJsonObject{{QStringLiteral("command"), QStringLiteral("echo hello")}});
    QCOMPARE(r.status, 200);
    QJsonObject body = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(body.value(QStringLiteral("output")).toString(), QStringLiteral("hello"));
    QCOMPARE(body.value(QStringLiteral("exitCode")).toInt(), 0);
    QCOMPARE(body.value(QStringLiteral("timedOut")).toBool(), false);
    // The response names the machine that actually ran the command.
    QCOMPARE(body.value(QStringLiteral("target")).toString(),
             QStringLiteral("root@10.0.0.2:22"));

    // Non-zero exit code propagates through the end marker.
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/board-b:1/exec"),
                    QJsonObject{{QStringLiteral("command"), QStringLiteral("false")}});
    QCOMPARE(r.status, 200);
    body = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(body.value(QStringLiteral("exitCode")).toInt(), 1);

    // Missing command -> 400; unknown tab -> 404.
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/board-b:1/exec"), QJsonObject{});
    QCOMPARE(r.status, 400);
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/no-such-board/exec"),
                    QJsonObject{{QStringLiteral("command"), QStringLiteral("true")}});
    QCOMPARE(r.status, 404);
}

void TestAgentHttp::visibleExecTimeoutAndBusy()
{
    // Start a never-finishing exec on a raw socket (no read wait), then
    // immediately fire a second exec on the same tab: it must get 409.
    QTcpSocket first;
    first.connectToHost(QHostAddress::LocalHost, m_server->port());
    QVERIFY(first.waitForConnected(5000));
    const QJsonObject slowBody{{QStringLiteral("command"), QStringLiteral("slow")},
                               {QStringLiteral("timeout"), 1500}};
    const QByteArray payload = QJsonDocument(slowBody).toJson(QJsonDocument::Compact);
    first.write("POST /api/v1/tabs/board-b:1/exec HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                "Content-Type: application/json\r\nContent-Length: "
                + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
    QVERIFY(first.waitForBytesWritten(5000));
    // Let the server process the first exec before the second arrives.
    QTest::qWait(400);

    const HttpResult busy = httpRequest(m_server->port(), "POST",
                                        QStringLiteral("/api/v1/tabs/board-b:1/exec"),
                                        QJsonObject{{QStringLiteral("command"), QStringLiteral("echo hello")}});
    QCOMPARE(busy.status, 409);

    // The first exec answers with timedOut:true once its 1.5 s deadline hits.
    QByteArray response;
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&first, &QTcpSocket::readyRead, &loop, [&]() {
        response += first.readAll();
    });
    QObject::connect(&first, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(8000);
    loop.exec();
    const int headEnd = response.indexOf("\r\n\r\n");
    QVERIFY(headEnd >= 0);
    const QJsonObject body = QJsonDocument::fromJson(response.mid(headEnd + 4)).object();
    QCOMPARE(body.value(QStringLiteral("timedOut")).toBool(), true);
    QCOMPARE(body.value(QStringLiteral("exitCode")).toInt(), -1);

    // After the timed-out exec the tab is free again.
    const HttpResult after = httpRequest(m_server->port(), "POST",
                                         QStringLiteral("/api/v1/tabs/board-b:1/exec"),
                                         QJsonObject{{QStringLiteral("command"), QStringLiteral("echo hello")}});
    QCOMPARE(after.status, 200);
}

void TestAgentHttp::staleAndPeerAnnotation()
{
    const HttpResult r = httpRequest(m_server->port(), "GET", QStringLiteral("/api/v2/tabs"));
    QCOMPARE(r.status, 200);
    const QJsonArray tabs = QJsonDocument::fromJson(r.body).object()
                                .value(QStringLiteral("tabs")).toArray();
    // Current order (after driftResilience closed board-a): board-b#1,
    // board-b#2 (peer), My Pi.
    QCOMPARE(tabs.size(), 3);

    const QJsonObject b1 = tabs.at(0).toObject();
    QCOMPARE(b1.value(QStringLiteral("ref")).toString(), QStringLiteral("board-b:1"));
    QCOMPARE(b1.value(QStringLiteral("target")).toString(), QStringLiteral("root@10.0.0.2:22"));
    // Saved session "board-b" now points to 10.9.9.9: this tab is stale.
    QCOMPARE(b1.value(QStringLiteral("stale")).toBool(), true);
    QCOMPARE(b1.value(QStringLiteral("savedTarget")).toString(),
             QStringLiteral("root@10.9.9.9:22"));

    const QJsonObject b2 = tabs.at(1).toObject();
    QCOMPARE(b2.value(QStringLiteral("ref")).toString(), QStringLiteral("board-b:2"));
    // The live peer wins over the configured host, and the mismatch is flagged.
    QCOMPARE(b2.value(QStringLiteral("target")).toString(), QStringLiteral("root@10.0.0.99:22"));
    QCOMPARE(b2.value(QStringLiteral("peerMismatch")).toBool(), true);

    const QJsonObject pi = tabs.at(2).toObject();
    QCOMPARE(pi.value(QStringLiteral("target")).toString(), QStringLiteral("local"));
    QVERIFY(!pi.contains(QStringLiteral("stale")));
}

void TestAgentHttp::disconnectUnlocksExec()
{
    // Start a never-finishing exec (no end marker), then drop the tab's
    // connection: the poll must fail fast and release the busy lock instead
    // of squatting on it until the deadline (2026-09-11 "409 锁死").
    QVariantMap &tab = m_tabs.entries[0]; // board-b#1
    tab[QStringLiteral("connected")] = true;

    QTcpSocket first;
    first.connectToHost(QHostAddress::LocalHost, m_server->port());
    QVERIFY(first.waitForConnected(5000));
    const QJsonObject slowBody{{QStringLiteral("command"), QStringLiteral("slow")},
                               {QStringLiteral("timeout"), 0}}; // no deadline
    const QByteArray payload = QJsonDocument(slowBody).toJson(QJsonDocument::Compact);
    first.write("POST /api/v1/tabs/board-b:1/exec HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                "Content-Type: application/json\r\nContent-Length: "
                + QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
    QVERIFY(first.waitForBytesWritten(5000));
    QTest::qWait(400); // let the poll start

    // Connection drops mid-exec.
    tab[QStringLiteral("connected")] = false;

    QByteArray response;
    QEventLoop loop;
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&first, &QTcpSocket::readyRead, &loop, [&]() {
        response += first.readAll();
    });
    QObject::connect(&first, &QTcpSocket::disconnected, &loop, &QEventLoop::quit);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(8000);
    loop.exec();
    const int headEnd = response.indexOf("\r\n\r\n");
    QVERIFY(headEnd >= 0);
    QVERIFY(response.left(headEnd).contains("500"));
    QVERIFY(response.mid(headEnd).contains("Connection lost"));

    // The busy lock is released: the next exec runs immediately.
    tab[QStringLiteral("connected")] = true;
    const HttpResult after = httpRequest(m_server->port(), "POST",
                                         QStringLiteral("/api/v1/tabs/board-b:1/exec"),
                                         QJsonObject{{QStringLiteral("command"), QStringLiteral("echo hello")}});
    QCOMPARE(after.status, 200);
}

void TestAgentHttp::reconnectRoute()
{
    QVariantMap &tab = m_tabs.entries[0]; // board-b#1

    // Reconnecting a live tab is rejected (it would kill its foreground job).
    tab[QStringLiteral("connected")] = true;
    HttpResult r = httpRequest(m_server->port(), "POST",
                               QStringLiteral("/api/v1/tabs/board-b:1/reconnect"));
    QCOMPARE(r.status, 400);
    QCOMPARE(m_tabs.reconnectCalls, 0);

    // Disconnected tab: reconnect fires and the target is reported.
    tab[QStringLiteral("connected")] = false;
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/board-b:1/reconnect"));
    QCOMPARE(r.status, 200);
    QCOMPARE(m_tabs.reconnectCalls, 1);
    QCOMPARE(tab.value(QStringLiteral("connected")).toBool(), true);
    QCOMPARE(QJsonDocument::fromJson(r.body).object()
                 .value(QStringLiteral("target")).toString(),
             QStringLiteral("root@10.0.0.2:22"));

    // Local tabs cannot reconnect.
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/My%20Pi/reconnect"));
    QCOMPARE(r.status, 400);
}

void TestAgentHttp::execResetOption()
{
    const int before = m_tabs.rawInputs.size();
    // ZMODEM send route: missing path -> 400, valid -> reaches the tab.
    HttpResult r = httpRequest(m_server->port(), "POST",
                               QStringLiteral("/api/v1/tabs/board-b:1/zsend"),
                               QJsonObject{{QStringLiteral("localPath"), QString()}});
    QCOMPARE(r.status, 400);
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/board-b:1/zsend"),
                    QJsonObject{{QStringLiteral("localPath"), QStringLiteral("C:/t.bin")}});
    QCOMPARE(r.status, 200);
    QCOMPARE(m_tabs.zmodemSends, QStringList{QStringLiteral("C:/t.bin")});
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/board-b:1/exec"),
                    QJsonObject{{QStringLiteral("command"), QStringLiteral("echo hello")},
                                {QStringLiteral("reset"), true}});
    QCOMPARE(r.status, 200);
    // Reset typed Ctrl+C first, then (~400 ms later) the marker sequence
    // with its leading bare newline.
    QCOMPARE(m_tabs.rawInputs.size(), before + 2);
    QCOMPARE(m_tabs.rawInputs.at(before), QStringLiteral("\u0003"));
    QVERIFY(m_tabs.rawInputs.at(before + 1).startsWith(
        QStringLiteral("\necho __HSSH_EXEC_B_")));
}

void TestAgentHttp::forwardRoutes()
{
    // Earlier cases close tabs; work on dedicated tabs appended here so the
    // refs are valid no matter what ran before.
    m_tabs.entries.append(sshTab(QStringLiteral("fwd-host"), QStringLiteral("10.1.1.1"),
                                 QStringLiteral("fwd-host")));
    m_tabs.entries.append(localTab(QStringLiteral("fwd-local")));
    const int sshIndex = m_tabs.entries.size() - 2;
    const int localIndex = sshIndex + 1;

    // Empty list on a fresh SSH tab.
    HttpResult r = httpRequest(m_server->port(), "GET",
                               QStringLiteral("/api/v1/tabs/fwd-host/forward"));
    QCOMPARE(r.status, 200);
    QCOMPARE(QJsonDocument::fromJson(r.body).object()
                 .value(QStringLiteral("forwards")).toArray().size(), 0);

    // Validation: bad type, missing bindPort, local without target.
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/fwd-host/forward"),
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("sctp")},
                                {QStringLiteral("bindPort"), 5000}});
    QCOMPARE(r.status, 400);
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/fwd-host/forward"),
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("local")}});
    QCOMPARE(r.status, 400);
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/fwd-host/forward"),
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("local")},
                                {QStringLiteral("bindPort"), 8080}});
    QCOMPARE(r.status, 400);

    // Local forward answers with the listen address and the full list.
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/fwd-host/forward"),
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("local")},
                                {QStringLiteral("bindPort"), 8080},
                                {QStringLiteral("targetHost"), QStringLiteral("10.0.0.5")},
                                {QStringLiteral("targetPort"), 80}});
    QCOMPARE(r.status, 200);
    QJsonObject body = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(body.value(QStringLiteral("listen")).toString(), QStringLiteral("127.0.0.1:8080"));
    QCOMPARE(body.value(QStringLiteral("target")).toString(), QStringLiteral("root@10.1.1.1:22"));
    QCOMPARE(body.value(QStringLiteral("forwards")).toArray().size(), 1);
    QCOMPARE(body.value(QStringLiteral("forwards")).toArray().at(0).toObject()
                 .value(QStringLiteral("target")).toString(),
             QStringLiteral("10.0.0.5:80"));

    // Dynamic (SOCKS) forward needs no target.
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/fwd-host/forward"),
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("dynamic")},
                                {QStringLiteral("bindPort"), 1080}});
    QCOMPARE(r.status, 200);

    // List shows both, with stable indices.
    r = httpRequest(m_server->port(), "GET",
                    QStringLiteral("/api/v1/tabs/fwd-host/forward"));
    QCOMPARE(r.status, 200);
    QJsonArray list = QJsonDocument::fromJson(r.body).object()
                          .value(QStringLiteral("forwards")).toArray();
    QCOMPARE(list.size(), 2);
    QCOMPARE(list.at(0).toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("local"));
    QCOMPARE(list.at(1).toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("dynamic"));
    QCOMPARE(list.at(1).toObject().value(QStringLiteral("target")).toString(), QString());

    // Remove by index; the remaining list reindexes.
    r = httpRequest(m_server->port(), "DELETE",
                    QStringLiteral("/api/v1/tabs/fwd-host/forward?index=0"));
    QCOMPARE(r.status, 200);
    r = httpRequest(m_server->port(), "GET",
                    QStringLiteral("/api/v1/tabs/fwd-host/forward"));
    list = QJsonDocument::fromJson(r.body).object()
               .value(QStringLiteral("forwards")).toArray();
    QCOMPARE(list.size(), 1);
    QCOMPARE(list.at(0).toObject().value(QStringLiteral("type")).toString(),
             QStringLiteral("dynamic"));

    // Out-of-range and missing index; local tab refusal.
    r = httpRequest(m_server->port(), "DELETE",
                    QStringLiteral("/api/v1/tabs/fwd-host/forward?index=9"));
    QCOMPARE(r.status, 400);
    r = httpRequest(m_server->port(), "DELETE",
                    QStringLiteral("/api/v1/tabs/fwd-host/forward"));
    QCOMPARE(r.status, 400);
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/fwd-local/forward"),
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("local")},
                                {QStringLiteral("bindPort"), 8080},
                                {QStringLiteral("targetHost"), QStringLiteral("x")},
                                {QStringLiteral("targetPort"), 80}});
    QCOMPARE(r.status, 400);

    m_tabs.entries.removeAt(localIndex);
    m_tabs.entries.removeAt(sshIndex);
    m_tabs.forwards.remove(sshIndex);
}

void TestAgentHttp::policyGate()
{
    // Fresh policy state (session grants + persistent rules from earlier
    // runs of this file share the test-mode Config).
    AgentPolicy::instance().clearSessionGrants();
    Config::instance().remove(QStringLiteral("agent/policyRules"));
    Config::instance().sync();

    // Dedicated SSH tab; identity = session:fake-policy-host (stable id).
    m_tabs.entries.append(sshTab(QStringLiteral("policy-host"), QStringLiteral("10.2.2.2"),
                                 QStringLiteral("policy-host")));
    const int tab = m_tabs.entries.size() - 1;
    const QString identity = QStringLiteral("session:fake-policy-host");
    QCOMPARE(AgentSudoAuth::identityFor(m_tabs.sessionConfigForTab(tab)), identity);

    // Default decision for uploads is Ask: the dialog is consulted.
    const int asksBefore = m_tabs.policyAsks;
    m_tabs.policyAnswer = FakeTabs::PolicyAnswerKind::Reject;
    HttpResult r = httpRequest(m_server->port(), "POST",
                               QStringLiteral("/api/v1/tabs/policy-host/upload"),
                               QJsonObject{{QStringLiteral("localPath"), QStringLiteral("a")},
                                           {QStringLiteral("remotePath"), QStringLiteral("b")}});
    QCOMPARE(r.status, 403);
    QVERIFY(QString::fromUtf8(r.body).contains(QLatin1String("user_rejected")));
    QCOMPARE(m_tabs.policyAsks, asksBefore + 1);

    // Timeout answer surfaces its own reason.
    m_tabs.policyAnswer = FakeTabs::PolicyAnswerKind::Timeout;
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/policy-host/upload"),
                    QJsonObject{{QStringLiteral("localPath"), QStringLiteral("a")},
                                {QStringLiteral("remotePath"), QStringLiteral("b")}});
    QCOMPARE(r.status, 403);
    QVERIFY(QString::fromUtf8(r.body).contains(QLatin1String("timeout")));

    // Session grant (the "This session" choice) bypasses the dialog.
    AgentPolicy::instance().grantSession(identity, AgentPolicy::Operation::Upload);
    const int asksGranted = m_tabs.policyAsks;
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/policy-host/upload"),
                    QJsonObject{{QStringLiteral("localPath"), QStringLiteral("a")},
                                {QStringLiteral("remotePath"), QStringLiteral("b")},
                                {QStringLiteral("async"), true}});
    QCOMPARE(r.status, 200);
    QCOMPARE(QJsonDocument::fromJson(r.body).object()
                 .value(QStringLiteral("started")).toBool(), true);
    QCOMPARE(m_tabs.policyAsks, asksGranted); // no dialog needed
    // Reap the started worker (black-hole host, cancel releases the slot).
    httpRequest(m_server->port(), "DELETE",
                QStringLiteral("/api/v1/tabs/policy-host/transfer"));

    // "Always" persists the rule: the NEXT forward needs no dialog.
    AgentPolicy::instance().clearSessionGrants(); // only the rule may grant now
    m_tabs.policyAnswer = FakeTabs::PolicyAnswerKind::AllowAlways;
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/policy-host/forward"),
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("dynamic")},
                                {QStringLiteral("bindPort"), 1080}});
    QCOMPARE(r.status, 200);
    QCOMPARE(AgentPolicy::instance().check(identity, AgentPolicy::Operation::Forward),
             AgentPolicy::Decision::Allow);
    const int asksAlways = m_tabs.policyAsks;
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/policy-host/forward"),
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("dynamic")},
                                {QStringLiteral("bindPort"), 1081}});
    QCOMPARE(r.status, 200);
    QCOMPARE(m_tabs.policyAsks, asksAlways); // rule answered, no dialog

    // An explicit deny rule wins without consulting the user.
    AgentPolicy::instance().setRule(identity, AgentPolicy::Operation::Download,
                                    AgentPolicy::Decision::Deny);
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/policy-host/download"),
                    QJsonObject{{QStringLiteral("localPath"), QStringLiteral("a")},
                                {QStringLiteral("remotePath"), QStringLiteral("b")}});
    QCOMPARE(r.status, 403);
    QVERIFY(QString::fromUtf8(r.body).contains(QLatin1String("policy_denied")));
    QCOMPARE(m_tabs.policyAsks, asksAlways);

    // Local tabs skip the gate entirely (empty identity).
    r = httpRequest(m_server->port(), "POST",
                    QStringLiteral("/api/v1/tabs/My%20Pi/upload"),
                    QJsonObject{{QStringLiteral("localPath"), QStringLiteral("a")},
                                {QStringLiteral("remotePath"), QStringLiteral("b")}});
    QCOMPARE(r.status, 400); // NOT a policy 403: the SSH validation caught it

    // Cleanup: drop rules and the scratch tab.
    Config::instance().remove(QStringLiteral("agent/policyRules"));
    Config::instance().sync();
    AgentPolicy::instance().clearSessionGrants();
    m_tabs.entries.removeAt(tab);
    m_tabs.policyAnswer = FakeTabs::PolicyAnswerKind::AllowOnce;
}

// B5-3: MCP over Streamable HTTP — initialize / tools/list / ping go through
// the SAME tool surface as the stdio transport; notifications get 202.
void TestAgentHttp::mcpHttpEndpoint()
{
    const auto post = [this](const QJsonObject &message) {
        return httpRequest(m_server->port(), "POST", QStringLiteral("/api/v1/mcp"), message);
    };

    // Bad JSON and batch arrays are rejected.
    HttpResult r = httpRequest(m_server->port(), "POST", QStringLiteral("/api/v1/mcp"));
    QCOMPARE(r.status, 400);

    // initialize (numeric id — the id-matching lesson): one JSON-RPC answer.
    r = post(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 1},
                         {QStringLiteral("method"), QStringLiteral("initialize")},
                         {QStringLiteral("params"), QJsonObject()}});
    QCOMPARE(r.status, 200);
    QJsonObject body = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(body.value(QStringLiteral("id")).toInt(), 1);
    QCOMPARE(body.value(QStringLiteral("jsonrpc")).toString(), QStringLiteral("2.0"));
    QVERIFY(body.value(QStringLiteral("result")).toObject()
                .value(QStringLiteral("serverInfo")).toObject()
                .value(QStringLiteral("name")).toString()
            == QStringLiteral("hssh"));

    // tools/list exposes the same tool names as stdio.
    r = post(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), QStringLiteral("list-1")}, // string id
                         {QStringLiteral("method"), QStringLiteral("tools/list")}});
    QCOMPARE(r.status, 200);
    body = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(body.value(QStringLiteral("id")).toString(), QStringLiteral("list-1"));
    const QJsonArray tools = body.value(QStringLiteral("result")).toObject()
                                 .value(QStringLiteral("tools")).toArray();
    QVERIFY(tools.size() >= 10);
    bool hasForward = false;
    for (const QJsonValue &v : tools) {
        if (v.toObject().value(QStringLiteral("name")).toString()
            == QStringLiteral("ssh_forward")) {
            hasForward = true;
        }
    }
    QVERIFY(hasForward);

    // Notifications carry no id: 202 with an empty body.
    r = post(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("method"),
                          QStringLiteral("notifications/initialized")}});
    QCOMPARE(r.status, 202);

    // ping answers with an empty result object.
    r = post(QJsonObject{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                         {QStringLiteral("id"), 7},
                         {QStringLiteral("method"), QStringLiteral("ping")}});
    QCOMPARE(r.status, 200);
    body = QJsonDocument::fromJson(r.body).object();
    QCOMPARE(body.value(QStringLiteral("id")).toInt(), 7);
    QVERIFY(body.value(QStringLiteral("result")).isObject());
}

QTEST_GUILESS_MAIN(TestAgentHttp)
#include "test_agent_http.moc"

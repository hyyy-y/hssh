// Tests the agent REST API tab addressing and the visible tab exec:
// name-based refs ("<name>[:<ordinal>]") must keep pointing at the same
// machine when global tab indices drift, and /tabs/<ref>/exec must capture
// output between its begin/end markers with the real exit code.
//
// The tabs provider is a fake AgentTabsInterface whose entry order mirrors
// GUI tab positions and whose per-tab buffer simulates a shell: input sent
// via sendInputToTab is echoed and "executed" (markers recognized).

#include "agent/AgentHttpServer.h"

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
        rawInputs.append(data);
        QStringList &buf = buffers[index];
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
        SessionConfig config;
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
    void adHocTabOpen();
    void visibleExec();
    void visibleExecTimeoutAndBusy();
    void staleAndPeerAnnotation();
    void disconnectUnlocksExec();
    void reconnectRoute();
    void execResetOption();

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
    const HttpResult r = httpRequest(m_server->port(), "POST",
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

QTEST_GUILESS_MAIN(TestAgentHttp)
#include "test_agent_http.moc"

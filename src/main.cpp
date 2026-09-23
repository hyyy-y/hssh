#include <QApplication>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QIcon>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QLocale>
#include <QLineEdit>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
#include <QPalette>
#include <QTextStream>
#include <QTimer>
#include <QTime>
#include <QTranslator>

#include "agent/AgentAudit.h"
#include "agent/AgentHttpServer.h"
#include "agent/AgentMcpServer.h"
#include "app/MainWindow.h"
#include "core/SessionConfig.h"
#include "core/SshConnect.h"
#include "hssh/Version.h"
#include "utils/Config.h"
#include "utils/Crypto.h"
#include "utils/Theme.h"

#ifdef HSSH_HAS_LIBSSH
#include <libssh/libssh.h>
#endif

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>
#include <cstdio>
#else
#include <iostream>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

// GUI-subsystem executables have no console handles on Windows; attach to
// the parent console so --agent-* and cli modes can print/read there.
// When stdout/stderr are already redirected (pipes, files, MCP stdio) the
// inherited handles must be kept, so the console is only attached when no
// usable standard handle exists.
void attachWindowsConsole()
{
#if defined(Q_OS_WIN)
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(out, &mode)) {
            return; // Already attached to a console.
        }
        return; // Redirected to a pipe/file: keep the handles as-is.
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        freopen("CONIN$", "r", stdin);
    }
#endif
}

// Hidden (no-echo) password prompt. Returns false when no interactive
// console is available (redirected stdin), so callers can require a cipher.
bool readPasswordFromConsole(QString *out)
{
#if defined(Q_OS_WIN)
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (!hIn || hIn == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD mode = 0;
    if (!GetConsoleMode(hIn, &mode)) {
        return false; // Not a console: stdin is redirected.
    }
    SetConsoleMode(hIn, mode & ~ENABLE_ECHO_INPUT);
    QString result;
    while (true) {
        const wchar_t ch = _getwch();
        if (ch == L'\r' || ch == L'\n') {
            break;
        }
        if (ch == 3) { // Ctrl+C
            SetConsoleMode(hIn, mode);
            return false;
        }
        if (ch == 8) { // Backspace
            if (!result.isEmpty()) {
                result.chop(1);
            }
            continue;
        }
        result.append(QChar(ch));
    }
    SetConsoleMode(hIn, mode);
    *out = result;
    return true;
#else
    termios oldt{};
    if (tcgetattr(STDIN_FILENO, &oldt) != 0) {
        return false;
    }
    termios newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    std::string line;
    std::getline(std::cin, line);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    *out = QString::fromUtf8(line.c_str());
    return true;
#endif
}

bool loadApplicationTranslator(QApplication &app)
{
    const QString localeName = QLocale::system().name(); // e.g. "zh_CN", "en_US"
    const QString baseName = QStringLiteral("hssh_") + localeName;

    auto *translator = new QTranslator(&app);
    if (translator->load(baseName, QStringLiteral(":/i18n"))) {
        app.installTranslator(translator);
        return true;
    }

    // Try language-only fallback (e.g. "hssh_zh" for "zh_CN").
    const QString languageOnly = localeName.left(localeName.indexOf(QLatin1Char('_')));
    if (!languageOnly.isEmpty()) {
        const QString fallbackName = QStringLiteral("hssh_") + languageOnly;
        if (translator->load(fallbackName, QStringLiteral(":/i18n"))) {
            app.installTranslator(translator);
            return true;
        }
    }

    delete translator;
    return false;
}

void loadQtTranslator(QApplication &app)
{
    const QString localeName = QLocale::system().name();

    // Load Qt's own translations from the application directory (deployed by windeployqt).
    auto *qtTranslator = new QTranslator(&app);
    if (qtTranslator->load(QStringLiteral("qt_") + localeName,
                              QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        app.installTranslator(qtTranslator);
        return;
    }

    // Fallback: load from the executable's directory/translations.
    const QString translationsDir = QCoreApplication::applicationDirPath() + QStringLiteral("/translations");
    if (qtTranslator->load(QStringLiteral("qt_") + localeName, translationsDir)) {
        app.installTranslator(qtTranslator);
        return;
    }

    delete qtTranslator;
}

} // namespace

// hssh cli exec --host H [--port P] [--username U] [--auth-method M]
//              [--password X] [--private-key K] --command "cmd"
// Non-interactive command execution with JSON output on stdout.
int runCliExec(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("hssh"));
    app.setOrganizationName(QStringLiteral("hssh-project"));
    app.setApplicationVersion(QStringLiteral(HSSH_VERSION_STRING));

    attachWindowsConsole();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("hssh - Modern SSH Client"));
    parser.addPositionalArgument(QStringLiteral("exec"), QStringLiteral("Execute a command on a remote host"));
    parser.addPositionalArgument(QStringLiteral("cipher"),
                                 QStringLiteral("Encrypt text with the agent public key (prints the cipher)"),
                                 QStringLiteral("[text...]"));

    QCommandLineOption hostOption(QStringLiteral("host"), QStringLiteral("Remote host"), QStringLiteral("host"));
    QCommandLineOption portOption(QStringList{QStringLiteral("p"), QStringLiteral("port")},
                                  QStringLiteral("SSH port (default 22)"), QStringLiteral("port"), QStringLiteral("22"));
    QCommandLineOption userOption(QStringList{QStringLiteral("l"), QStringLiteral("username")},
                                  QStringLiteral("Login user"), QStringLiteral("user"));
    QCommandLineOption authOption(QStringLiteral("auth-method"),
                                  QStringLiteral("password, publickey or agent (default password)"),
                                  QStringLiteral("method"), QStringLiteral("password"));
    QCommandLineOption cipherOption(QStringLiteral("password-cipher"),
                                    QStringLiteral("RSA-OAEP-SHA256+base64 encrypted password "
                                                   "(see hssh /api/v1/keys for the public key)"),
                                    QStringLiteral("cipher"));
    QCommandLineOption keyOption(QStringLiteral("private-key"), QStringLiteral("Path to a private key"), QStringLiteral("path"));
    QCommandLineOption commandOption(QStringLiteral("command"), QStringLiteral("Command to run"), QStringLiteral("command"));

    parser.addOptions({hostOption, portOption, userOption, authOption,
                       cipherOption, keyOption, commandOption});
    parser.process(app);

    const QStringList positional = parser.positionalArguments();
    // argv is [hssh, cli, <subcommand>, ...]: "cli" is positional[0].
    if (positional.size() >= 2 && positional.at(1) == QLatin1String("cipher")) {
        // hssh cli cipher [text...] — encrypt with the agent public key.
        const QByteArray pem = hssh::Crypto::agentPublicKeyPem();
        if (pem.isEmpty()) {
            QTextStream(stderr) << "Agent keys unavailable (application locked)\n";
            return 1;
        }
        QString text;
        if (positional.size() > 2) {
            text = positional.mid(2).join(QLatin1Char(' '));
        } else {
            QTextStream in(stdin);
            text = in.readAll().trimmed();
        }
        bool ok = false;
        const QByteArray cipher = hssh::Crypto::rsaEncryptWithPublicKey(text.toUtf8(), pem, &ok);
        if (!ok) {
            QTextStream(stderr) << "Encryption failed\n";
            return 1;
        }
        QJsonObject result;
        result[QStringLiteral("cipher")] = QString::fromUtf8(cipher);
        result[QStringLiteral("fingerprint")] = hssh::Crypto::agentKeyFingerprint();
        QTextStream(stdout) << QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)) << '\n';
        return 0;
    }

    const QString host = parser.value(hostOption);
    const QString command = parser.value(commandOption);
    if (host.isEmpty() || command.isEmpty()) {
        QTextStream(stderr) << "Usage: hssh cli exec --host H --command CMD [--port P] [--username U] "
                               "[--password-cipher C | --private-key K]\n";
        return 2;
    }

    hssh::SessionConfig config;
    config.setHost(host);
    config.setPort(parser.value(portOption).toInt());
    config.setUsername(parser.value(userOption));
    const QString authMethod = parser.value(authOption);
    if (authMethod == QLatin1String("publickey")) {
        config.setAuthMethod(hssh::AuthMethod::PublicKey);
        config.setPrivateKeyPath(parser.value(keyOption));
    } else if (authMethod == QLatin1String("agent")) {
        config.setAuthMethod(hssh::AuthMethod::Agent);
    } else {
        config.setAuthMethod(hssh::AuthMethod::Password);

        const QString cipher = parser.value(cipherOption);
        if (!cipher.isEmpty()) {
            bool ok = false;
            const QByteArray plain = hssh::Crypto::rsaDecrypt(cipher.toUtf8(), &ok);
            if (!ok) {
                QTextStream(stderr) << "Failed to decrypt password cipher (locked or bad cipher)\n";
                return 1;
            }
            config.setPassword(hssh::SecureString(plain));
        } else {
            // Interactive, hidden password prompt when a TTY is available.
            QTextStream out(stdout);
            out << "Password: " << Qt::flush;
            QString password;
            if (!readPasswordFromConsole(&password)) {
                QTextStream(stderr) << "No password provided (non-interactive mode requires --password-cipher)\n";
                return 2;
            }
            out << '\n';
            config.setPassword(hssh::SecureString(password));
            password.fill(QLatin1Char('\0'));
        }
    }

    QJsonObject result;
    QString error;
    hssh::AgentAudit::log(hssh::AgentAudit::Source::Cli, QStringLiteral("exec"),
                          QStringLiteral("host=%1 user=%2 cmd=\"%3\"")
                              .arg(host, config.username(), command),
                          QStringLiteral("invoked"));
    ssh_session session = hssh::sshConnectAndAuthenticate(config, &error);
    if (!session) {
        hssh::AgentAudit::log(hssh::AgentAudit::Source::Cli, QStringLiteral("exec"),
                              QStringLiteral("host=%1").arg(host),
                              QStringLiteral("error: %1").arg(error));
        result[QStringLiteral("ok")] = false;
        result[QStringLiteral("error")] = error;
        QTextStream(stdout) << QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)) << '\n';
        return 1;
    }

    ssh_channel channel = ssh_channel_new(session);
    int exitCode = -1;
    if (channel && ssh_channel_open_session(channel) == SSH_OK
        && ssh_channel_request_exec(channel, command.toUtf8().constData()) == SSH_OK) {
        QByteArray output;
        char buffer[4096];
        int n = 0;
        while ((n = ssh_channel_read(channel, buffer, sizeof(buffer), 0)) > 0) {
            output.append(buffer, n);
        }
        while ((n = ssh_channel_read(channel, buffer, sizeof(buffer), 1)) > 0) {
            output.append(buffer, n);
        }
        ssh_channel_send_eof(channel);
        exitCode = ssh_channel_get_exit_status(channel);
        ssh_channel_close(channel);
        result[QStringLiteral("output")] = QString::fromUtf8(output);
    } else {
        result[QStringLiteral("error")] = QString::fromUtf8(ssh_get_error(session));
    }
    if (channel) {
        ssh_channel_free(channel);
    }
    ssh_disconnect(session);
    ssh_free(session);

    hssh::AgentAudit::log(hssh::AgentAudit::Source::Cli, QStringLiteral("exec"),
                          QStringLiteral("host=%1 cmd=\"%2\"").arg(host, command),
                          QStringLiteral("ok exit=%1").arg(exitCode));
    result[QStringLiteral("ok")] = true;
    result[QStringLiteral("exitCode")] = exitCode;
    QTextStream(stdout) << QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)) << '\n';
    return 0;
}

// Headless agent modes: hssh --agent-mcp (MCP stdio server, a bridge to the
// GUI agent — it opens no SSH connections of its own) and hssh --agent-http
// [--port N] (local REST API).
int runAgentCli(int argc, char *argv[], bool mcpMode)
{
    Q_UNUSED(mcpMode)
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("hssh"));
    app.setOrganizationName(QStringLiteral("hssh-project"));
    app.setApplicationVersion(QStringLiteral(HSSH_VERSION_STRING));

    attachWindowsConsole();

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("hssh - Modern SSH Client"));
    QCommandLineOption mcpOption(QStringLiteral("agent-mcp"),
                                 QStringLiteral("Run the MCP stdio server (headless)"));
    QCommandLineOption httpOption(QStringLiteral("agent-http"),
                                  QStringLiteral("Run the local REST API server (headless)"));
    QCommandLineOption portOption(QStringList{QStringLiteral("p"), QStringLiteral("port")},
                                  QStringLiteral("HTTP port (default 8222)"),
                                  QStringLiteral("port"), QStringLiteral("8222"));
    parser.addOptions({mcpOption, httpOption, portOption});
    parser.process(app);

    if (mcpMode) {
        hssh::AgentMcpServer server;
        return app.exec();
    }

    hssh::AgentHttpServer server;
    if (!server.start(parser.value(portOption).toInt())) {
        QTextStream(stderr) << QStringLiteral("Failed to start agent: %1\n").arg(server.errorString());
        return 1;
    }
    QTextStream(stdout) << QStringLiteral("hssh agent listening on %1\n").arg(server.url());
    const int rc = app.exec();
    server.stop();
    return rc;
}

int main(int argc, char *argv[])
{
    // Test observability: HSSH_DEBUG_LOG=<file> mirrors qDebug/qInfo/qWarning
    // to a file (GUI subsystem builds have no console).
    if (qEnvironmentVariableIsSet("HSSH_DEBUG_LOG")) {
        const QString dbgPath = qEnvironmentVariable("HSSH_DEBUG_LOG");
        QFile *dbgFile = new QFile(dbgPath);
        if (dbgFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            static QFile *g_dbgFile = dbgFile; // captured via static for the handler
            qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &,
                                      const QString &message) {
                static QMutex mutex;
                QMutexLocker locker(&mutex);
                const char *tag = type == QtDebugMsg ? "DBG "
                                  : type == QtWarningMsg ? "WRN "
                                  : type == QtCriticalMsg ? "CRT "
                                  : type == QtFatalMsg ? "FTL " : "INF ";
                QTextStream(g_dbgFile)
                    << QTime::currentTime().toString(QStringLiteral("hh:mm:ss.zzz")) << ' '
                    << tag << message << '\n';
                g_dbgFile->flush();
            });
        } else {
            delete dbgFile;
        }
    }

    const QStringList rawArgs = [&]() {
        QStringList list;
        for (int i = 0; i < argc; ++i) {
            list.append(QString::fromLocal8Bit(argv[i]));
        }
        return list;
    }();

    if (rawArgs.contains(QStringLiteral("--agent-mcp"))) {
        return runAgentCli(argc, argv, true);
    }
    if (rawArgs.contains(QStringLiteral("--agent-http"))) {
        return runAgentCli(argc, argv, false);
    }
    if (rawArgs.size() >= 2 && rawArgs.at(1) == QStringLiteral("cli")) {
        return runCliExec(argc, argv);
    }

    QApplication app(argc, argv);

    app.setApplicationName(QStringLiteral("hssh"));
    app.setOrganizationName(QStringLiteral("hssh-project"));
    app.setApplicationDisplayName(QStringLiteral("hssh - Modern SSH Client"));
    app.setApplicationVersion(QStringLiteral(HSSH_VERSION_STRING));
    app.setWindowIcon(QIcon(QStringLiteral(":/hssh.ico")));

    loadQtTranslator(app);
    loadApplicationTranslator(app);
    hssh::applyTheme(app, hssh::Config::instance()
                                .value(QStringLiteral("ui/theme"), QStringLiteral("dark"))
                                .toString());

    // Master-password mode: stored secrets cannot be read until unlocked.
    while (hssh::Crypto::usesMasterPassword() && !hssh::Crypto::isUnlocked()) {
        bool ok = false;
        const QString password = QInputDialog::getText(
            nullptr, QStringLiteral("hssh"),
            QCoreApplication::translate("main", "Master password:"),
            QLineEdit::Password, QString(), &ok);
        if (!ok) {
            return 0;
        }
        if (!hssh::Crypto::unlock(password)) {
            QMessageBox::warning(nullptr, QStringLiteral("hssh"),
                                 QCoreApplication::translate("main", "Wrong master password."));
        }
    }

    // --agent: start the local REST agent with the GUI. The MCP bridge
    // auto-launches the GUI this way when no agent is reachable.
    // --agent --port N: bind a different port (secondary test instances;
    // the primary GUI keeps 8222). The port must reach the constructor:
    // agent/autostart fires during it and would otherwise grab 8222.
    int agentPort = 8222;
    if (rawArgs.contains(QStringLiteral("--agent"))) {
        const int portIdx = rawArgs.indexOf(QStringLiteral("--port"));
        if (portIdx >= 0 && portIdx + 1 < rawArgs.size()) {
            bool portOk = false;
            const int parsed = rawArgs.at(portIdx + 1).toInt(&portOk);
            if (portOk && parsed > 0 && parsed < 65536) {
                agentPort = parsed;
            }
        }
    }
    // --no-restore: test instances skip the crash-restore prompt (must also
    // reach the constructor — restorePreviousTabs runs during it).
    const bool skipRestore = rawArgs.contains(QStringLiteral("--no-restore"));
    hssh::MainWindow window(agentPort, skipRestore);
    window.show();
    // PH1-05: persisted window opacity (percent; 100 = opaque).
    const int windowOpacity = hssh::Config::instance()
                                  .value(QStringLiteral("ui/windowOpacity"), 100)
                                  .toInt();
    if (windowOpacity < 100) {
        window.setWindowOpacity(windowOpacity / 100.0);
    }

    if (rawArgs.contains(QStringLiteral("--agent"))) {
        QTimer::singleShot(0, &window, [&window, agentPort]() {
            window.startAgent(agentPort);
        });
    }

    return app.exec();
}

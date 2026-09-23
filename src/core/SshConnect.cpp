#include "SshConnect.h"

#include "SessionConfig.h"

#include <QCoreApplication>
#include <QDateTime>

#include <thread>

#ifdef Q_OS_WIN
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
#endif

#include <cstring>

#include <libssh/libssh.h>

namespace hssh {

namespace {

int authenticate(ssh_session session, const SessionConfig &config,
                 const KbdintPrompter &kbdintPrompter, QString *detailError)
{
    const AuthMethod method = config.authMethod();

    if (method == AuthMethod::Agent) {
        return ssh_userauth_publickey_auto(session, nullptr, nullptr);
    }

    if (method == AuthMethod::PublicKey) {
        const QString keyPath = config.privateKeyPath();
        if (keyPath.isEmpty()) {
            return SSH_AUTH_DENIED;
        }
        ssh_key privateKey = nullptr;
        const QByteArray passphrase = config.keyPassphrase().toByteArray();
        int rc = ssh_pki_import_privkey_file(keyPath.toUtf8().constData(),
                                              passphrase.isEmpty() ? nullptr : passphrase.constData(),
                                              nullptr,
                                              nullptr,
                                              &privateKey);
        if (rc != SSH_OK) {
            return SSH_AUTH_DENIED;
        }
        rc = ssh_userauth_publickey(session, nullptr, privateKey);
        ssh_key_free(privateKey);
        return rc;
    }

    if (method == AuthMethod::KeyboardInteractive) {
        // PH2-13: real keyboard-interactive rounds — supports 2FA (TOTP,
        // push, one-time codes). A lone hidden "password" prompt is answered
        // from the stored password (previous behavior); anything else needs
        // an interactive prompter.
        const QString password = config.password().toString();
        int rc = ssh_userauth_kbdint(session, nullptr, nullptr);
        int rounds = 0;
        while (rc == SSH_AUTH_INFO) {
            if (++rounds > 8) {
                if (detailError) {
                    *detailError = QStringLiteral("Too many keyboard-interactive rounds.");
                }
                return SSH_AUTH_DENIED;
            }
            const char *rawName = ssh_userauth_kbdint_getname(session);
            const char *rawInstruction = ssh_userauth_kbdint_getinstruction(session);
            const QString name = QString::fromUtf8(rawName ? rawName : "");
            const QString instruction = QString::fromUtf8(rawInstruction ? rawInstruction : "");
            // 0.10 libssh names this getnprompts (getnanswers is 0.11+).
            const int n = ssh_userauth_kbdint_getnprompts(session);
            QStringList prompts;
            QList<bool> echo;
            for (int i = 0; i < n; ++i) {
                char echoChar = 0;
                // This libssh returns const storage (no free needed).
                const char *prompt =
                    ssh_userauth_kbdint_getprompt(session, static_cast<unsigned int>(i), &echoChar);
                prompts.append(QString::fromUtf8(prompt ? prompt : ""));
                echo.append(echoChar != 0);
            }

            QStringList answers;
            if (n == 1 && !echo.first() && !password.isEmpty()
                && prompts.first().contains(QLatin1String("password"),
                                            Qt::CaseInsensitive)) {
                // Classic password round via kbdint: use the stored password.
                answers.append(password);
            } else {
                if (!kbdintPrompter || !kbdintPrompter(name, instruction, prompts, echo, &answers)
                    || answers.size() != n) {
                    if (detailError) {
                        *detailError = QStringLiteral(
                            "Keyboard-interactive authentication %1 (2FA prompts require "
                            "an interactive terminal tab).")
                            .arg(kbdintPrompter ? QStringLiteral("was cancelled")
                                                : QStringLiteral("needs interaction"));
                    }
                    return SSH_AUTH_DENIED;
                }
            }
            for (int i = 0; i < n; ++i) {
                ssh_userauth_kbdint_setanswer(session, i, answers.at(i).toUtf8().constData());
            }
            rc = ssh_userauth_kbdint(session, nullptr, nullptr);
        }
        return rc;
    }

    if (method == AuthMethod::Password) {
        const QString password = config.password().toString();
        return ssh_userauth_password(session, nullptr, password.toUtf8().constData());
    }

    return SSH_AUTH_DENIED;
}

constexpr long proxyConnectTimeoutSec = 10;

bool wouldBlock()
{
#ifdef Q_OS_WIN
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EINPROGRESS || errno == EWOULDBLOCK;
#endif
}

// Sends exactly size bytes (bounded by the socket send timeout).
bool sendAll(socket_t fd, const char *data, size_t size)
{
    size_t sent = 0;
    while (sent < size) {
        const int n = ::send(fd, data + sent, static_cast<int>(size - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Receives until the buffer holds exactly size bytes.
bool recvAll(socket_t fd, char *data, size_t size)
{
    size_t got = 0;
    while (got < size) {
        const int n = ::recv(fd, data + got, static_cast<int>(size - got), 0);
        if (n <= 0) {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

// Reads until the pattern appears in the stream (bounded by the receive
// timeout). Returns everything up to and including the pattern.
bool recvUntil(socket_t fd, const char *pattern, QByteArray *out)
{
    const size_t patLen = std::strlen(pattern);
    QByteArray buffer;
    char chunk[1024];
    while (!buffer.contains(pattern)) {
        if (static_cast<size_t>(buffer.size()) > 64 * 1024) {
            return false; // runaway response
        }
        const int n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return false;
        }
        buffer.append(chunk, n);
    }
    if (out) {
        *out = buffer.left(static_cast<int>(buffer.indexOf(pattern) + patLen));
    }
    return true;
}

// Blocking TCP connect with a bounded timeout (used for the proxy itself).
socket_t tcpConnect(const QString &host, int port, QString *error)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *result = nullptr;
    const QByteArray hostBytes = host.toUtf8();
    char portBuf[16] = {};
    snprintf(portBuf, sizeof(portBuf), "%d", port);
    if (getaddrinfo(hostBytes.constData(), portBuf, &hints, &result) != 0 || !result) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect",
                                                 "Cannot resolve proxy host %1").arg(host);
        }
        return SSH_INVALID_SOCKET;
    }

    const socket_t fd = ::socket(result->ai_family, result->ai_socktype, 0);
    if (fd == SSH_INVALID_SOCKET) {
        freeaddrinfo(result);
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect", "Cannot create socket");
        }
        return SSH_INVALID_SOCKET;
    }

    // Non-blocking connect + select for the timeout.
#ifdef Q_OS_WIN
    u_long nonBlock = 1;
    ioctlsocket(fd, FIONBIO, &nonBlock);
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    const int rc = ::connect(fd, result->ai_addr, static_cast<socklen_t>(result->ai_addrlen));
    freeaddrinfo(result);
    if (rc != 0 && !wouldBlock()) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect",
                                                 "Cannot connect to proxy %1:%2")
                         .arg(host).arg(port);
        }
        closesocket(fd);
        return SSH_INVALID_SOCKET;
    }
    if (rc != 0) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(fd, &writeSet);
        timeval tv{};
        tv.tv_sec = proxyConnectTimeoutSec;
        if (::select(static_cast<int>(fd) + 1, nullptr, &writeSet, nullptr, &tv) <= 0) {
            if (error) {
                *error = QCoreApplication::translate("hssh::SshConnect",
                                                     "Proxy connect timeout (%1:%2)")
                             .arg(host).arg(port);
            }
            closesocket(fd);
            return SSH_INVALID_SOCKET;
        }
    }
    // Back to blocking mode with send/recv timeouts.
#ifdef Q_OS_WIN
    u_long block = 0;
    ioctlsocket(fd, FIONBIO, &block);
#else
    fcntl(fd, F_SETFL, flags);
#endif
    timeval ioTv{};
    ioTv.tv_sec = proxyConnectTimeoutSec;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ioTv), sizeof(ioTv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ioTv), sizeof(ioTv));
    return fd;
}

// HTTP CONNECT tunnel (with optional Basic auth).
socket_t connectViaHttpProxy(const SessionConfig &config, QString *error)
{
    const socket_t fd = tcpConnect(config.proxyHost(), config.proxyPort(), error);
    if (fd == SSH_INVALID_SOCKET) {
        return SSH_INVALID_SOCKET;
    }
    QByteArray request = "CONNECT " + config.host().toUtf8() + ":"
                             + QByteArray::number(config.port()) + " HTTP/1.1\r\nHost: "
                             + config.host().toUtf8() + "\r\n";
    if (!config.proxyUsername().isEmpty()) {
        const QByteArray cred = config.proxyUsername().toUtf8() + ":"
                                + config.proxyPassword().toString().toUtf8();
        request += "Proxy-Authorization: Basic " + cred.toBase64() + "\r\n";
    }
    request += "\r\n";
    if (!sendAll(fd, request.constData(), static_cast<size_t>(request.size()))) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect",
                                                 "Failed to send HTTP CONNECT to proxy");
        }
        closesocket(fd);
        return SSH_INVALID_SOCKET;
    }
    QByteArray head;
    if (!recvUntil(fd, "\r\n\r\n", &head) || !head.startsWith("HTTP/1.")
        || !head.contains(" 200")) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect",
                                                 "HTTP proxy refused CONNECT (%1)")
                         .arg(QString::fromUtf8(head.left(64)));
        }
        closesocket(fd);
        return SSH_INVALID_SOCKET;
    }
    return fd;
}

// SOCKS5 connect (RFC 1928; user/pass auth per RFC 1929).
socket_t connectViaSocks5Proxy(const SessionConfig &config, QString *error)
{
    const socket_t fd = tcpConnect(config.proxyHost(), config.proxyPort(), error);
    if (fd == SSH_INVALID_SOCKET) {
        return SSH_INVALID_SOCKET;
    }
    const bool hasAuth = !config.proxyUsername().isEmpty();
    const unsigned char greet[4] = {0x05, static_cast<unsigned char>(hasAuth ? 2 : 1),
                                    0x00, 0x02};
    if (!sendAll(fd, reinterpret_cast<const char *>(greet), hasAuth ? 4 : 3)) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect",
                                                 "Failed to greet SOCKS5 proxy");
        }
        closesocket(fd);
        return SSH_INVALID_SOCKET;
    }
    unsigned char choice[2] = {};
    if (!recvAll(fd, reinterpret_cast<char *>(choice), 2)) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect", "SOCKS5 proxy no reply");
        }
        closesocket(fd);
        return SSH_INVALID_SOCKET;
    }
    if (choice[0] != 0x05) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect", "Not a SOCKS5 proxy");
        }
        closesocket(fd);
        return SSH_INVALID_SOCKET;
    }
    if (choice[1] == 0x02) {
        // RFC 1929 user/pass subnegotiation.
        const QByteArray user = config.proxyUsername().toUtf8();
        const QByteArray pass = config.proxyPassword().toString().toUtf8();
        if (user.size() > 255 || pass.size() > 255) {
            if (error) {
                *error = QCoreApplication::translate("hssh::SshConnect",
                                                     "SOCKS5 credentials too long");
            }
            closesocket(fd);
            return SSH_INVALID_SOCKET;
        }
        QByteArray auth(3 + user.size() + pass.size(), Qt::Uninitialized);
        auth[0] = 0x01;
        auth[1] = static_cast<char>(user.size());
        memcpy(auth.data() + 2, user.constData(), user.size());
        auth[2 + user.size()] = static_cast<char>(pass.size());
        memcpy(auth.data() + 3 + user.size(), pass.constData(), pass.size());
        if (!sendAll(fd, auth.constData(), static_cast<size_t>(auth.size()))
            || !recvAll(fd, reinterpret_cast<char *>(choice), 2) || choice[1] != 0x00) {
            if (error) {
                *error = QCoreApplication::translate("hssh::SshConnect",
                                                     "SOCKS5 authentication failed");
            }
            closesocket(fd);
            return SSH_INVALID_SOCKET;
        }
    } else if (choice[1] != 0x00) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect",
                                                 "SOCKS5 proxy rejected auth methods");
        }
        closesocket(fd);
        return SSH_INVALID_SOCKET;
    }

    // CONNECT: VER 5 CMD 1 RSV 0 ATYP 3 (domainname — the proxy resolves).
    const QByteArray host = config.host().toUtf8();
    if (host.size() > 255) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect", "Target host too long");
        }
        closesocket(fd);
        return SSH_INVALID_SOCKET;
    }
    QByteArray req(7 + host.size(), Qt::Uninitialized);
    req[0] = 0x05;
    req[1] = 0x01; // CONNECT
    req[2] = 0x00;
    req[3] = 0x03; // domain
    req[4] = static_cast<char>(host.size());
    memcpy(req.data() + 5, host.constData(), host.size());
    const quint16 port = static_cast<quint16>(config.port());
    req[5 + host.size()] = static_cast<char>(port >> 8);
    req[6 + host.size()] = static_cast<char>(port & 0xff);
    if (!sendAll(fd, req.constData(), static_cast<size_t>(req.size()))) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect",
                                                 "Failed to send SOCKS5 CONNECT");
        }
        closesocket(fd);
        return SSH_INVALID_SOCKET;
    }
    unsigned char head[4] = {};
    if (!recvAll(fd, reinterpret_cast<char *>(head), 4) || head[0] != 0x05 || head[1] != 0x00) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect",
                                                 "SOCKS5 CONNECT failed (reply 0x%1)")
                         .arg(head[1], 2, 16, QLatin1Char('0'));
        }
        closesocket(fd);
        return SSH_INVALID_SOCKET;
    }
    // Skip the bound address: 4 (IPv4) / 16 (IPv6) / 1+len (domain) + port.
    size_t skip = 2;
    if (head[3] == 0x01) {
        skip += 4;
    } else if (head[3] == 0x04) {
        skip += 16;
    } else if (head[3] == 0x03) {
        unsigned char len = 0;
        if (!recvAll(fd, reinterpret_cast<char *>(&len), 1)) {
            closesocket(fd);
            return SSH_INVALID_SOCKET;
        }
        skip += 1 + len;
    } else {
        closesocket(fd);
        return SSH_INVALID_SOCKET;
    }
    char sink[256];
    const size_t drain = skip < sizeof(sink) ? skip : sizeof(sink);
    if (!recvAll(fd, sink, drain)) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect",
                                                 "SOCKS5 proxy reply truncated");
        }
        closesocket(fd);
        return SSH_INVALID_SOCKET;
    }
    return fd;
}

socket_t connectViaProxy(const SessionConfig &config, QString *error)
{
    if (config.proxyType() == QLatin1String("http")) {
        return connectViaHttpProxy(config, error);
    }
    if (config.proxyType() == QLatin1String("socks5")) {
        return connectViaSocks5Proxy(config, error);
    }
    if (error) {
        *error = QCoreApplication::translate("hssh::SshConnect", "Unknown proxy type: %1")
                     .arg(config.proxyType());
    }
    return SSH_INVALID_SOCKET;
}

#ifndef Q_OS_WIN
inline void closesocket(socket_t fd) { ::close(fd); }
#endif

// Loopback socket pair: Windows has no socketpair(), so emulate it with a
// short-lived TCP listener on 127.0.0.1.
bool makeSocketPair(socket_t *a, socket_t *b)
{
#ifdef Q_OS_WIN
    const socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == SSH_INVALID_SOCKET) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0
        || ::listen(listener, 1) != 0) {
        closesocket(listener);
        return false;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(listener, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        closesocket(listener);
        return false;
    }
    const socket_t client = ::socket(AF_INET, SOCK_STREAM, 0);
    if (client == SSH_INVALID_SOCKET
        || ::connect(client, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        closesocket(listener);
        if (client != SSH_INVALID_SOCKET) {
            closesocket(client);
        }
        return false;
    }
    const socket_t server = ::accept(listener, nullptr, nullptr);
    closesocket(listener);
    if (server == SSH_INVALID_SOCKET) {
        closesocket(client);
        return false;
    }
    *a = client; // pump end
    *b = server; // handed to the target ssh_session
    return true;
#else
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return false;
    }
    *a = sv[0];
    *b = sv[1];
    return true;
#endif
}

// PH1-03: jump-host tunnel. Owns the jump ssh_session and a direct-tcpip
// channel to the target, plus a pump thread that shuttles bytes between
// the channel and the local socket end. The tunnel self-destructs when
// either side closes (the target session closing its fd unblocks the
// pump), so no external lifetime management is needed.
struct JumpTunnel {
    ssh_session jump = nullptr;
    ssh_channel channel = nullptr;
    socket_t local = SSH_INVALID_SOCKET;
    std::thread pump;
};

void jumpPumpRun(JumpTunnel *t)
{
    char buffer[32 * 1024];
    bool alive = true;
    while (alive) {
        // Local socket -> channel (select so the loop stays responsive).
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(t->local, &readSet);
        timeval tv{};
        tv.tv_usec = 5000; // 5 ms
        if (::select(static_cast<int>(t->local) + 1, &readSet, nullptr, nullptr, &tv) > 0) {
            const int n = ::recv(t->local, buffer, sizeof(buffer), 0);
            if (n <= 0) {
                break; // target session closed its end
            }
            int off = 0;
            while (off < n) {
                // Blocking full write: the jump session keeps timeout 0
                // (bulk-transfer rule — no silent short writes).
                const int w = ssh_channel_write(t->channel, buffer + off,
                                                static_cast<uint32_t>(n - off));
                if (w <= 0) {
                    alive = false;
                    break;
                }
                off += w;
            }
            if (!alive) {
                break;
            }
        }
        // Channel -> local socket (nonblocking drain).
        const int n = ssh_channel_read_nonblocking(t->channel, buffer, sizeof(buffer), 0);
        if (n < 0) {
            break;
        }
        if (n > 0 && !sendAll(t->local, buffer, static_cast<size_t>(n))) {
            break;
        }
        if (ssh_channel_is_eof(t->channel) || ssh_channel_is_closed(t->channel)) {
            break;
        }
    }
    ::shutdown(t->local, 2);
    closesocket(t->local);
    ssh_channel_close(t->channel);
    ssh_channel_free(t->channel);
    ssh_disconnect(t->jump);
    ssh_free(t->jump);
    delete t; // self-destruct
}

// Connects through the jump host and returns the fd the TARGET session
// should use (SSH_OPTIONS_FD). nullptr-safe: on failure fills *error.
socket_t jumpTunnelFd(const SessionConfig &config, QString *error,
                      const KeyStore::HostKeyVerifier &hostKeyVerifier)
{
    // Build the jump session's own config: jump credentials come from the
    // jump fields; the target's proxy does not apply to the jump leg.
    SessionConfig jump = config;
    jump.setHost(config.jumpHost());
    jump.setPort(config.jumpPort());
    jump.setUsername(config.jumpUsername().isEmpty()
                         ? config.username()
                         : config.jumpUsername());
    if (!config.jumpPrivateKeyPath().isEmpty()) {
        jump.setAuthMethod(AuthMethod::PublicKey);
        jump.setPrivateKeyPath(config.jumpPrivateKeyPath());
    } else {
        jump.setAuthMethod(AuthMethod::Password);
        jump.setPassword(config.jumpPassword());
    }
    jump.setProxyType(QString());
    jump.setJumpHost(QString()); // no chaining (v1)

    // Bulk timeout 0: the tunnel carries bulk transfers later.
    ssh_session jumpSession = sshConnectAndAuthenticate(jump, error, 0, hostKeyVerifier);
    if (!jumpSession) {
        return SSH_INVALID_SOCKET;
    }
    ssh_channel channel = ssh_channel_new(jumpSession);
    if (!channel) {
        if (error) {
            *error = QString::fromUtf8(ssh_get_error(jumpSession));
        }
        ssh_disconnect(jumpSession);
        ssh_free(jumpSession);
        return SSH_INVALID_SOCKET;
    }
    const QByteArray targetHost = config.host().toUtf8();
    if (ssh_channel_open_forward(channel, targetHost.constData(),
                                  config.port(),
                                  "127.0.0.1", 0) != SSH_OK) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect",
                                                 "Jump host failed to reach %1:%2: %3")
                         .arg(config.host())
                         .arg(config.port())
                         .arg(QString::fromUtf8(ssh_get_error(jumpSession)));
        }
        ssh_channel_free(channel);
        ssh_disconnect(jumpSession);
        ssh_free(jumpSession);
        return SSH_INVALID_SOCKET;
    }

    socket_t local = SSH_INVALID_SOCKET;
    socket_t target = SSH_INVALID_SOCKET;
    if (!makeSocketPair(&local, &target)) {
        if (error) {
            *error = QCoreApplication::translate("hssh::SshConnect",
                                                 "Failed to create jump tunnel sockets");
        }
        ssh_channel_free(channel);
        ssh_disconnect(jumpSession);
        ssh_free(jumpSession);
        return SSH_INVALID_SOCKET;
    }

    auto *tunnel = new JumpTunnel{jumpSession, channel, local, std::thread()};
    tunnel->pump = std::thread(jumpPumpRun, tunnel);
    tunnel->pump.detach();
    return target;
}

} // namespace

ssh_session sshConnectAndAuthenticate(const SessionConfig &config, QString *errorMessage,
                                      long postConnectTimeoutSec,
                                      const KeyStore::HostKeyVerifier &hostKeyVerifier,
                                      const KbdintPrompter &kbdintPrompter)
{
    ssh_session session = ssh_new();
    if (!session) {
        if (errorMessage) {
            *errorMessage = QCoreApplication::translate("hssh::SshConnect", "Failed to create SSH session");
        }
        return nullptr;
    }

    const QByteArray host = config.host().toUtf8();
    ssh_options_set(session, SSH_OPTIONS_HOST, host.constData());
    int port = config.port();
    ssh_options_set(session, SSH_OPTIONS_PORT, &port);
    if (!config.username().isEmpty()) {
        const QByteArray user = config.username().toUtf8();
        ssh_options_set(session, SSH_OPTIONS_USER, user.constData());
    }
    // Without a timeout ssh_connect can block for minutes on an
    // unreachable host.
    long timeout = 10;
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);

    // PH1-02/03: pre-connect the transport ourselves — through a jump host
    // (direct-tcpip tunnel, takes precedence) or an HTTP/SOCKS5 proxy —
    // and hand the established fd to libssh via SSH_OPTIONS_FD. libssh
    // then skips its own connect and runs the SSH handshake over it.
    socket_t preconnected = SSH_INVALID_SOCKET;
    if (!config.jumpHost().isEmpty()) {
        preconnected = jumpTunnelFd(config, errorMessage, hostKeyVerifier);
        if (preconnected == SSH_INVALID_SOCKET) {
            ssh_free(session);
            return nullptr;
        }
    } else if (!config.proxyType().isEmpty() && config.proxyType() != QLatin1String("none")) {
        preconnected = connectViaProxy(config, errorMessage);
        if (preconnected == SSH_INVALID_SOCKET) {
            ssh_free(session);
            return nullptr;
        }
    }
    if (preconnected != SSH_INVALID_SOCKET) {
        ssh_options_set(session, SSH_OPTIONS_FD, &preconnected);
    }

    int rc = ssh_connect(session);
    if (rc != SSH_OK) {
        if (errorMessage) {
            *errorMessage = QString::fromUtf8(ssh_get_error(session));
        }
        ssh_free(session);
        return nullptr;
    }

    // The timeout above must apply to ssh_connect ONLY. libssh reuses
    // SSH_OPTIONS_TIMEOUT for every later blocking call (channel window
    // waits, reads, flushes): on a congested link a channel window that
    // takes >10 s to drain makes ssh_channel_write return a SHORT count
    // without an error, and sftp_write only logs that — the SFTP byte
    // stream desyncs and uploads get silently corrupted. Callers choose the
    // post-connect behavior: 0 waits indefinitely (mandatory for bulk
    // transfers), a positive value bounds channel operations so a wedged
    // sshd cannot hang an exec worker forever.
    ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &postConnectTimeoutSec);

    // PH2-12: verify the server key before handing over credentials.
    if (!KeyStore::verifyAndStoreHostKey(session, hostKeyVerifier, errorMessage)) {
        ssh_disconnect(session);
        ssh_free(session);
        return nullptr;
    }

    QString authDetail;
    rc = authenticate(session, config, kbdintPrompter, &authDetail);
    if (rc != SSH_AUTH_SUCCESS) {
        if (errorMessage) {
            *errorMessage = authDetail.isEmpty()
                                ? QCoreApplication::translate("hssh::SshConnect",
                                                              "Authentication failed")
                                : QCoreApplication::translate("hssh::SshConnect",
                                                              "Authentication failed: %1")
                                      .arg(authDetail);
        }
        ssh_disconnect(session);
        ssh_free(session);
        return nullptr;
    }

    return session;
}

void sshDisconnectAndFree(ssh_session session)
{
    if (session) {
        ssh_disconnect(session);
        ssh_free(session);
    }
}

} // namespace hssh

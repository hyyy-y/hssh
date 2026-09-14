#include "SshConnect.h"

#include "SessionConfig.h"

#include <QCoreApplication>

#include <libssh/libssh.h>

namespace hssh {

namespace {

int authenticate(ssh_session session, const SessionConfig &config)
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

    if (method == AuthMethod::Password || method == AuthMethod::KeyboardInteractive) {
        const QString password = config.password().toString();
        return ssh_userauth_password(session, nullptr, password.toUtf8().constData());
    }

    return SSH_AUTH_DENIED;
}

} // namespace

ssh_session sshConnectAndAuthenticate(const SessionConfig &config, QString *errorMessage,
                                      long postConnectTimeoutSec)
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

    rc = authenticate(session, config);
    if (rc != SSH_AUTH_SUCCESS) {
        if (errorMessage) {
            *errorMessage = QCoreApplication::translate("hssh::SshConnect", "Authentication failed");
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

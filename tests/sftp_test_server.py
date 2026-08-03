# Minimal SSH+SFTP test server for the hssh integration tests.
# Listens on 127.0.0.1:2222, user "test", password "test123",
# SFTP rooted at a temp directory. Not for any other use.

import os
import socket
import sys
import tempfile
import threading
import time
import logging

import paramiko
# paramiko 5.0 renamed SFTPAttr to SFTPAttributes.
try:
    from paramiko.sftp_attr import SFTPAttributes as SFTPAttr
except ImportError:
    from paramiko import SFTPAttr  # type: ignore[attr-defined]

logging.basicConfig(
    level=logging.DEBUG,
    filename=os.path.join(tempfile.gettempdir(), "hssh_sftp_paramiko.log"),
    filemode="w",
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)

HOST_KEY = paramiko.RSAKey.generate(2048)
ROOT = tempfile.mkdtemp(prefix="hssh_sftp_root_")


class Server(paramiko.ServerInterface):
    def check_auth_password(self, username, password):
        if username == "test" and password == "test123":
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED

    def get_allowed_auths(self, username):
        return "password"

    def check_channel_request(self, kind, chanid):
        if kind == "session":
            return paramiko.OPEN_SUCCEEDED
        return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED


class StubSFTPHandle(paramiko.SFTPHandle):
    def stat(self):
        try:
            return SFTPAttr.from_stat(os.fstat(self.readfile.fileno()))
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno if isinstance(e.errno, int) else paramiko.ENOENT)

    def chattr(self, attr):
        return paramiko.SFTP_OK


class StubSFTPServer(paramiko.SFTPServerInterface):
    ROOT = ROOT

    def _realpath(self, path):
        return self.ROOT + "/" + self.canonicalize(path)

    def list_folder(self, path):
        real = self._realpath(path)
        print(f"list_folder({path}) -> {real}", flush=True)
        try:
            out = []
            for fname in os.listdir(real):
                attr = SFTPAttr.from_stat(os.stat(os.path.join(real, fname)))
                attr.filename = fname
                out.append(attr)
            return out
        except OSError as e:
            print(f"list_folder error: {e}", flush=True)
            return paramiko.SFTPServer.convert_errno(e.errno)

    def stat(self, path):
        print(f"stat({path})", flush=True)
        try:
            return SFTPAttr.from_stat(os.stat(self._realpath(path)))
        except OSError as e:
            print(f"stat error: {e}", flush=True)
            return paramiko.SFTPServer.convert_errno(e.errno)

    def lstat(self, path):
        return self.stat(path)

    def open(self, path, flags, attr):
        print(f"open({path}, flags={flags})", flush=True)
        real = self._realpath(path)
        try:
            binary_flag = getattr(os, "O_BINARY", 0)
            flags |= binary_flag
            mode = getattr(attr, "st_mode", None) or 0o666
            fd = os.open(real, flags, mode)
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)
        if (flags & os.O_CREAT) and attr is not None:
            paramiko.SFTPServer.set_file_attr(real, attr)
        if flags & os.O_WRONLY:
            fstr = "ab" if flags & os.O_APPEND else "wb"
        elif flags == os.O_RDONLY:
            fstr = "rb"
        else:
            fstr = "rb+"
        try:
            f = os.fdopen(fd, fstr)
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)
        handle = StubSFTPHandle(flags)
        handle.filename = real
        handle.readfile = f
        handle.writefile = f
        return handle

    def remove(self, path):
        print(f"remove({path})", flush=True)
        try:
            os.remove(self._realpath(path))
        except OSError as e:
            print(f"remove error: {e}", flush=True)
            return paramiko.SFTPServer.convert_errno(e.errno)
        return paramiko.SFTP_OK

    def rename(self, oldpath, newpath):
        print(f"rename({oldpath} -> {newpath})", flush=True)
        try:
            os.rename(self._realpath(oldpath), self._realpath(newpath))
        except OSError as e:
            print(f"rename error: {e}", flush=True)
            return paramiko.SFTPServer.convert_errno(e.errno)
        return paramiko.SFTP_OK

    def mkdir(self, path, attr):
        print(f"mkdir({path})", flush=True)
        try:
            os.mkdir(self._realpath(path))
            if attr is not None:
                paramiko.SFTPServer.set_file_attr(self._realpath(path), attr)
        except OSError as e:
            print(f"mkdir error: {e}", flush=True)
            return paramiko.SFTPServer.convert_errno(e.errno)
        return paramiko.SFTP_OK

    def rmdir(self, path):
        print(f"rmdir({path})", flush=True)
        try:
            os.rmdir(self._realpath(path))
        except OSError as e:
            print(f"rmdir error: {e}", flush=True)
            return paramiko.SFTPServer.convert_errno(e.errno)
        return paramiko.SFTP_OK

    def canonicalize(self, path):
        if os.path.isabs(path):
            out = os.path.normpath(path)
        else:
            out = os.path.normpath("./" + path)
        if os.path.isabs(out):
            out = out.replace(os.sep, "/")
            # Strip the drive letter so SFTP paths stay posix-like.
            if len(out) > 2 and out[1] == ":":
                out = out[2:]
        else:
            out = out.replace(os.sep, "/")
        return out


def handle_client(client):
    transport = paramiko.Transport(client)
    transport.add_server_key(HOST_KEY)
    transport.set_subsystem_handler("sftp", paramiko.SFTPServer, StubSFTPServer)
    server = Server()
    try:
        transport.start_server(server=server)
        # The subsystem handler accepts the sftp channel itself; do NOT call
        # transport.accept() here or it races the handler and closes the
        # channel underneath the client.
        while transport.is_active():
            time.sleep(1)
    except Exception as e:  # noqa: BLE001
        print("client error:", e, file=sys.stderr)
    finally:
        transport.close()


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", 2222))
    sock.listen(10)
    print(f"SFTP test server on 127.0.0.1:2222, root={ROOT}", flush=True)
    try:
        while True:
            client, _ = sock.accept()
            threading.Thread(target=handle_client, args=(client,), daemon=True).start()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()

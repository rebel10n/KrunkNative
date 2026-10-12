#!/usr/bin/env python3
"""Fake SMTP submission server for the knsvc SMTP client and the brain's email tests.

  from fake_smtp import FakeSMTP, make_cert
  cert, key = make_cert(tmpdir)                  # self-signed for 127.0.0.1 / localhost (openssl CLI)
  srv = FakeSMTP(mode="starttls", cert=cert, key=key, users={"u": "p"}).start()
  ... srv.port ... srv.messages (list of dicts) ... srv.wait_messages(1) ... srv.stop()

Modes: "none" (clear text), "starttls" (STARTTLS offered and required before AUTH / MAIL),
"implicit" (TLS from the first byte, like port 465). Supports EHLO/HELO, AUTH PLAIN (inline and
continuation) and LOGIN, MAIL, RCPT, DATA (dot-unstuffing), RSET, NOOP, QUIT. Knobs: users (None =
no AUTH required), reject_rcpt (addresses answered 550), temp_fail (first n MAIL commands answered
451), sender_domains (MAIL FROM outside them -> 553, like a relay that only accepts its own domain),
inject_after_starttls (sends extra bytes with the 220: the client must refuse).
Each message: {mail_from, rcpt, data (bytes, unstuffed), tls, auth_mech, auth_user}.
Standalone: fake_smtp.py <mode> [port] prints "port <n>" and every message as JSON (manual tests).
"""
import base64
import json
import os
import socket
import ssl
import subprocess
import sys
import threading
import time


def make_cert(directory):
    cert = os.path.join(directory, "smtp-cert.pem")
    key = os.path.join(directory, "smtp-key.pem")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", cert,
                    "-days", "2", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost,IP:127.0.0.1"],
                   check=True, capture_output=True, timeout=60)
    return cert, key


class _Session:
    def __init__(self, srv, conn):
        self.srv, self.conn = srv, conn
        self.tls = srv.mode == "implicit"
        self.buf = b""
        self.auth_mech = self.auth_user = None
        self.mail_from = None
        self.rcpt = []

    def send(self, line):
        self.conn.sendall(line.encode() + b"\r\n")

    def readline(self):
        while b"\n" not in self.buf:
            chunk = self.conn.recv(65536)
            if not chunk:
                raise EOFError
            self.buf += chunk
            if len(self.buf) > 1 << 20:
                raise EOFError
        line, self.buf = self.buf.split(b"\n", 1)
        return line.rstrip(b"\r")

    def ehlo(self):
        caps = ["fake.smtp.test greets you"]
        if self.srv.mode == "starttls" and not self.tls:
            caps.append("STARTTLS")
        if self.srv.users is not None and (self.tls or self.srv.mode == "none"):
            caps.append("AUTH PLAIN LOGIN")
        caps += ["SIZE 10485760", "8BITMIME", "ENHANCEDSTATUSCODES"]
        for i, c in enumerate(caps):
            self.send("250%s%s" % ("-" if i < len(caps) - 1 else " ", c))

    def check_login(self, user, password, mech):
        if self.srv.users.get(user) == password:
            self.auth_mech, self.auth_user = mech, user
            self.send("235 2.7.0 Authentication successful")
        else:
            self.send("535 5.7.8 Authentication credentials invalid")

    def run(self):
        self.send("220 fake.smtp.test ESMTP ready")
        while True:
            raw = self.readline()
            line = raw.decode("utf-8", "replace")
            cmd = line.split(" ", 1)[0].upper()
            arg = line[len(cmd) + 1:] if " " in line else ""
            if cmd in ("EHLO", "HELO"):
                self.ehlo()
            elif cmd == "STARTTLS" and self.srv.mode == "starttls" and not self.tls:
                extra = b"250 injected\r\n" if self.srv.inject_after_starttls else b""
                self.conn.sendall(b"220 2.0.0 Ready to start TLS\r\n" + extra)
                if self.buf:
                    raise EOFError  # pipelined plain text after STARTTLS: drop like a careful server
                self.conn = self.srv.ctx.wrap_socket(self.conn, server_side=True)
                self.tls = True
            elif self.srv.mode == "starttls" and not self.tls and cmd in ("AUTH", "MAIL"):
                self.send("530 5.7.0 Must issue a STARTTLS command first")
            elif cmd == "AUTH":
                parts = arg.split()
                mech = parts[0].upper() if parts else ""
                if self.srv.users is None:
                    self.send("503 5.5.1 AUTH not available")
                elif mech == "PLAIN":
                    payload = parts[1] if len(parts) > 1 else None
                    if payload is None:
                        self.send("334 ")
                        payload = self.readline().decode()
                    try:
                        _, user, password = base64.b64decode(payload).decode().split("\0")
                    except ValueError:
                        self.send("501 5.5.2 bad PLAIN payload")
                        continue
                    self.check_login(user, password, "PLAIN")
                elif mech == "LOGIN":
                    self.send("334 VXNlcm5hbWU6")
                    user = base64.b64decode(self.readline()).decode()
                    self.send("334 UGFzc3dvcmQ6")
                    password = base64.b64decode(self.readline()).decode()
                    self.check_login(user, password, "LOGIN")
                else:
                    self.send("504 5.5.4 mechanism not supported")
            elif cmd == "MAIL":
                if self.srv.users is not None and not self.auth_user:
                    self.send("530 5.7.0 Authentication required")
                    continue
                addr = arg.split(":", 1)[1].strip().strip("<>") if ":" in arg else ""
                if self.srv.sender_domains and addr.rsplit("@", 1)[-1].lower() not in self.srv.sender_domains:
                    self.send("553 5.7.1 Sender address rejected: not owned by user")
                    continue
                with self.srv.lock:
                    self.srv.mail_commands += 1
                    temp = self.srv.mail_commands <= self.srv.temp_fail
                if temp:
                    self.send("451 4.3.0 Try again later")
                    continue
                self.mail_from, self.rcpt = addr, []
                self.send("250 2.1.0 Ok")
            elif cmd == "RCPT":
                addr = arg.split(":", 1)[1].strip().strip("<>") if ":" in arg else ""
                if self.mail_from is None:
                    self.send("503 5.5.1 need MAIL")
                elif addr.lower() in self.srv.reject_rcpt:
                    self.send("550 5.1.1 <%s>: Recipient address rejected" % addr)
                else:
                    self.rcpt.append(addr)
                    self.send("250 2.1.5 Ok")
            elif cmd == "DATA":
                if not self.rcpt:
                    self.send("503 5.5.1 need RCPT")
                    continue
                self.send("354 End data with <CR><LF>.<CR><LF>")
                lines = []
                while True:
                    l = self.readline()
                    if l == b".":
                        break
                    lines.append(l[1:] if l.startswith(b".") else l)
                msg = {"mail_from": self.mail_from, "rcpt": list(self.rcpt), "data": b"\r\n".join(lines) + b"\r\n",
                       "tls": self.tls, "auth_mech": self.auth_mech, "auth_user": self.auth_user}
                with self.srv.lock:
                    self.srv.messages.append(msg)
                    self.srv.cond.notify_all()
                self.mail_from, self.rcpt = None, []
                self.send("250 2.0.0 Ok: queued as FAKE%d" % len(self.srv.messages))
            elif cmd == "RSET":
                self.mail_from, self.rcpt = None, []
                self.send("250 2.0.0 Ok")
            elif cmd == "NOOP":
                self.send("250 2.0.0 Ok")
            elif cmd == "QUIT":
                self.send("221 2.0.0 Bye")
                return
            else:
                self.send("502 5.5.2 Error: command not recognized")


class FakeSMTP:
    def __init__(self, mode="starttls", cert=None, key=None, users=None, reject_rcpt=(), temp_fail=0,
                 sender_domains=(), inject_after_starttls=False, host="127.0.0.1", port=0):
        self.mode, self.users = mode, users
        self.reject_rcpt = {a.lower() for a in reject_rcpt}
        self.sender_domains = {d.lower() for d in sender_domains}
        self.temp_fail, self.inject_after_starttls = temp_fail, inject_after_starttls
        self.mail_commands = 0
        self.messages = []
        self.lock = threading.Lock()
        self.cond = threading.Condition(self.lock)
        self.ctx = None
        if mode in ("starttls", "implicit"):
            self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.ctx.load_cert_chain(cert, key)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        self.running = False

    def start(self):
        self.running = True
        threading.Thread(target=self._accept, daemon=True).start()
        return self

    def _accept(self):
        while self.running:
            try:
                conn, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(conn,), daemon=True).start()

    def _serve(self, conn):
        conn.settimeout(30)
        try:
            if self.mode == "implicit":
                conn = self.ctx.wrap_socket(conn, server_side=True)
            _Session(self, conn).run()
        except (EOFError, OSError, ssl.SSLError, UnicodeError, ValueError):
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass

    def wait_messages(self, n, timeout=15.0):
        deadline = time.time() + timeout
        with self.lock:
            while len(self.messages) < n and time.time() < deadline:
                self.cond.wait(max(0.01, deadline - time.time()))
            return list(self.messages)

    def stop(self):
        self.running = False
        try:
            self.sock.close()
        except OSError:
            pass


if __name__ == "__main__":
    import tempfile
    mode = sys.argv[1] if len(sys.argv) > 1 else "none"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    cert = key = None
    if mode != "none":
        cert, key = make_cert(tempfile.mkdtemp(prefix="fake-smtp-", dir=os.path.expanduser("~/.cache")))
    srv = FakeSMTP(mode, cert, key, port=port).start()
    print("port %d" % srv.port, flush=True)
    seen = 0
    while True:
        msgs = srv.wait_messages(seen + 1, timeout=3600)
        for m in msgs[seen:]:
            print(json.dumps(dict(m, data=m["data"].decode("utf-8", "replace"))), flush=True)
        seen = len(msgs)

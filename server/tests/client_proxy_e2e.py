#!/usr/bin/env python3
"""Settings > Client > Proxy and the official krunker.io lobbies of the native server browser, end to end
(client/include/http_proxy.h, client/include/krunker_lobbies.h).

  python3 -I server/tests/client_proxy_e2e.py BUILD_DIR      (from the repository root)

Stubs (Python, ports from kn_proto.free_ports()): Krunker's matchmaker (GET /game-list), a krunker.io
block page (403 "ACCESS DENIED" HTML) and two proxies that speak HTTP CONNECT and SOCKS5 (one reaches the
matchmaker stub, one the block page, whatever host the client asks for). The client asks for
http://matchmaker.krunker.io:<port>, so only a proxy can reach the stub: nothing leaves the machine.

  1 proxy set in Settings > Client (knClientSet): the official list loads through it (the proxy saw
    CONNECT matchmaker.krunker.io:<port>), the Servers window lists the KRUNKER.IO groups although the
    Server Type is KrunkNative (the default), the basic Settings view shows the Server Type row
  2 restart with the same config: the proxy persisted (knClient storage, the Client tab's row) and the
    list loads through it again
  3 no proxy, krunker.io blocks this network: the KRUNKER.IO section says so instead of an empty list
  4 live: a proxy whose exit is blocked too, then another one set without a restart: the message, then
    the lobbies; socks5h:// (the proxy resolves the name)
  5 the environment when the setting is empty: HTTP_PROXY carries the request (Proxy Scope "all" is
    not needed for it)

Each run is one headless offline client_bin (KRUNKNATIVE_HEADLESS=1, KRUNKNATIVE_AUDIO_BACKEND=null) driven
by an input script. Exit 0 when every check passed; logs are kept in ~/.cache/kn-client-proxy-e2e-* on failure.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import re
import shutil
import signal
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-client-proxy-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
MM_PORT, BLOCK_PORT, PX_GOOD, PX_BLOCKED = kn_proto.free_ports(4)
MM_URL = "http://matchmaker.krunker.io:%d" % MM_PORT
results = []

STUB_GAMES = {"success": True, "totalPlayerCount": 21, "games": [
    ["FRA:aaaaa", "de-fra", 5, 8, {"i": "Burg", "g": 0, "c": 0, "v": "x"}, 120],
    ["NY:bbbbb", "us-nj", 3, 8, {"i": "Sandstorm", "g": 1, "c": 0, "v": "x"}, 200],
    ["TOK:ddddd", "jb-hnd", 3, 8, {"i": "Littletown", "g": 3, "c": 0, "v": "x"}, 50],
]}
BLOCK_PAGE = (b"<!DOCTYPE html><html><head><title>Access Denied</title></head><body><h1>ACCESS DENIED</h1>"
              b"<p>Your network has been blocked.</p></body></html>")
mm_hits = []
tunnels = []  # (proxy port, kind, target)


class Matchmaker(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        mm_hits.append((self.headers.get("Host"), self.path))
        if self.path.startswith("/game-list"):
            body = json.dumps(STUB_GAMES).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
        else:
            body = b"not found"
            self.send_response(404)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass


class Blocked(BaseHTTPRequestHandler):
    def do_GET(self):  # noqa: N802
        self.send_response(403)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(BLOCK_PAGE)))
        self.end_headers()
        self.wfile.write(BLOCK_PAGE)

    def log_message(self, *args):
        pass


def recv_exact(s, n):
    data = b""
    while len(data) < n:
        chunk = s.recv(n - len(data))
        if not chunk:
            raise ConnectionError("closed")
        data += chunk
    return data


def pipe(a, b):
    try:
        while True:
            data = a.recv(65536)
            if not data:
                break
            b.sendall(data)
    except OSError:
        pass
    finally:
        for s in (a, b):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


class Proxy(socketserver.BaseRequestHandler):
    """HTTP CONNECT or SOCKS5 (by the first byte); every tunnel goes to the server's backend port."""

    def handle(self):
        c = self.request
        c.settimeout(10)
        first = c.recv(1, socket.MSG_PEEK)
        if first == b"\x05":
            _, n = recv_exact(c, 2)
            recv_exact(c, n)
            c.sendall(b"\x05\x00")
            atyp = recv_exact(c, 4)[3]
            if atyp == 3:
                target = recv_exact(c, recv_exact(c, 1)[0]).decode()
                kind = "socks5h"
            elif atyp == 1:
                target = socket.inet_ntoa(recv_exact(c, 4))
                kind = "socks5"
            else:
                target = socket.inet_ntop(socket.AF_INET6, recv_exact(c, 16))
                kind = "socks5"
            port = int.from_bytes(recv_exact(c, 2), "big")
            tunnels.append((self.server.server_address[1], kind, "%s:%d" % (target, port)))
            c.sendall(b"\x05\x00\x00\x01\x7f\x00\x00\x01\x00\x00")
        else:
            head = b""
            while b"\r\n\r\n" not in head:
                chunk = c.recv(1)
                if not chunk:
                    return
                head += chunk
            line = head.split(b"\r\n", 1)[0].decode()
            method, target, _ = line.split(" ", 2)
            if method != "CONNECT":
                c.sendall(b"HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n")
                return
            tunnels.append((self.server.server_address[1], "connect", target))
            c.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
        up = socket.create_connection(("127.0.0.1", self.server.backend), timeout=10)
        c.settimeout(None)
        up.settimeout(None)
        t = threading.Thread(target=pipe, args=(up, c), daemon=True)
        t.start()
        pipe(c, up)
        t.join(5)
        up.close()


class ProxyServer(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, port, backend):
        self.backend = backend
        super().__init__(("127.0.0.1", port), Proxy)


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


def run(name, script, cfg, extra_env=None, timeout=120):
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL") and not k.lower().endswith("_proxy")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path,
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_KRUNKER_MATCHMAKER": MM_URL,
                "KRUNKNATIVE_FINDER_URL": "off"})
    env.update(extra_env or {})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline"], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(f"{name}: client_bin finished within {timeout} s", False)
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def probe(log, label):
    m = re.search(r"\[input_script\] text t=\d+ @" + re.escape(label) + r" n=(\d+) checked=(\d) value=(.*?) text=(.*)", log)
    return {"n": int(m.group(1)), "value": m.group(3), "text": m.group(4)} if m else {"n": -1, "value": None, "text": ""}


def nodes(log, label):
    m = re.search(r"\[input_script\] nodes t=\d+ @" + re.escape(label) + r" elements=\d+ match=(\d+)", log)
    return int(m.group(1)) if m else -1


def seen(port, kind, since=0):
    return [t for t in tunnels[since:] if t[0] == port and t[1] == kind and t[2] == "matchmaker.krunker.io:%d" % MM_PORT]


def storage(cfg, key):
    try:
        with open(os.path.join(cfg, "krunknative", key + ".json")) as f:
            return f.read()
    except OSError:
        return ""


DENIED = "krunker.io refused this network (ACCESS DENIED). Set a proxy in Settings > Client."
OPEN_TEXT = "KRUNKER.IO krunker.io servers: pick one to join it here"  # the in-client default (krunker_net.h)


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("client_proxy_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    servers = [ThreadingHTTPServer(("127.0.0.1", MM_PORT), Matchmaker), ThreadingHTTPServer(("127.0.0.1", BLOCK_PORT), Blocked),
               ProxyServer(PX_GOOD, MM_PORT), ProxyServer(PX_BLOCKED, BLOCK_PORT)]
    for s in servers:
        threading.Thread(target=s.serve_forever, daemon=True).start()
    try:
        # ---- 1: the proxy set in Settings > Client; the official list through it
        cfg = os.path.join(WORK, "cfg")
        since = len(tunnels)
        log = run("set", """
0 menu
100 call showWindow 1
600 text @type [id=knSeekServers]
700 call knClientSet "proxy" " http://127.0.0.1:%d "
800 call showWindow 2
5000 text @status [id=kioStatus]
5100 nodes @kgroups .quickJoin[title*=krunker]
5150 text @head .setHed
5200 quit
""" % PX_GOOD, cfg)
        typ = probe(log, "type")
        check("1: the basic Settings view has the Server Type row (default KrunkNative)", typ["n"] == 1 and typ["value"] == "kn", str(typ))
        check("1: the setting is applied at once (logged)", "[http] proxy http://127.0.0.1:%d for krunker.io" % PX_GOOD in log, log[-400:])
        check("1: the official list went through the proxy (CONNECT matchmaker.krunker.io)", len(seen(PX_GOOD, "connect", since)) >= 1,
              str(tunnels[since:]))
        check("1: ... and reached the matchmaker", any(h == "matchmaker.krunker.io:%d" % MM_PORT for h, _ in mm_hits), str(mm_hits))
        check("1: the client logs the lobbies via the proxy",
              re.search(r"\[krunker\] 3 official lobbies .* via proxy http://127\.0\.0\.1:%d" % PX_GOOD, log), log[-400:])
        st = probe(log, "status")
        check("1: the KRUNKER.IO section with Server Type KrunkNative", st["n"] == 1 and OPEN_TEXT in st["text"], str(st))
        check("1: its groups are listed (one per Krunker region)", nodes(log, "kgroups") == 3, str(nodes(log, "kgroups")))
        head = probe(log, "head")["text"]
        check("1: the first one is krunker.io - Frankfurt (the most players)", "krunker.io - Frankfurt" in head, head)
        stored = storage(cfg, "knClient")
        check("1: the proxy persisted trimmed in knClient", '"proxy":"http://127.0.0.1:%d"' % PX_GOOD in stored, stored[:300])

        # ---- 2: restart with the same config
        since = len(tunnels)
        log = run("restart", """
0 menu
100 call showWindow 1
300 call windows.0.changeTab 4
800 text @proxy [id=knClient_proxy]
900 call showWindow 2
5000 text @status [id=kioStatus]
5100 quit
""", cfg)
        check("2: the proxy is configured at startup", "[http] proxy http://127.0.0.1:%d for krunker.io" % PX_GOOD in log, log[-400:])
        row = probe(log, "proxy")
        check("2: the Client tab's Proxy row shows it", row["n"] == 1 and row["value"] == "http://127.0.0.1:%d" % PX_GOOD, str(row))
        check("2: the list went through the proxy again", len(seen(PX_GOOD, "connect", since)) >= 1, str(tunnels[since:]))
        check("2: the lobbies are listed", OPEN_TEXT in probe(log, "status")["text"], probe(log, "status")["text"])

        # ---- 3: no proxy, krunker.io blocks this network
        log = run("blocked", """
0 menu
100 call showWindow 2
4000 text @status [id=kioStatus]
4100 quit
""", os.path.join(WORK, "cfg-blocked"), {"KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d" % BLOCK_PORT})
        st = probe(log, "status")
        check("3: the KRUNKER.IO section says krunker.io refused this network", st["n"] == 1 and DENIED in st["text"], str(st))
        check("3: logged", "[krunker] game list failed (HTTP 403)" in log, log[-400:])

        # ---- 4: live: a blocked proxy, then socks5h:// to a working one, no restart
        cfg4 = os.path.join(WORK, "cfg-live")
        since = len(tunnels)
        log = run("live", """
0 menu
100 call knClientSet "proxy" "http://127.0.0.1:%d"
200 call showWindow 2
4000 text @first [id=kioStatus]
4100 call knClientSet "proxy" "socks5h://127.0.0.1:%d"
4200 call knClientSet "proxy" "ftp://nope"
8000 text @second [id=kioStatus]
8100 quit
""" % (PX_BLOCKED, PX_GOOD), cfg4)
        first = probe(log, "first")["text"]
        check("4: a proxy whose exit is blocked too: said so", "ACCESS DENIED), also through the proxy http://127.0.0.1:%d" % PX_BLOCKED in first, first)
        check("4: socks5h:// handed the proxy the name", len(seen(PX_GOOD, "socks5h", since)) >= 1, str(tunnels[since:]))
        second = probe(log, "second")["text"]
        check("4: the lobbies after the change, without a restart", OPEN_TEXT in second, second)
        check("4: an invalid proxy URL is refused (the setting stays)", "client setting proxy: not a proxy URL: ftp://nope" in log
              and '"proxy":"socks5h://127.0.0.1:%d"' % PX_GOOD in storage(cfg4, "knClient"), storage(cfg4, "knClient")[:200])

        # ---- 5: the environment when the setting is empty
        since = len(tunnels)
        log = run("env", """
0 menu
100 call showWindow 2
4000 text @status [id=kioStatus]
4100 quit
""", os.path.join(WORK, "cfg-env"), {"HTTP_PROXY": "http://127.0.0.1:%d" % PX_GOOD, "NO_PROXY": "example.com"})
        check("5: HTTP_PROXY carried the request (setting empty)", len(seen(PX_GOOD, "connect", since)) >= 1, str(tunnels[since:]))
        check("5: the lobbies are listed", OPEN_TEXT in probe(log, "status")["text"], probe(log, "status")["text"])
    finally:
        for s in servers:
            s.shutdown()
            s.server_close()
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

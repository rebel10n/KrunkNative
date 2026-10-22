#!/usr/bin/env python3
"""Krunker Strike's Missions tab for a signed-in Strike account whose missions answer never comes
(client/src/ui/strike_meta.c bp_tab, strike_meta_tick): the tab says it failed, with Retry, after the Battle Pass's
15 s deadline (BATTLEPASS_LOAD_TIMEOUT_S, backend_ui.h), never "Loading Missions" for good.

  python3 -I client/tests/strike_missions_timeout_e2e.py BUILD_DIR      (repository root)

kn_brain (sqlite, temp dir) signs the account in and serves its Strike economy, as strike_econ_ui_e2e.py does. The
client's brain URL is a local HTTP proxy in front of that brain which holds GET .../missions/me open and never
answers it; everything else is forwarded unchanged. A headless client_bin --flavor strike --offline opens the
Battle Pass's Missions tab, and the tab's text is read once the deadline has passed.
  1. the account is loaded before the tap ("[strike] account" in the log)
  2. the missions request reached the proxy and was held
  3. the tab shows the failed state: "Couldn't load your missions" with a Retry button (before the fix:
     "Loading Missions" for good, and the script's wait for the button runs out)
  4. the log says the request had no answer in 15 s
  5. Retry (bp-tab:1) asks again: a second request reaches the proxy and the tab is loading again
Headless (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs); the ports come from kn_proto.free_ports().
77 = skip (no kn_brain / client_bin).
"""
import http.client
import http.server
import json
import os
import re
import shutil
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.abspath(".")
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
CLIENT = os.path.join(BUILD, "client_bin")
if not (os.access(BRAIN, os.X_OK) and os.access(CLIENT, os.X_OK)):
    print("needs kn_brain and client_bin: skip")
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-missions-", dir=CACHE if os.path.isdir(CACHE) else None)
STAMP = int(time.time()) % 100000
START = 20000
results = []
procs = []
HELD = threading.Event()  # set at the end: the held requests are let go
held_count = [0]


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def db(path, sql, args=()):
    con = sqlite3.connect(path, timeout=10)
    try:
        cur = con.execute(sql, args)
        con.commit()
        return cur.fetchall()
    finally:
        con.close()


def frame_end(buf):
    """The end offset of the websocket frame at the start of buf (client frames are masked); None while its header is short."""
    if len(buf) < 2:
        return None
    ln = buf[1] & 0x7F
    off = 2
    if ln == 126:
        if len(buf) < 4:
            return None
        ln, off = int.from_bytes(buf[2:4], "big"), 4
    elif ln == 127:
        if len(buf) < 10:
            return None
        ln, off = int.from_bytes(buf[2:10], "big"), 10
    if buf[1] & 0x80:
        off += 4
    return off + ln


def frame_payload(frame):
    """The unmasked payload of a masked client frame."""
    ln = frame[1] & 0x7F
    off = 2
    if ln == 126:
        ln, off = int.from_bytes(frame[2:4], "big"), 4
    elif ln == 127:
        ln, off = int.from_bytes(frame[2:10], "big"), 10
    if not frame[1] & 0x80:
        return frame[off:off + ln]
    key = frame[off:off + 4]
    body = frame[off + 4:off + 4 + ln]
    return bytes(b ^ key[i % 4] for i, b in enumerate(body))


class Proxy(http.server.ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def make_handler(brain_port):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):  # quiet
            pass

        def _forward(self):
            if self.headers.get("Upgrade"):  # the social socket (the login rides on it): a raw pipe to the brain
                self._pipe_upgrade(brain_port)
                return
            if self.command == "GET" and "/missions/me" in self.path:
                held_count[0] += 1
                print(f"[proxy] held {self.command} {self.path}", flush=True)
                HELD.wait()  # never answered: the client's own deadline is what ends it
                return
            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length) if length else None
            headers = {k: v for k, v in self.headers.items() if k.lower() not in ("host", "connection", "content-length")}
            conn = http.client.HTTPConnection("127.0.0.1", brain_port, timeout=30)
            try:
                conn.request(self.command, self.path, body=body, headers=headers)
                resp = conn.getresponse()
                data = resp.read()
            except OSError as e:
                self.send_error(502, str(e))
                return
            finally:
                conn.close()
            self.send_response(resp.status)
            for k, v in resp.getheaders():
                if k.lower() not in ("connection", "transfer-encoding", "content-length", "keep-alive"):
                    self.send_header(k, v)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        do_GET = do_POST = do_PUT = do_DELETE = do_PATCH = _forward

        def _pipe_upgrade(self, port):
            up = socket.create_connection(("127.0.0.1", port), timeout=30)
            up.settimeout(None)
            lines = [f"{self.command} {self.path} {self.request_version}"] + [f"{k}: {v}" for k, v in self.headers.items()]
            up.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
            self.close_connection = True

            def pump(src, dst):
                try:
                    while True:
                        b = src.recv(65536)
                        if not b:
                            break
                        dst.sendall(b)
                except OSError:
                    pass
                finally:
                    for sock in (src, dst):
                        try:
                            sock.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass

            def pump_client(src, dst):  # the client's frames: a missions request is held (not forwarded), the rest pass
                buf = b""
                try:
                    while True:
                        chunk = src.recv(65536)
                        if not chunk:
                            break
                        buf += chunk
                        while True:
                            n = frame_end(buf)
                            if n is None or len(buf) < n:
                                break
                            frame, buf = buf[:n], buf[n:]
                            if b"missions/me" in frame_payload(frame):
                                held_count[0] += 1
                                print("[proxy] held the missions request frame", flush=True)
                            else:
                                dst.sendall(frame)
                except OSError:
                    pass
                finally:
                    for sock in (src, dst):
                        try:
                            sock.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass

            threading.Thread(target=pump, args=(up, self.connection), daemon=True).start()
            pump_client(self.connection, up)
            up.close()

    return Handler


def http_json(port, path, body):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
    try:
        conn.request("POST", path, body=json.dumps(body), headers={"Content-Type": "application/json"})
        resp = conn.getresponse()
        raw = resp.read()
        try:
            return json.loads(raw or b"{}")
        except ValueError:
            return {}
    finally:
        conn.close()


def wait_for(fn, secs):
    end = time.time() + secs
    while time.time() < end:
        if fn():
            return True
        time.sleep(0.2)
    return False


def main():
    brain_port, proxy_port = free_ports(2)
    dbpath = os.path.join(WORK, "brain.db")
    brain_log = os.path.join(WORK, "brain.log")
    proxy = None
    try:
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % brain_port, "--brain.region=local",
                                       "--brain.id=kmt-%d" % os.getpid(), "--accounts.signup_rewards.enabled=false",
                                       "--database.driver=sqlite", "--database.path=" + dbpath, "--migrate",
                                       "--brain.public_url=http://127.0.0.1:%d" % brain_port],
                                      cwd=ROOT, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, start_new_session=True))
        if not wait_for(lambda: "Strike catalogue:" in read(brain_log), 30):
            check("kn_brain starts with the Strike catalogue", False, read(brain_log)[-600:])
            return
        proxy = Proxy(("127.0.0.1", proxy_port), make_handler(brain_port))
        threading.Thread(target=proxy.serve_forever, daemon=True).start()

        stamp = str(STAMP)
        reg = http_json(brain_port, "/auth/register", {"username": "kmt" + stamp, "password": "strike-missions-" + stamp})
        token = reg.get("token", "")
        aid = (reg.get("account") or {}).get("id")
        check("an account registered", bool(token and aid), str(reg)[:200])
        if not (token and aid):
            return
        # a Strike account past the missions unlock (strike_econ_ui_e2e.py's seed)
        db(dbpath, "INSERT INTO strike_wallets (account_id, currency, amount) VALUES (?, 'kr', ?)", (aid, START))
        db(dbpath, "INSERT INTO strike_progress (account_id, xp) VALUES (?, 6000)", (aid,))

        cfg = os.path.join(WORK, "cfg")
        folder = os.path.join(cfg, "krunknative-strike")  # the Strike flavor's own store (storage_dir)
        os.makedirs(folder, exist_ok=True)
        for key, value in (("krunker_username", "kmt" + stamp), ("krunker_token", token), ("consent", "1")):
            with open(os.path.join(folder, key + ".json"), "w") as f:
                f.write(value)
        with open(os.path.join(folder, "knFtue.json"), "w") as f:
            f.write('{"guest":{"played":true}}')
        script = """0 boot
0 waitfor [id=mobMenuPlay]
15000 call strikeAction "missions"
15100 waitfor .tabsContent .frvr-button
15200 text @missions .tabsContent
15300 call strikeAction "bp-tab:1"
15400 text @retry .tabsContent
15500 quit
"""
        path = os.path.join(WORK, "client.script")
        with open(path, "w") as f:
            f.write(script)
        log = os.path.join(WORK, "client.log")
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KN_FLAVOR", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                    "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                    "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_FIXED_DELTA": "16.667",
                    "KRUNKNATIVE_KRUNKER_MATCHMAKER": "off",
                    "KN_FLAVOR": "strike",
                    "KRUNKNATIVE_ALT_KEYSTORE": "file",
                    "KRUNKNATIVE_ASSETS_DIR": os.path.abspath("assets") + os.sep})  # the saved session in a file key (no keyring in a headless run)
        with open(log, "w") as out:
            p = subprocess.Popen([CLIENT, "--flavor", "strike", "--offline", "--brain", "http://127.0.0.1:%d" % proxy_port, "--map", "kanji"], cwd=ROOT, stdout=out,
                                 stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
            try:
                p.wait(timeout=300)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
                check("client_bin finished its script", False, f"timeout, log {log}")
        text = read(log)
        check("the Strike account is loaded before the tap", "[strike] account" in text)
        check("the missions request reached the brain's proxy and was held", held_count[0] >= 1, f"held {held_count[0]}")
        tab = re.findall(r"\[input_script\] text t=\d+ @missions n=\d+ .*? text=(.*)", text)
        shown = tab[-1] if tab else ""
        check("the Missions tab says it failed, with Retry (not Loading Missions)",
              "Couldn't load your missions" in shown and "Retry" in shown and "Loading" not in shown, f"text={shown[:160]!r}")
        check("the log says the answer did not come in 15 s", "[strike] missions: no answer in 15 s" in text)
        # Retry (bp-tab:1) asks again: a second request reaches the proxy and the tab loads once more
        retry = re.findall(r"\[input_script\] text t=\d+ @retry n=\d+ .*? text=(.*)", text)
        again = retry[-1] if retry else ""
        check("Retry re-sends the missions request", held_count[0] >= 2, f"held {held_count[0]}")
        check("after Retry the tab is loading again (not the failed text)", "Loading Missions" in again and "Retry" not in again, f"text={again[:160]!r}")
        print(f"log: {log}")
    finally:
        HELD.set()
        if proxy is not None:
            proxy.shutdown()
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    ok = bool(results) and all(results)
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    main()

#!/usr/bin/env python3
"""A launcher-started client is online: it asks the server finder and the matchmaker, never localhost.

    python3 -I launcher/tests/launcher_online_e2e.py <build dir> [--keep]

A fresh install (BoxOffice, runtime/client_bin + runtime/kn-install.json, runtime/ui linked, game content as
read-only symlinks in a scratch dir) is started through the launcher, headless, with no updates
(KRUNKNATIVE_UPDATES_URL=off). One stub HTTP server on a free port (kn_proto.free_ports) plays
the server finder (GET /servers) and the matchmaker (/mm/status, /mm/ping-list, /mm/seek-game,
...); a server_bin on a second free port is the game server the matchmaker names (as 127.0.0.2).

  1  the client requests the finder URL, lists its servers in the server browser, seeks through
     the matchmaker and connects to the host:port it answered (not 127.0.0.1:21015)
  2  the finder answers 503: the browser shows "Server finder unavailable: HTTP 503" and the log
     names the URL and the reason
  3  the launcher refuses a game content dir inside a git checkout (the sync deletes what it does
     not list) unless --allow-checkout-assets
Regression: a Windows client compiled the brain transport to stubs (no account service, empty
finder, a game connection to 127.0.0.1); client/tests/backend_online_test.c covers that binary.
"""
import argparse
import http.server
import json
import os
import re
import shutil
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import signal

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "server", "tests"))
import kn_proto  # noqa: E402  (free_ports)

FAILS = []


def check(cond, what):
    print(("  ok    " if cond else "  FAIL  ") + what, flush=True)
    if not cond:
        FAILS.append(what)


class Stub:
    """The finder + matchmaker; records every request line and Host / User-Agent header."""

    def __init__(self, port, game_host, game_port):
        self.lock = threading.Lock()
        self.requests = []
        self.finder_status = 200
        self.game_host, self.game_port = game_host, game_port
        stub = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def do_GET(self):
                with stub.lock:
                    stub.requests.append((self.path, self.headers.get("Host", ""), self.headers.get("User-Agent", "")))
                code, body = stub.answer(self.path)
                data = json.dumps(body).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *a):
                pass

        class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
            daemon_threads = True
            allow_reuse_address = True

        self.srv = Server(("127.0.0.1", port), Handler)
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def answer(self, path):
        route = path.split("?")[0]
        if route == "/servers":
            if self.finder_status != 200:
                return self.finder_status, {"error": "maintenance"}
            servers = [{"host": f"e2e-{i}.example", "port": 21016 + i, "name": f"E2E Finder #{i}", "map": "Burg",
                        "mode": "Free for All", "players": i, "max": 8, "region": "VA", "online_mode": False}
                       for i in (1, 2)]
            return 200, {"ok": True, "servers": servers}
        if route == "/mm/status":
            return 200, {"brain": "e2e", "version": "1.0.0"}
        if route == "/mm/ping-list":
            return 200, {"us-nj": "127.0.0.1"}  # the v10 id of VA (doc/v10/regions.md)
        if route == "/mm/seek-game":
            return 200, {"host": self.game_host, "port": self.game_port, "gameId": "E2E1:abcde", "clientId": "e2e-key",
                         "region": "VA"}
        if route == "/mm/game-list":
            return 200, {"games": [], "totalPlayerCount": 0}
        if route == "/api/player-count":
            return 200, {"playerCount": 0}
        return 404, {"error": "NotFound"}

    def take(self):
        with self.lock:
            out, self.requests = self.requests, []
        return out
def start_game_server(build, port, log_path):
    """A real server_bin (offline_mode, Burg) on the free port: the game server the matchmaker names."""
    log = open(log_path, "w")
    p = subprocess.Popen([os.path.join(build, "server_bin"), "--headless", "--cfg", "none", "--port", str(port), "--offline-mode",
                          "assets/maps/burg.json"], cwd=REPO, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    return p


def run(cmd, env, cwd, timeout=150):
    p = subprocess.run(cmd, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    return p.returncode, p.stdout.decode(errors="replace")


def make_install(tmp, build):
    # the installed layout (package-client.sh): <inst>/BoxOffice and <inst>/runtime/{client_bin, ui/, kn-install.json}
    inst = os.path.join(tmp, "install")
    rt = os.path.join(inst, "runtime")
    os.makedirs(rt)
    shutil.copy2(os.path.join(build, "BoxOffice"), os.path.join(inst, "BoxOffice"))
    shutil.copy2(os.path.join(build, "client_bin"), os.path.join(rt, "client_bin"))
    os.symlink(os.path.join(REPO, "ui"), os.path.join(rt, "ui"))
    with open(os.path.join(rt, "kn-install.json"), "w") as f:
        json.dump({"format": 1, "platform": "linux-x64", "package": "tar.gz", "channel": "latest", "root": ".."}, f)
    # the game content: read-only symlinks into the repository's tree, in a scratch dir (never
    # the checkout itself: a content sync rewrites and deletes files there)
    content = os.path.join(tmp, "content")
    os.makedirs(content)
    for name in os.listdir(os.path.join(REPO, "assets")):
        os.symlink(os.path.realpath(os.path.join(REPO, "assets", name)), os.path.join(content, name))
    return inst, content


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("build")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    build = os.path.abspath(a.build)
    for b in ("client_bin", "BoxOffice"):
        if not os.path.exists(os.path.join(build, b)):
            sys.exit(f"missing {build}/{b} (build the client_bin and kn_launcher targets)")
    if not os.path.exists(os.path.join(REPO, "assets", "css", "fonts", "font2.ttf")):
        print("launcher online e2e: skipped (no game content in assets/)")
        return 0
    tmp = tempfile.mkdtemp(prefix="kn-launcher-online-", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    print(f"launcher online e2e in {tmp}")
    http_port, game_port = kn_proto.free_ports(2)
    game_host = "127.0.0.2"  # the matchmaker's server: anything but the client's 127.0.0.1 default
    stub = Stub(http_port, game_host, game_port)
    game = start_game_server(build, game_port, os.path.join(tmp, "server.log"))
    stub_base = f"http://127.0.0.1:{http_port}"
    finder_url = f"{stub_base}/servers"
    inst, content = make_install(tmp, build)
    cfg = os.path.join(tmp, "xdg-config")
    # the account service and the finder as the client's own settings (Settings > Client), in the
    # config dir a launcher-started client reads: headless runs never use the public defaults
    os.makedirs(os.path.join(cfg, "krunknative"))
    for key, value in (("knBrainUrl", stub_base), ("knFinderUrl", finder_url)):
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_ASSETS_DIR",
                                                           "KRUNKNATIVE_LAUNCHER", "KRUNKNATIVE_BRAIN_URL",
                                                           "KRUNKNATIVE_FINDER_URL", "KRUNKNATIVE_MODE",
                                                           "KRUNKNATIVE_SERVER_PORT")}
    env.update({"XDG_CONFIG_HOME": cfg, "XDG_DATA_HOME": os.path.join(tmp, "xdg-data"), "KRUNKNATIVE_HEADLESS": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_NO_BROWSER": "1"})
    script = os.path.join(tmp, "browser.script")
    try:
        # ------------------------------------------------------------ 1 online through the launcher
        print("1  launcher-started client: finder + matchmaker")
        lines = ["0 menu", "3000 call showWindow 2"]
        lines += [f"{4500 + i * 600} call setExpandedRegion {i}\n{4800 + i * 600} nodes @g{i} .settNameIn" for i in range(4)]
        lines += ["8000 vis @err finderError", "9000 quit"]
        with open(script, "w") as f:
            f.write("\n".join(lines) + "\n")
        env1 = dict(env, KRUNKNATIVE_INPUT_SCRIPT=script)
        code, out = run([os.path.join(inst, "BoxOffice"), "--cli", "--assets-dir", content], env1, tmp)
        reqs = stub.take()
        paths = [p for p, _, _ in reqs]
        for line in out.splitlines():
            if re.match(r"\[(mode|backend|mm|finder|http|net|launcher)\]|connected to|Could not connect|\[input_script\] (nodes|vis)", line):
                print("     " + line)
        check(code == 0, f"launcher + client exit 0 ({code})")
        check("[launcher] starting" in out, "the launcher started the client")
        check(f"server finder {finder_url}" in out, "the client's finder is the configured URL")
        check("/servers" in paths, "the client requested the finder URL")
        check(any(p.startswith("/mm/seek-game") for p in paths), "the client seeked a game through the matchmaker")
        check(all(h == f"127.0.0.1:{http_port}" for _, h, _ in reqs) and all(ua.startswith("KrunkNative") for _, _, ua in reqs),
              "Host header with the port, KrunkNative user agent")
        rows = [int(m) for m in re.findall(r"\[input_script\] nodes t=\d+ @g\d+ elements=\d+ match=(\d+)", out)]
        check(max(rows or [0]) >= 2, f"the server browser lists the finder's servers (rows per group {rows})")
        check(f"[finder] {finder_url}: 2 servers" in out, "the finder answer is logged")
        check(re.search(r"vis t=\d+ @err finderError=[-0]", out) is not None, "no finder error shown")
        check(f"connected to server {game_host}:{game_port}" in out, f"connected to the matchmaker's server {game_host}:{game_port}")
        check("127.0.0.1:21015" not in out, "never the 127.0.0.1:21015 default")

        # ------------------------------------------------------------ 2 the finder fails: visible
        print("2  finder answers 503")
        stub.finder_status = 503
        with open(script, "w") as f:
            f.write("0 menu\n3000 call showWindow 2\n6000 vis @err finderError\n7000 quit\n")
        code, out = run([os.path.join(inst, "BoxOffice"), "--cli", "--assets-dir", content], env1, tmp)
        stub.take()
        check(re.search(r"vis t=\d+ @err finderError=1", out) is not None, "the browser shows the finder error")
        check(f"[finder] {finder_url} failed: HTTP 503" in out, "the log names the URL and the reason")
        stub.finder_status = 200

        # ------------------------------------------------------------ 3 content dir inside a checkout
        print("3  game content inside a git checkout")
        fake_repo = os.path.join(tmp, "repo")
        os.makedirs(os.path.join(fake_repo, ".git"))
        os.makedirs(os.path.join(fake_repo, "assets"))
        code, out = run([os.path.join(inst, "BoxOffice"), "--cli", "--no-launch", "--assets-dir", os.path.join(fake_repo, "assets")], env, tmp)
        check(code != 0 and "inside the git checkout" in out, "refused without --allow-checkout-assets")
        code, out = run([os.path.join(inst, "BoxOffice"), "--cli", "--no-launch", "--allow-checkout-assets", "--assets-dir",
                         os.path.join(fake_repo, "assets")], env, tmp)
        check("inside the git checkout" not in out, "allowed with --allow-checkout-assets")
    finally:
        stub.srv.shutdown()
        if game.poll() is None:
            os.killpg(game.pid, signal.SIGKILL)
            game.wait()
        if FAILS or a.keep:
            print(f"scratch kept: {tmp}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    print(f"launcher online e2e: {'FAILED: ' + '; '.join(FAILS) if FAILS else 'all checks passed'}")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""The comp host's webhook on a real server_bin (doc/v10/ranked-impl.md "Comp webhook"; v10.0.3's host option
"webhook", deob-10.0.3.js:20077-20082, kept for a comp game only, 216122-216129).

  python3 -I server/tests/comp_webhook_e2e.py BUILD_DIR      (run from the repository root)

A real kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite) and one server_bin linked to it, run with
KRUNKNATIVE_COMP_WEBHOOK_INSECURE=1 so that it may post to this test's receiver on 127.0.0.1 over http://. A guest
hosts a 1v1 comp game (/mm/host-game, override 1, one round, team names, the webhook URL); two guests from loopback
(DEV for a guest, so /end works) pick a team and a class, spawn, and the round ends:
  1. the server keeps the webhook of the comp game, and no client gets it in the game settings
  2. one POST reaches the receiver at the URL's path, application/json, after the match's END
  3. its body: type "match_end", the map, mode 2, winner, the two teams with the host's names, both players on
     their teams with getEndGameStats' keys, and no IP or token
  4. the server log names the receiver's host and never the URL's secret path
  5. a custom (not comp) game with a webhook posts nothing at its END
Prints PASS/FAIL lines; exit code 0 only when everything passed. 77 = skipped (no server_bin, no database tool).
"""
import http.server as httpserver
import json
import os
import re
import secrets
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_compwebhook_%d" % os.getpid()
SECRET = "kn-compwebhook-%d" % os.getpid()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-compwebhook-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
PORT_GAME, PORT_HOOK = kn_proto.free_ports(2)
HOOK_PATH = "/api/webhooks/42/s3cr3t-%s" % secrets.token_hex(6)

HELLO_EXT = bytes([1, 4, 2, 1])  # protocol 1.4: NET_PACKET_EXT
CHAT_SEND, END, EXT = 8, 16, 254
EXT_GUEST_ID = 0x8140
COMP_SET = 184
results = []
procs = []
posts = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 200)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.1)
    return None


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {},
                                 method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


class Hook(httpserver.BaseHTTPRequestHandler):
    def do_POST(self):  # noqa: N802 - http.server's name
        n = int(self.headers.get("Content-Length", "0") or 0)
        posts.append({"path": self.path, "type": self.headers.get("Content-Type"), "body": self.rfile.read(n).decode(errors="replace")})
        self.send_response(204)
        self.end_headers()

    def log_message(self, *args):
        pass


class Guest(kn_proto.Conn):
    """A direct loopback connection (the server's own machine: DEV for a guest, so /end works)."""

    def __init__(self, port, guest_id=None):
        super().__init__("127.0.0.1", port)
        self.closed = False
        self.send(kn_proto.HELLO, HELLO_EXT)
        self.join()
        if guest_id:  # NET_EXT_KN_GUEST_ID right away: the host guest is in its game before the host check runs
            self.send(EXT, struct.pack("<H", EXT_GUEST_ID) + kn_proto.cstr(guest_id, 33))
        self.pump(1.5)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end and not self.closed:
            if self.read(timeout=max(0.05, end - time.time())) is None and time.time() < end - 0.05:
                self.closed = True

    def comp(self, key, value):
        self.send(COMP_SET, struct.pack("<Bi", ord(key), value))
        self.pump(0.5)

    def spawn(self):
        self.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 10
        while time.time() < end and not self.closed:
            p = self.read(until=kn_proto.SPAWN, timeout=1)
            info = kn_proto.spawn_info(p) if p is not None else None
            if info and info["is_you"]:
                return True
        return False

    def say(self, text):
        time.sleep(0.9)  # config.chatInterval 800 ms
        self.send(CHAT_SEND, text.encode() + b"\0")
        self.pump(0.6)

    def states(self):
        want = struct.pack("<H", 0x0901)
        return [p[2] for t, p in self.packets if t == EXT and p[:2] == want and len(p) >= 3]

    def text(self):
        return b"".join(p for _, p in self.packets)


def node_config(path, port, brain_port):
    cfg = {"gameserver": {"region": "local", "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0,
                          "ruleset": "v10", "bans_file": os.path.join(WORK, "bans.json"), "node_id": "CWH",
                          "name": "comp webhook e2e", "listen_port": port, "public_host": "127.0.0.1",
                          "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port, "brain_secret": SECRET,
                          "finder_url": "http://127.0.0.1:%d" % brain_port}}
    with open(path, "w") as f:
        json.dump(cfg, f)


def host(brain_port, config):
    """/mm/host-game as a guest (retried while the brain still sees the node busy): (code, reply, guest id)."""
    gid = secrets.token_hex(16)
    end = time.time() + 30
    while True:
        code, reply = http(brain_port, "/mm/host-game?guest=" + gid, {"config": config})
        if code == 200 or time.time() > end:
            return code, reply, gid
        time.sleep(1.0)


def play_round(gid, comp):
    """The host guest and another join the hosted game, take teams 1 / 2 and a class (comp), spawn, and /end the round."""
    a = Guest(PORT_GAME, gid)
    b = Guest(PORT_GAME)
    if comp:
        a.comp("t", 1)
        b.comp("t", 2)
        a.comp("c", 0)
        b.comp("c", 0)
    ok = a.spawn() and b.spawn()
    # the round clock runs from IN_PROGRESS (after a comp game's warmup): GAME_STATE_CHANGED (EXT 0x0901) 3
    end = time.time() + 40
    while time.time() < end and 3 not in a.states():
        a.pump(0.2)
    a.say("/end")
    ended = a.read(until=END, timeout=40) is not None
    b.pump(1.0)
    return a, b, ok, ended


def main():
    if not os.access(f"{BUILD}/server_bin", os.X_OK):
        print("comp_webhook_e2e: no server_bin in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("comp_webhook_e2e: %s unavailable: skipped" % DRIVER)
        return 77
    db = json.loads(r.stdout)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    server_log = os.path.join(WORK, "server.log")
    receiver = httpserver.HTTPServer(("127.0.0.1", PORT_HOOK), Hook)
    threading.Thread(target=receiver.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d%s" % (PORT_HOOK, HOOK_PATH)
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=compwebhook-%d" % os.getpid(),
                                  "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        brain_port = int(m.group(1))
        cfg_path = os.path.join(WORK, "server.json")
        node_config(cfg_path, PORT_GAME, brain_port)
        senv = dict(os.environ, KRUNKNATIVE_COMP_WEBHOOK_INSECURE="1")
        server = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", cfg_path, "--cfg", "none"], cwd=ROOT, env=senv,
                                  stdout=open(server_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                  start_new_session=True)
        procs.append(server)
        linked = wait_for(lambda: "welcomed" in read_log(server_log) or "linked" in read_log(server_log), 20)
        check("the game server links to the brain", linked, read_log(server_log)[-300:])
        if not linked:
            return 1

        # 5 first: a custom game (host type 0) with a webhook posts nothing
        code, reply, gid = host(brain_port, {"override": 0, "webhook": url, "maps": [2], "modes": [0], "gamRounds": 1})
        check("a custom game with a webhook is hosted", code == 200, "%s %s" % (code, str(reply)[:200]))
        a, b, spawned, ended = play_round(gid, comp=False)
        check("its round ends (END) with the host in it", ended and "custom game over" not in read_log(server_log), read_log(server_log)[-300:])
        time.sleep(2.0)
        check("5: a custom (not comp) game posts no webhook", not posts, str(posts)[:200])
        a.close()
        b.close()
        wait_for(lambda: "back to the public rotation" in read_log(server_log), 30)

        code, reply, gid = host(brain_port, {"override": 1, "webhook": url, "maps": [2], "modes": [2], "tmSize": 0, "specSlots": 0,
                                             "gamRounds": 1, "noDraws": False, "nameTeam1": "TeamA", "nameTeam2": "TeamB"})
        check("a 1v1 comp game with a webhook is hosted", code == 200, "%s %s" % (code, str(reply)[:200]))
        a, b, spawned, ended = play_round(gid, comp=True)
        check("both comp players take a team and a class and spawn", spawned)
        check("the comp round ends (END)", ended, "states %s, timer reply %s" % (a.states()[-6:], b"Match timer set" in a.text()))
        check("1: no client gets the webhook in the game settings", HOOK_PATH.encode() not in a.text() and HOOK_PATH.encode() not in b.text())
        got = wait_for(lambda: posts, 15)
        check("2: one POST reaches the receiver at the URL's path, application/json",
              got and len(posts) == 1 and posts[0]["path"] == HOOK_PATH and posts[0]["type"] == "application/json", str(posts)[:300])
        body = json.loads(posts[0]["body"]) if posts else {}
        teams = body.get("teams", [])
        players = body.get("players", [])
        check("3: type match_end, map Sandstorm, mode 2, a winner", body.get("type") == "match_end" and body.get("map") == "Sandstorm"
              and body.get("mode") == 2 and body.get("winner") in (1, 2), json.dumps(body)[:300])
        check("3: the teams with the host's names", [(t.get("team"), t.get("name")) for t in teams] == [(1, "TeamA"), (2, "TeamB")], str(teams))
        keys = {"team", "name", "player_id", "score", "kills", "deaths", "assists", "accuracy", "headshots", "damage_done",
                "objective_score", "rounds_won", "victory", "kr"}
        check("3: both players on their teams with getEndGameStats' keys", sorted(p.get("team") for p in players) == [1, 2]
              and all(set(p) == keys for p in players), str(players)[:300])
        check("3: no IP or token in the body", "127.0.0.1" not in posts[0]["body"] and "token" not in posts[0]["body"] if posts else False)
        time.sleep(0.5)
        log = read_log(server_log)
        check("4: the server log names the host only", "[webhook] match_end posted to 127.0.0.1:%d" % PORT_HOOK in log and HOOK_PATH not in log,
              "\n".join(l for l in log.splitlines() if "webhook" in l)[:300])
        a.pump(3.0)
        check("2: still one POST (once per match)", len(posts) == 1, str(len(posts)))
        a.close()
        b.close()
    finally:
        receiver.shutdown()
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    if results and all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

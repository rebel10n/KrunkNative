#!/usr/bin/env python3
"""Krunker v10.0.3 event kill pools through the game server (doc/v10/kill-pools.md): the brain's running
game event reaches server_bin, the round's END pays each signed-in player one draw per kill, the items
reach the player (NET_EXT_KN_END_ITEM) and the stats commit puts them in the account's inventory.

  python3 -I server/tests/kill_pools_server_e2e.py BUILD_DIR      (run from the repository root)

A real kn_brain on a scratch sqlite database (under ~/.cache) whose community feed has one running event
(killPoolsWeight 100, one pool of skin 5, icon "gift": every draw pays skin 5) and one server_bin linked
to it. Two loopback connections (the server's own machine: DEV, so /god, /bot, /kill and /end work):
  A  joins as an account (JOIN with a session token), kills the bots (/kill @bots)
  G  stays a guest and kills the bots too
  1. the server takes the brain's event ("[events] 1 ongoing event(s), kill pools on")
  2. after the END, A gets one NET_EXT_KN_END_ITEM per kill (min(kills, 30)): skin 5, icon "gift"
  3. the brain's inventory holds that many of skin 5 for A's account, audited as "killpool", and the
     count matches A's committed kills
  4. G, a guest, gets no item (endGame draws for accounts only)
Prints PASS/FAIL lines; exit code 0 only when everything passed. 77 = skipped (no server_bin / kn_brain).
"""
import json
import os
import re
import shutil
import signal
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
SECRET = "kn-killpools-%d" % os.getpid()
PW = "pw-killpools-%d" % os.getpid()
USER = "kpusr%d" % (os.getpid() % 100000)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-killpools-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
PORT_B = kn_proto.free_ports(1)[0]

HELLO_EXT = bytes([1, 4, 2, 1])  # protocol 1.4: NET_PACKET_EXT
CHAT_SEND, END, EXT = 8, 16, 254
EXT_RIGHTS, EXT_END_ITEM = 0x8001, 0x8150
SKIN = 5
results = []
procs = []


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
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


def db_rows(path, sql, args=()):
    con = sqlite3.connect(path)
    try:
        return con.execute(sql, args).fetchall()
    except sqlite3.Error:
        return []
    finally:
        con.close()


def rights_since(conn, mark):
    out = []
    fmt = "<BBBBii32s64s"
    for t, p in conn.packets[mark:]:
        if t == EXT and len(p) >= 2 and struct.unpack("<H", p[:2])[0] == EXT_RIGHTS:
            v = struct.unpack(fmt, p[2:2 + struct.calcsize(fmt)])
            out.append({"status": v[0], "seq": v[1], "account": v[5]})
    return out


def end_items(conn):
    """NET_EXT_KN_END_ITEM bodies: (skin, icon)."""
    out = []
    for t, p in conn.packets:
        if t == EXT and len(p) >= 2 + 32 and struct.unpack("<H", p[:2])[0] == EXT_END_ITEM:
            skin, icon = struct.unpack("<i28s", p[2:34])
            out.append((skin, icon.split(b"\0")[0].decode(errors="replace")))
    return out


class Peer(kn_proto.Conn):
    """A direct loopback connection (the server's own machine: DEV)."""

    def __init__(self, port, session=""):
        super().__init__("127.0.0.1", port)
        self.closed = False
        self.seq = 0
        self.send(kn_proto.HELLO, HELLO_EXT)
        self.join(session=session)
        self.push = self.wait_rights(lambda r: r["status"] == 0, 15)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end and not self.closed:
            if self.read(timeout=max(0.05, end - time.time())) is None and time.time() < end - 0.05:
                self.closed = True

    def wait_rights(self, pred, secs=12, mark=0):
        end = time.time() + secs
        while time.time() < end and not self.closed:
            for r in rights_since(self, mark):
                if pred(r):
                    return r
            self.pump(0.1)
        for r in rights_since(self, mark):
            if pred(r):
                return r
        return None

    def spawn(self):
        self.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 8
        while time.time() < end and not self.closed:
            p = self.read(until=kn_proto.SPAWN, timeout=1)
            info = kn_proto.spawn_info(p) if p is not None else None
            if info and info["is_you"]:
                return True
        return False

    def say(self, text):
        time.sleep(0.9)  # the chat interval
        self.send(CHAT_SEND, text.encode() + b"\0")
        self.pump(0.6)



def write_feed(path):
    now = time.time()
    iso = lambda t: time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t))
    feed = {"news": [], "events": [], "polls": [], "gameEvents": [
        {"id": 1, "name": "e2e event", "startsAt": iso(now - 3600), "endsAt": iso(now + 3600), "killPoolsWeight": 100,
         "killPoolsMode": "add", "killPoolsIcon": "gift",
         "killPools": [{"weight": 100, "contents": [{"content_type": "skin", "content_ref": str(SKIN)}]}]}]}
    with open(path, "w") as f:
        json.dump(feed, f)


def node_config(path, port, brain_port):
    cfg = {"gameserver": {"region": "local", "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0,
                          "ruleset": "v10", "bans_file": os.path.join(WORK, "bans.json"), "node_id": "KPE",
                          "name": "kill pools e2e", "listen_port": port, "public_host": "127.0.0.1",
                          "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port, "brain_secret": SECRET,
                          "finder_url": "http://127.0.0.1:%d" % brain_port}}
    with open(path, "w") as f:
        json.dump(cfg, f)


def main():
    brain_bin = f"{BUILD}/services/brain/kn_brain"
    if not os.access(f"{BUILD}/server_bin", os.X_OK) or not os.access(brain_bin, os.X_OK):
        print("kill_pools_server_e2e: no server_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    db_path = os.path.join(WORK, "coordinator.db")
    feed = os.path.join(WORK, "community.json")
    write_feed(feed)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, KN_DATABASE_DRIVER="sqlite", KN_DATABASE_PATH=db_path)
    brain_log = os.path.join(WORK, "brain.log")
    server_log = os.path.join(WORK, "server.log")
    try:
        subprocess.run([brain_bin, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([brain_bin, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=killpools-%d" % os.getpid(), "--brain.community.feed=" + feed],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        brain_port = int(m.group(1))
        cfg_path = os.path.join(WORK, "server.json")
        node_config(cfg_path, PORT_B, brain_port)
        server = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", cfg_path, "--cfg", "none"], cwd=ROOT,
                                  stdout=open(server_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                  start_new_session=True)
        procs.append(server)
        linked = wait_for(lambda: "welcomed" in read_log(server_log) or "linked" in read_log(server_log), 20)
        check("the game server links to the brain", linked, read_log(server_log)[-300:])
        if not linked:
            return 1
        check("1: the server takes the brain's running event (kill pools on)",
              wait_for(lambda: "[events] 1 ongoing event(s), kill pools on" in read_log(server_log), 10), read_log(server_log)[-300:])

        st, reg = http(brain_port, "/auth/register", {"username": USER, "password": PW})
        token = reg.get("token", "") if isinstance(reg, dict) else ""
        check("an account registers and gets a session token", st == 200 and bool(token), "HTTP %d" % st)

        # A joins as the account (JOIN.session): a live sign-in mid-round keeps the round out of the new
        # account's stats (accounts.c: "nothing of this round goes to whoever comes next")
        a = Peer(PORT_B, session=token)
        g = Peer(PORT_B)
        account = a.push["account"] if a.push else 0
        check("A joins as the account", account > 0, str(a.push))
        check("A and G spawn", a.spawn() and g.spawn())
        a.say("/god")
        g.say("/god")
        a.say("/bot 3")
        a.pump(3)
        g.say("/kill @bots")  # G, a guest, kills
        g.pump(1)
        a.pump(5)  # the bots respawn
        a.say("/kill @bots")  # A's kills
        a.say("/end")
        end_seen = a.read(until=END, timeout=25) is not None
        check("the round ends for A (END)", end_seen)
        if not end_seen:
            return 1
        g.read(until=END, timeout=10)
        a.pump(3)
        g.pump(1)

        items = end_items(a)
        kills = wait_for(lambda: (db_rows(db_path, "SELECT player_kills FROM players WHERE player_id = ?", (account,)) or [[0]])[0][0], 15) or 0
        check("A killed bots in the round (its committed kills)", kills > 0, "kills %s" % kills)
        check("2: A gets one END item per kill, skin %d with the event's icon" % SKIN,
              len(items) == min(kills, 30) and all(i == (SKIN, "gift") for i in items), "%s for %s kill(s)" % (items, kills))
        count = wait_for(lambda: (db_rows(db_path, "SELECT count FROM inventory WHERE account_id = ? AND skin_id = ?", (account, SKIN)) or [[0]])[0][0], 15) or 0
        check("3: A's inventory holds them (the stats commit's itemRewards)", count == len(items) and count > 0, "count %s" % count)
        src = db_rows(db_path, "SELECT DISTINCT source FROM skin_transactions WHERE account_id = ? AND skin_id = ?", (account, SKIN))
        check("3: audited as killpool", src == [("killpool",)], str(src))
        g_killed = any(t == EXT and b"Killed" in p for t, p in g.packets)
        check("4: G, a guest who killed the bots too, gets no item", g_killed and end_items(g) == [], "killed %s, items %s" % (g_killed, end_items(g)))
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    if results and all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

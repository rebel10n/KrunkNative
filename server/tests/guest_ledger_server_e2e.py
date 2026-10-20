#!/usr/bin/env python3
"""A guest's unclaimed rewards on the game server (v10's guest profile: the END credits the guest's
reward and score, the "ger" push tells the menu to ask again, a login zeroes the balance).

  python3 -I server/tests/guest_ledger_server_e2e.py BUILD_DIR      (run from the repository root)

A real kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite) and one server_bin linked to
it (gameserver.brain_url). Two guests connect from loopback: the server's own machine, which is DEV for
a guest, so the chat commands /score and /end work (as in live_signin_e2e.py):
  G  announces its guest id (NET_EXT_KN_GUEST_ID), spawns, gets 300 score (/score 300) and the round
     ends (/end, the 3 s timer)
  N  announces no id and plays the same round
  1. G is credited by the round: the brain's balance for G's id holds KR (the END reward) and 300 score
     (v10 banks a guest's END reward and, in a counted match, its score: acc_player_stats' rules)
  2. G gets NET_EXT_KN_GUEST_EARNED ("ger") after the END, once the brain has taken the credit
  3. the round's credit is one guest_credits row for G's match (a resent frame credits once)
  4. N, with no guest id, earns nothing: no ger and no balance row for it
  5. G signs in to an account on its connection (NET_EXT_KN_ACCOUNT_AUTH, a session token): OK, and
     the login zeroes G's balance (GET /guest/profile: 0 and 0, the row too)
Prints PASS/FAIL lines; exit code 0 only when everything passed. 77 = skipped (no server_bin, no
database tool). The guest ids are never printed.
"""
import json
import os
import re
import secrets
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
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_guestledger_%d" % os.getpid()
SECRET = "kn-guestledger-%d" % os.getpid()
PW = "pw-guestledger-%d" % os.getpid()
STAMP = os.getpid() % 100000
USER = "glusr%d" % STAMP
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-guestledger-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
PORT_B = kn_proto.free_ports(1)[0]

HELLO_EXT = bytes([1, 4, 2, 1])  # protocol 1.4: NET_PACKET_EXT
CHAT_SEND, END, TIMER, EXT = 8, 16, 15, 254
EXT_GUEST_ID, EXT_GUEST_EARNED = 0x8140, 0x8141
EXT_AUTH, EXT_RIGHTS = 0x8000, 0x8001
AUTH_SESSION = 1
ST_OK = 1
LIVE_SESSION = 2
SCORE = 300
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


def register(port):
    st, reg = http(port, "/auth/register", {"username": USER, "password": PW})
    token = reg.get("token", "") if isinstance(reg, dict) else ""
    check("an account registers and gets a session token", st == 200 and bool(token), "HTTP %d" % st)
    return token


def db_rows(path, sql, args=()):
    """The rows of one query; none when the table is missing (a build without the feature fails its checks)."""
    con = sqlite3.connect(path)
    try:
        return con.execute(sql, args).fetchall()
    except sqlite3.Error:
        return []
    finally:
        con.close()


def ext_subtypes(conn, since=0):
    return [struct.unpack("<H", p[:2])[0] for t, p in conn.packets[since:] if t == EXT and len(p) >= 2]


def rights_since(conn, mark):
    out = []
    fmt = "<BBBBii32s64s"
    for t, p in conn.packets[mark:]:
        if t == EXT and len(p) >= 2 and struct.unpack("<H", p[:2])[0] == EXT_RIGHTS:
            v = struct.unpack(fmt, p[2:2 + struct.calcsize(fmt)])
            out.append({"status": v[0], "seq": v[1], "account": v[5]})
    return out


class Guest(kn_proto.Conn):
    """A direct loopback connection (the server's own machine: DEV for a guest, as live_signin_e2e's C)."""

    def __init__(self, port):
        super().__init__("127.0.0.1", port)
        self.closed = False
        self.seq = 0
        self.send(kn_proto.HELLO, HELLO_EXT)
        self.join()
        self.push = self.wait_rights(lambda r: r["status"] == 0, 10)  # RIGHTS STATE: the join is settled

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

    def announce(self, guest_id):
        """NET_EXT_KN_GUEST_ID: the 33-byte body (the id, NUL-terminated)."""
        self.send(EXT, struct.pack("<H", EXT_GUEST_ID) + kn_proto.cstr(guest_id, 33))

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
        time.sleep(0.45)  # the chat burst limit
        self.send(CHAT_SEND, text.encode() + b"\0")
        self.pump(0.6)

    def auth(self, kind, key="", name="", secs=12):
        self.seq = (self.seq + 1) % 256 or 1
        seq, mark = self.seq, len(self.packets)
        body = struct.pack("<BB32s64s", kind, seq, kn_proto.cstr(name, 32), kn_proto.cstr(key, 64))
        self.send(EXT, struct.pack("<H", EXT_AUTH) + body)
        return self.wait_rights(lambda r: r["seq"] == seq and r["status"] != 0, secs, mark)


def wait_ext(conn, subtype, secs):
    """Pumps the connection until an EXT of `subtype` came (True) or the time is up."""
    end = time.time() + secs
    while time.time() < end and not conn.closed:
        if subtype in ext_subtypes(conn):
            return True
        conn.pump(0.1)
    return subtype in ext_subtypes(conn)


def node_config(path, port, brain_port):
    cfg = {"gameserver": {"region": "local", "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0,
                          "ruleset": "v10", "bans_file": os.path.join(WORK, "bans.json"), "node_id": "GLE",
                          "name": "guest ledger e2e", "listen_port": port, "public_host": "127.0.0.1",
                          "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port, "brain_secret": SECRET,
                          "finder_url": "http://127.0.0.1:%d" % brain_port}}
    with open(path, "w") as f:
        json.dump(cfg, f)


def main():
    if not os.access(f"{BUILD}/server_bin", os.X_OK):
        print("guest_ledger_server_e2e: no server_bin in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("guest_ledger_server_e2e: %s unavailable: skipped" % DRIVER)
        return 77
    db = json.loads(r.stdout)
    if DRIVER != "sqlite":
        print("guest_ledger_server_e2e: the balance rows are read from sqlite here: skipped on %s" % DRIVER)
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        return 77
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    server_log = os.path.join(WORK, "server.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=guestledger-%d" % os.getpid(),
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
        node_config(cfg_path, PORT_B, brain_port)
        server = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", cfg_path, "--cfg", "none"], cwd=ROOT,
                                  stdout=open(server_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                  start_new_session=True)
        procs.append(server)
        linked = wait_for(lambda: "welcomed" in read_log(server_log) or "linked" in read_log(server_log), 20)
        check("the game server links to the brain", linked, read_log(server_log)[-300:])
        if not linked:
            return 1
        user_token = register(brain_port)

        gid = secrets.token_hex(16)  # 32 lowercase hex: G's guest id (never printed)
        nid = secrets.token_hex(16)  # N's guest id, never announced
        g = Guest(PORT_B)
        n = Guest(PORT_B)
        check("guest G joins and gets the RIGHTS push", g.push is not None and g.push["account"] == 0, str(g.push))
        g.announce(gid)
        check("guest G spawns", g.spawn())
        check("guest N spawns (no guest id announced)", n.spawn())
        g.say("/score %d" % SCORE)
        g.say("/end")
        end_seen = g.read(until=END, timeout=25) is not None
        check("the round ends for G (END)", end_seen)
        if not end_seen:
            return 1
        n.read(until=END, timeout=10)

        def balance():
            st, body = http(brain_port, "/guest/profile?guest=" + gid)
            prof = (body or {}).get("data", {}).get("profile", {}) if isinstance(body, dict) else {}
            return st, prof.get("funds"), prof.get("score")

        wait_for(lambda: (balance()[1] or 0) > 0, 15)
        st, funds, score = balance()
        check("1: the brain credits G's round: KR from the END's reward", (funds or 0) >= 1, "funds %s" % funds)
        check("1: the brain credits G's round: the score in a counted match", score == SCORE, "score %s" % score)
        check("2: G gets the 'ger' push once the brain took the credit", wait_ext(g, EXT_GUEST_EARNED, 10),
              str(ext_subtypes(g)))
        rows = db_rows(db["path"], "SELECT COUNT(*) FROM guest_credits WHERE guest_id = ?", (gid,))
        check("3: one credit row for G's match (idempotent per match)", rows and rows[0][0] == 1, str(rows))
        n.pump(3)
        nrows = db_rows(db["path"], "SELECT COUNT(*) FROM guest_profiles WHERE guest_id = ?", (nid,))
        check("4: N (no guest id) earns nothing: no 'ger' and no balance row", EXT_GUEST_EARNED not in ext_subtypes(n) and
              bool(nrows) and nrows[0][0] == 0, str(nrows))

        signed = g.auth(AUTH_SESSION, user_token)
        check("5: G signs in to the account on its connection: OK", signed is not None and signed["status"] == ST_OK, str(signed))
        st, funds, score = balance()
        check("5: the login zeroes G's balance (GET /guest/profile)", st == 200 and funds == 0 and score == 0, "%s %s" % (funds, score))
        rows = db_rows(db["path"], "SELECT funds, score FROM guest_profiles WHERE guest_id = ?", (gid,))
        check("5: the balance row is zeroed", rows and rows[0] == (0, 0), str(rows))
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
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    if results and all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

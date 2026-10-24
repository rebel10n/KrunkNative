#!/usr/bin/env python3
"""The live trade of Trade mode end to end (doc/v10/trade.md): a real kn_brain on a scratch SQLite database, one
server_bin in Trade mode linked to it, two raw protocol 1.4 clients signed in to accounts (level 40).

  python3 -I server/tests/live_trade_e2e.py BUILD_DIR      (run from the repository root)

  1. A sends "strd" for B: B gets trd type 0 from A
  2. B accepts ("trd" [A, 1]): the server loads both inventories from the brain, both get trd type 1 with
     [mine, theirs] (A's 2 x3, B's 5 x2)
  3. A adds one unit of 2, B adds one unit of 5, A adds 50 KR: both see every utrd / utrdc
  4. both accept: READY, Loading 3..1, then the brain's live_trade settles it: both get atrd [null, funds, ...]
  5. the database moved it: A 2 x2 + 5 x1 and 1000 - 50 - 10 KR, B 2 x1 + 5 x1 and 1000 + 50 - 10 KR; one live_trades row
  6. a second trade that B cancels: A's window shuts (trd type 2), nothing moves
Prints PASS/FAIL lines; exit 0 only when everything passed, 77 = skipped (no server_bin / kn_brain).
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
NAME = "kn_test_livetrade_%d" % os.getpid()
SECRET = "kn-livetrade-%d" % os.getpid()
PW = "pw-livetrade-%d" % os.getpid()
STAMP = os.getpid() % 100000
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-livetrade-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
PORT = kn_proto.free_ports(1)[0]

HELLO_EXT = bytes([1, 4, 2, 1])  # protocol 1.4: NET_PACKET_EXT
EXT = 254
EXT_AUTH, EXT_RIGHTS = 0x8000, 0x8001
AUTH_SESSION, ST_OK = 1, 1
T_REQUEST, T_INIT, T_UPDATE, T_CURRENCY, T_ACCEPT, T_DECLINE, T_CANCEL, T_CHAT = range(0x0b00, 0x0b08)
T_RESPONSE, T_OPEN, T_ITEM, T_CUR_STATE, T_RESULT, T_CHAT_LINE = range(0x0b10, 0x0b16)
LEVEL40 = int((40 / 0.03) ** 2) + 1
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
                                 headers={"Content-Type": "application/json"} if data else {}, method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, None


def sql(path, query, args=()):
    con = sqlite3.connect(path, timeout=30)
    try:
        rows = con.execute(query, args).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


class Player(kn_proto.Conn):
    def __init__(self, port):
        super().__init__("127.0.0.1", port)
        self.closed = False
        self.uid = -1
        self.send(kn_proto.HELLO, HELLO_EXT)
        self.join()
        self.wait_ext(EXT_RIGHTS, 10)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end and not self.closed:
            if self.read(timeout=max(0.05, end - time.time())) is None and time.time() < end - 0.05:
                self.closed = True

    def exts(self, subtype, since=0):
        return [p[2:] for t, p in self.packets[since:] if t == EXT and len(p) >= 2 and struct.unpack("<H", p[:2])[0] == subtype]

    def wait_ext(self, subtype, secs, since=0, pred=None):
        end = time.time() + secs
        while time.time() < end and not self.closed:
            for b in self.exts(subtype, since):
                if pred is None or pred(b):
                    return b
            self.pump(0.1)
        for b in self.exts(subtype, since):
            if pred is None or pred(b):
                return b
        return None

    def sign_in(self, token):
        mark = len(self.packets)
        self.send(EXT, struct.pack("<H", EXT_AUTH) + struct.pack("<BB32s64s", AUTH_SESSION, 1, kn_proto.cstr("", 32), kn_proto.cstr(token, 64)))
        b = self.wait_ext(EXT_RIGHTS, 15, mark, lambda b: b[0] != 0)
        return b is not None and b[0] == ST_OK

    def spawn(self):
        self.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 8
        while time.time() < end and not self.closed:
            p = self.read(until=kn_proto.SPAWN, timeout=1)
            info = kn_proto.spawn_info(p) if p is not None else None
            if info and info["is_you"]:
                self.uid = info["uid"]
                return True
        return False

    def ext(self, subtype, body=b""):
        self.send(EXT, struct.pack("<H", subtype) + body)


def responses(p, since=0):
    out = []
    for b in p.exts(T_RESPONSE, since):
        typ, uid = struct.unpack("<Bi", b[:5])
        out.append((typ, uid, kn_proto.uncstr(b[5:])))
    return out


def chunked(p, subtype, since=0):
    """The last whole chunked JSON record of the subtype, (uid, value) or None."""
    data, uid, last = b"", -1, None
    for b in p.exts(subtype, since):
        part, parts, u = struct.unpack("<HHi", b[:8])
        if part == 0:
            data = b""
        data += b[8:]
        if part + 1 == parts:
            uid, last = u, data
    return (uid, json.loads(last)) if last is not None else None


def node_config(path, brain_port):
    cfg = {"gameserver": {"region": "local", "modes": ["trade"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0,
                          "ruleset": "v10", "bans_file": os.path.join(WORK, "bans.json"), "node_id": "LTE",
                          "name": "live trade e2e", "listen_port": PORT, "public_host": "127.0.0.1",
                          "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port, "brain_secret": SECRET,
                          "finder_url": "http://127.0.0.1:%d" % brain_port}}
    with open(path, "w") as f:
        json.dump(cfg, f)


def main():
    brain_bin = f"{BUILD}/services/brain/kn_brain"
    if not os.access(f"{BUILD}/server_bin", os.X_OK) or not os.access(brain_bin, os.X_OK):
        print("live_trade_e2e: no server_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", "sqlite", NAME], capture_output=True)
    if r.returncode:
        print("live_trade_e2e: sqlite unavailable: skipped")
        return 77
    db = json.loads(r.stdout)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log, server_log = os.path.join(WORK, "brain.log"), os.path.join(WORK, "server.log")
    try:
        subprocess.run([brain_bin, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([brain_bin, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=livetrade-%d" % os.getpid(), "--finder.poll_interval_s=1",
                                  "--accounts.signup_rewards.enabled=false",
                                  "--accounts.skins_file=" + os.path.join(ROOT, "assets", "skins.json")],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                 start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        brain_port = int(m.group(1))
        cfg_path = os.path.join(WORK, "server.json")
        node_config(cfg_path, brain_port)
        server = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", cfg_path, "--cfg", "none"], cwd=ROOT,
                                  stdout=open(server_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(server)
        check("the game server links to the brain", wait_for(lambda: "welcomed" in read_log(server_log) or "linked" in read_log(server_log), 20))

        ids, tokens = {}, {}
        for k in ("a", "b"):
            st, reg = http(brain_port, "/auth/register", {"username": "lt%s%d" % (k, STAMP), "password": PW})
            tokens[k] = (reg or {}).get("token", "") if isinstance(reg, dict) else ""
            ids[k] = sql(db["path"], "SELECT id FROM accounts WHERE username = ?", ("lt%s%d" % (k, STAMP),))[0][0]
            check("account %s registers" % k, st == 200 and tokens[k], "HTTP %d" % st)
            sql(db["path"], "UPDATE players SET player_score = ?, player_funds = 1000 WHERE player_id = ?", (LEVEL40, ids[k]))
            sql(db["path"], "INSERT INTO kr_transactions (account_id, amount, source) VALUES (?, 1000, 'admin')", (ids[k],))
        sql(db["path"], "INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 2, 3)", (ids["a"],))
        sql(db["path"], "INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 5, 2)", (ids["b"],))

        A, B = Player(PORT), Player(PORT)
        check("A and B sign in to their accounts", A.sign_in(tokens["a"]) and B.sign_in(tokens["b"]))
        check("A and B spawn in Trade mode", A.spawn() and B.spawn(), "uids %d %d" % (A.uid, B.uid))

        # 1. the request
        A.ext(T_REQUEST, struct.pack("<i", B.uid))
        got = B.wait_ext(T_RESPONSE, 8, pred=lambda b: b[0] == 0)
        check("1: B gets trd type 0 from A", got is not None and struct.unpack("<i", got[1:5])[0] == A.uid)
        # 2. the window
        markA, markB = len(A.packets), len(B.packets)
        B.ext(T_INIT, struct.pack("<iB", A.uid, 1))
        wait_for(lambda: (A.pump(0.2), chunked(A, T_OPEN, markA))[1], 10)
        B.pump(0.5)
        oa, ob = chunked(A, T_OPEN, markA), chunked(B, T_OPEN, markB)
        check("2: both windows open with [mine, theirs]",
              oa and ob and oa[0] == B.uid and ob[0] == A.uid and oa[1][0][0]["i"] == 2 and oa[1][0][0]["c"] == 3 and oa[1][1][0]["i"] == 5,
              "%r / %r" % (oa, ob))
        # 3. the offers
        A.ext(T_UPDATE, struct.pack("<iBq", 2, 1, 0))
        A.pump(0.5)
        B.ext(T_UPDATE, struct.pack("<iBq", 5, 1, 0))
        B.pump(0.5)
        A.ext(T_CURRENCY, struct.pack("<BBi", 1, 1, 50))
        A.pump(0.8)
        B.pump(0.5)
        items_b = [struct.unpack("<iBiq", b[:17]) for b in B.exts(T_ITEM, markB)]
        cur_b = [struct.unpack("<iBBi", b[:10]) for b in B.exts(T_CUR_STATE, markB)]
        check("3: B sees A's 2 and its own 5 (utrd)", (A.uid, 1, 2, 0) in items_b and (B.uid, 1, 5, 0) in items_b, str(items_b))
        check("3: B sees A's 50 KR (utrdc)", (A.uid, 1, 1, 50) in cur_b, str(cur_b))
        # 4. both accept, the countdown, the settle
        A.ext(T_ACCEPT)
        A.pump(0.4)
        B.ext(T_ACCEPT)
        res_a = wait_for(lambda: (A.pump(0.2), chunked(A, T_RESULT, markA))[1], 15)
        B.pump(1.0)
        res_b = chunked(B, T_RESULT, markB)
        steps = [u for t, u, _ in responses(A, markA) if t == 4]
        check("4: READY and Loading 3, 2, 1", steps[:3] == [3, 2, 1] and any(t == 3 for t, _, _ in responses(B, markB)), str(steps))
        check("4: A's atrd [null, 940, [\"2\"], [\"5\"], {}]", res_a and res_a[1][0] is None and res_a[1][1] == 940 and res_a[1][2] == ["2"] and
              res_a[1][3] == ["5"], str(res_a))
        check("4: B's atrd [null, 1040, ...]", res_b and res_b[1][0] is None and res_b[1][1] == 1040, str(res_b))
        # 5. the database
        inv = lambda acc, skin: (sql(db["path"], "SELECT count FROM inventory WHERE account_id = ? AND skin_id = ?", (acc, skin)) or [(0,)])[0][0]
        funds = lambda acc: sql(db["path"], "SELECT player_funds FROM players WHERE player_id = ?", (acc,))[0][0]
        check("5: the items moved", (inv(ids["a"], 2), inv(ids["a"], 5), inv(ids["b"], 2), inv(ids["b"], 5)) == (2, 1, 1, 1))
        check("5: KR and the 10 KR tradeCost", (funds(ids["a"]), funds(ids["b"])) == (940, 1040), "%d %d" % (funds(ids["a"]), funds(ids["b"])))
        check("5: one live_trades row", sql(db["path"], "SELECT count(*) FROM live_trades")[0][0] == 1)
        # 6. a canceled trade
        time.sleep(3.2)  # interactTradeTimer
        markA, markB = len(A.packets), len(B.packets)
        A.ext(T_REQUEST, struct.pack("<i", B.uid))
        B.wait_ext(T_RESPONSE, 8, markB, lambda b: b[0] == 0)
        B.ext(T_INIT, struct.pack("<iB", A.uid, 1))
        wait_for(lambda: (A.pump(0.2), chunked(A, T_OPEN, markA))[1], 10)
        A.ext(T_UPDATE, struct.pack("<iBq", 2, 1, 0))
        A.pump(0.4)
        B.ext(T_CANCEL)
        A.pump(1.0)
        shut = [r for r in responses(A, markA) if r[0] == 2]
        check("6: B cancels: A's window shuts (B canceled the trade)", shut and "canceled the trade" in shut[0][2], str(responses(A, markA)))
        check("6: nothing moved", (inv(ids["a"], 2), funds(ids["a"])) == (2, 940))
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
        subprocess.run(scratch + ["drop", "sqlite", NAME], capture_output=True)
    if results and all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Krunker v7.1.4's /xp and /kr chat commands end to end: kn_brain on a scratch database (KN_TEST_DB_DRIVER, default
sqlite here; postgres / mysql work too), one brain-linked server_bin node, protocol 1.4 peers signed in to real
accounts (shared/src/chat_commands.c cmd_grant, server/src/accounts.c accounts_dev_grant, services/brain/db_accounts.c
run_dev_grant).

  python3 -I server/tests/dev_grant_e2e.py BUILD_DIR      (run from the repository root)

Peers are bridged browsers (NET_PACKET_FORWARDED, TEST-NET addresses: never the server's own machine, which is DEV).
  1. alice (a player) says "/kr 500" and "/xp 500": the notify "Host or developer required", no answer line; her
     score, funds and ledger are unchanged
  2. bob (a developer account, DEV rights) says "/xp 5000": "Added 5000 XP (level 2)"; players.player_score; alice
     sees bob's PLAYER_INFO with level 2. "/kr 250": "Added 250 KR (now 250 KR)"; player_funds and one kr_transactions
     row (250, admin, dev_grant); moderation_log has both grants
  3. bob's "/kr 0" and "/xp lots": the usage line, nothing changes
Exit 77 when server_bin / kn_brain or the database is unavailable.
"""
import json
import os
import re
import shutil
import signal
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
STAMP = os.getpid() % 100000
NAME = "kn_test_dev_grant_e2e_%d" % os.getpid()
SECRET = "kn-dev-grant-e2e-%d" % os.getpid()
(PORT,) = kn_proto.free_ports(1)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-dev-grant-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
PW = "pw-dev-grant-%d" % os.getpid()
NODE = "GRANTE2E"
ALICE, BOB = "dga%d" % STAMP, "dgb%d" % STAMP

CHAT, PLAYER_INFO, NOTIFY, FORWARDED, EXT = 9, 34, 82, 85, 254
CHAT_SEND = 8
EXT_AUTH, EXT_RIGHTS = 0x8000, 0x8001
AUTH_SESSION = 1
ST_STATE, ST_OK = 0, 1
RIGHTS_FMT = "<BBBBii32s64s"
INFO_FMT = "<iiiB8s"            # NetPlayerInfoPacket
CHAT_FMT = "<iB32s288s"         # NetChatPacket
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(str(detail)) <= 200)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if show else ''}", flush=True)


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


class Sql:
    """Statements on the scratch database (sqlite3 / psql / mariadb); rows as lists of strings, None when not possible."""

    def __init__(self, db):
        self.db = db

    def __call__(self, text):
        driver = self.db.get("driver")
        if driver == "sqlite":
            import sqlite3
            con = sqlite3.connect(self.db["path"])
            rows = [["" if v is None else str(v) for v in r] for r in con.execute(text).fetchall()]
            con.commit()
            con.close()
            return rows
        if driver == "postgres":
            env = dict(os.environ, PGPASSWORD=str(self.db.get("password", "")))
            r = subprocess.run(["psql", "-h", str(self.db["host"]), "-p", str(self.db["port"]), "-U", str(self.db["user"]),
                                "-d", str(self.db["name"]), "-v", "ON_ERROR_STOP=1", "-qAt", "-c",
                                "SET search_path TO %s; %s" % (self.db["search_path"], text)],
                               env=env, capture_output=True, text=True, timeout=30)
            if r.returncode:
                print("sql failed: " + r.stderr.strip()[:200])
                return None
            return [l.split("|") for l in r.stdout.splitlines() if l and l != "SET" and not l.startswith(("UPDATE", "INSERT"))]
        if driver == "mysql":
            tool = shutil.which("mariadb") or shutil.which("mysql")
            if not tool:
                return None
            r = subprocess.run([tool, "--no-defaults", "--protocol=TCP", "-h", str(self.db["host"]), "-P", str(self.db["port"]),
                                "-u", str(self.db["user"]), "-D", str(self.db["name"]), "-N", "-B", "-e", text],
                               env=dict(os.environ, MYSQL_PWD=str(self.db.get("password", ""))), capture_output=True, text=True,
                               timeout=30)
            if r.returncode:
                return None
            return [["" if v == "NULL" else v for v in l.split("\t")] for l in r.stdout.splitlines() if l]
        return None


def ext_bodies(packets, subtype, mark=0):
    out = []
    for t, p in packets[mark:]:
        if t == EXT and len(p) >= 2 and struct.unpack("<H", p[:2])[0] == subtype:
            out.append(p[2:])
    return out


class Peer(kn_proto.Conn):
    """A bridged browser on protocol 1.4 (NET_PACKET_EXT), signed in by session token."""

    def __init__(self, remote):
        super().__init__("127.0.0.1", PORT)
        self.closed = False
        self.seq = 0
        self.send(FORWARDED, remote.encode() + b"\0")
        self.send(kn_proto.HELLO, bytes([1, 4, 2, 1]))
        self.join()
        self.wait(lambda: self.rights(lambda r: r["status"] == ST_STATE), 8)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end and not self.closed:
            if self.read(timeout=max(0.05, end - time.time())) is None and time.time() < end - 0.05:
                self.closed = True

    def wait(self, fn, secs=8.0):
        end = time.time() + secs
        while time.time() < end and not self.closed:
            v = fn()
            if v:
                return v
            self.pump(0.1)
        return fn()

    def rights(self, pred, mark=0):
        for b in ext_bodies(self.packets, EXT_RIGHTS, mark):
            v = struct.unpack(RIGHTS_FMT, b[:struct.calcsize(RIGHTS_FMT)])
            r = {"status": v[0], "seq": v[1], "rights": v[2], "account": v[5], "name": kn_proto.uncstr(v[6]),
                 "message": kn_proto.uncstr(v[7])}
            if pred(r):
                return r
        return None

    def sign_in(self, token):
        self.seq = (self.seq + 1) % 256 or 1
        seq, mark = self.seq, len(self.packets)
        body = struct.pack("<BB32s64s", AUTH_SESSION, seq, kn_proto.cstr("", 32), kn_proto.cstr(token, 64))
        self.send(EXT, struct.pack("<H", EXT_AUTH) + body)
        return self.wait(lambda: self.rights(lambda r: r["seq"] == seq and r["status"] != ST_STATE, mark), 12)

    def spawn(self):
        self.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 6
        while time.time() < end and not self.closed:
            p = self.read(until=kn_proto.SPAWN, timeout=1)
            info = kn_proto.spawn_info(p) if p is not None else None
            if info and info["is_you"]:
                return info
        return None

    def say(self, text):
        # the server's io_ct burst limit closes a connection over NET_CHAT_BURST_MAX (3) lines in 1 s: keep a third of a
        # second between this peer's lines (reading the server meanwhile)
        gap = getattr(self, "last_say", 0) + 0.35 - time.time()
        if gap > 0:
            self.pump(gap)
        self.last_say = time.time()
        self.send(CHAT_SEND, text.encode() + b"\0")

    def chats(self, mark=0):
        return [kn_proto.uncstr(struct.unpack(CHAT_FMT, p[:struct.calcsize(CHAT_FMT)])[3])
                for t, p in self.packets[mark:] if t == CHAT and len(p) >= struct.calcsize(CHAT_FMT)]

    def notifies(self, mark=0):
        return [kn_proto.uncstr(p[:96]) for t, p in self.packets[mark:] if t == NOTIFY]

    def infos(self, mark=0):
        out = {}
        for t, p in self.packets[mark:]:
            if t == PLAYER_INFO and len(p) >= struct.calcsize(INFO_FMT):
                uid, account, level, flags, _clan = struct.unpack(INFO_FMT, p[:struct.calcsize(INFO_FMT)])
                out[uid] = {"account": account, "level": level, "flags": flags}
        return out


def register(port, name):
    st, reg = http(port, "/auth/register", {"username": name, "password": PW})
    token = reg.get("token", "") if isinstance(reg, dict) else ""
    st2, me = http(port, "/auth/me?token=" + token) if token else (0, None)
    acc = me.get("account", me) if isinstance(me, dict) else {}
    aid = acc.get("id", 0) if isinstance(acc, dict) else 0
    check(f"account {name} registered", st == 200 and token and aid, f"{st} {str(reg)[:120]} {st2}")
    return token, aid


def node(port_brain):
    log = os.path.join(WORK, "node.log")
    path = os.path.join(WORK, "node.json")
    gs = {"node_id": NODE, "name": "dev grant e2e", "listen_port": PORT, "public_host": "127.0.0.1", "region": "local",
          "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0, "ruleset": "v10",
          "bans_file": os.path.join(WORK, "bans.json"), "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % port_brain,
          "brain_secret": SECRET, "finder_url": "http://127.0.0.1:%d" % port_brain}
    with open(path, "w") as f:
        json.dump({"gameserver": gs}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", path, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    return log


def scenario(log, sql, tokens):
    ids = {n: tokens[n][1] for n in tokens}
    sql("UPDATE players SET player_developer = 1 WHERE player_id = %d" % ids[BOB])

    def money(name):
        rows = sql("SELECT player_score, player_funds FROM players WHERE player_id = %d" % ids[name])
        return (int(rows[0][0]), int(rows[0][1])) if rows else None

    def ledger(name):
        return sql("SELECT amount, source, coalesce(ref, '') FROM kr_transactions WHERE account_id = %d ORDER BY id" % ids[name])

    peers = {}
    for i, name in enumerate((ALICE, BOB)):
        p = Peer("203.0.113.%d" % (90 + i))
        r = p.sign_in(tokens[name][0])
        check(f"{name} signs in", r is not None and r["status"] == ST_OK and r["account"] == ids[name], str(r))
        sp = p.spawn()
        check(f"{name} spawns", sp is not None, str(sp))
        p.uid = sp["uid"] if sp else -1
        peers[name] = p
    a, b = peers[ALICE], peers[BOB]
    for p in peers.values():
        p.pump(0.5)
    check("bob has DEV rights (a developer account)", b.rights(lambda r: r["status"] == ST_OK and r["rights"] & 1) is not None)
    alice0, bob0, alice_ledger0, bob_ledger0 = money(ALICE), money(BOB), ledger(ALICE), ledger(BOB)

    # 1. a player is refused by the command's gate
    mark = len(a.packets)
    a.say("/kr 500")
    a.say("/xp 500")
    told = a.wait(lambda: len([t for t in a.notifies(mark) if "Host or developer required" in t]) >= 2, 5)
    a.pump(1.0)
    check("1: alice's /kr and /xp: \"Host or developer required\"", told, str(a.notifies(mark)))
    check("1: and no answer line", not [t for t in a.chats(mark) if t.startswith("Added") or "/kr" in t or "/xp" in t], str(a.chats(mark)))
    check("1: her score, funds and ledger are unchanged", money(ALICE) == alice0 and ledger(ALICE) == alice_ledger0, str(money(ALICE)))

    # 2. a developer grants to itself
    mark, amark = len(b.packets), len(a.packets)
    b.say("/xp 5000")
    told = b.wait(lambda: [t for t in b.chats(mark) if t.startswith("Added")], 8)
    check("2: /xp 5000: \"Added 5000 XP (level 2)\"", told and told[0] == "Added 5000 XP (level 2)", str(told))
    check("2: player_score +5000", money(BOB) == (bob0[0] + 5000, bob0[1]), str(money(BOB)))
    info = a.wait(lambda: a.infos(amark).get(b.uid), 5)
    check("2: alice sees bob's PLAYER_INFO at level 2", info is not None and info["level"] == 2, str(info))
    mark = len(b.packets)
    b.say("/kr 250")
    told = b.wait(lambda: [t for t in b.chats(mark) if t.startswith("Added")], 8)
    check("2: /kr 250: \"Added 250 KR (now %d KR)\"" % (bob0[1] + 250), told and told[0] == "Added 250 KR (now %d KR)" % (bob0[1] + 250),
          str(told))
    check("2: player_funds +250", money(BOB) == (bob0[0] + 5000, bob0[1] + 250), str(money(BOB)))
    rows = ledger(BOB)
    check("2: one kr_transactions row (250, admin, dev_grant)", rows == bob_ledger0 + [["250", "admin", "dev_grant"]], str(rows))
    rows = sql("SELECT actor_id, target_id, action, detail FROM moderation_log WHERE target_id = %d ORDER BY id" % ids[BOB])
    check("2: moderation_log has both grants", rows == [[str(ids[BOB]), str(ids[BOB]), "dev_grant_xp", "5000"],
                                                        [str(ids[BOB]), str(ids[BOB]), "dev_grant_kr", "250"]], str(rows))
    check("2: the server logged them", re.search(r"dev grant: 250 KR to account %d" % ids[BOB], read_log(log)) is not None)

    # 3. usage
    mark = len(b.packets)
    b.say("/kr 0")
    b.wait(lambda: [t for t in b.chats(mark) if t.startswith("Usage: /")], 5)
    b.say("/xp lots")
    told = b.wait(lambda: len([t for t in b.chats(mark) if t.startswith("Usage: /")]) >= 2, 5)
    check("3: /kr 0 and /xp lots: the usage lines", told and [t for t in b.chats(mark) if t.startswith("Usage: /")]
          == ["Usage: /kr <amount> (1 to 10000000)", "Usage: /xp <amount> (1 to 10000000)"], str(b.chats(mark)))
    b.pump(0.5)
    check("3: nothing changed", money(BOB) == (bob0[0] + 5000, bob0[1] + 250), str(money(BOB)))
    for p in peers.values():
        p.close()


def main():
    if not os.access(f"{BUILD}/server_bin", os.X_OK) or not os.access(f"{BUILD}/services/brain/kn_brain", os.X_OK):
        print("dev_grant_e2e: no server_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("dev_grant_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    db = json.loads(r.stdout)
    if str(db.get("port")) == "5434":  # never the production tunnel
        print("dev_grant_e2e: refusing database port 5434")
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        return 1
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=dev-grant-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts on %s" % DRIVER, bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        log = node(port)
        check("the node links to the coordinator", wait_for(lambda: "welcomed" in read_log(log) or "linked" in read_log(log), 20),
              read_log(log)[-300:])
        tokens = {n: register(port, n) for n in (ALICE, BOB)}
        if all(t for t, _ in tokens.values()):
            scenario(log, Sql(db), tokens)
        bad = [l for l in read_log(brain_log).splitlines() if re.search(r"dev_grant\w*: .*ERROR", l)]
        check("no dev_grant statement errors in the brain log", not bad, str(bad[:3]))
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

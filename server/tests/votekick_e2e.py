#!/usr/bin/env python3
"""The player list's vote kick and K.P.D call end to end: kn_brain on a scratch database (default the
local PostgreSQL of config/config.json, 127.0.0.1:5432, never the 5434 tunnel; KN_TEST_DB_DRIVER=sqlite
works too), one brain-linked server_bin node, protocol 1.4 peers signed in to real accounts
(server/src/votekick.c, server/src/accounts.c, services/brain/db_kpd.c, migration 0209_kpd_calls).

  python3 -I server/tests/votekick_e2e.py BUILD_DIR      (run from the repository root)

Accounts (levels from player_score, Krunker v10 level = 0.03 * sqrt(score)): alice 30, bob 6, carol 9
(flagged hacker: player_hack), dave 3, an officer (made with /kpd). Peers are bridged browsers
(NET_PACKET_FORWARDED, TEST-NET addresses: never the server's own machine, which is DEV).
  1. PLAYER_INFO: carol's carries NET_PLAYER_INFO_HACKER (v10 isHacker: the list's red name)
  2. K.P.D: dave (level 3) is refused (Requires Lvl 5); bob reports carol: "Reported carol to the K.P.D",
     and kpd_calls has the row (reporter, suspect, lobby node:id, node, region, map, mode, type, open)
  3. vote kick: alice opens a vote on dave (everyone gets "vk" opened 1/3 by alice), bob and carol vote
     yes: dave gets ERROR "Vote Kicked", everyone "vk" ended (passed); dave signing in again on a
     new connection is refused for voteKickBan (30 min); a new vote right after: the cooldown
  4. /kpd: alice's (a player) changes nothing; bob, made a developer (his own call is never his
     case), runs "/kpd <officer> on": the account is an officer (player_moderator), "/kpd apps" lists none. The officer signs in: NET_EXT_KPD_DATA {sus carol,
     caller bob}; a verdict right away is refused ("kpdf", kpdWatchTime); after 10 s "rkpd" tagged: the
     row is handled (status 1, rating 1, handled_by the officer, handled_at); bob's Helpful Reports 1/1;
     "/kpd <officer> off" revokes it, moderation_log has both
Exit 77 when the database is unavailable.
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
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
STAMP = os.getpid() % 100000
NAME = "kn_test_votekick_%d" % os.getpid()
SECRET = "kn-votekick-%d" % os.getpid()
(PORT,) = kn_proto.free_ports(1)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-votekick-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
PW = "pw-votekick-%d" % os.getpid()
NODE = "VKE2E"
ALICE, BOB, CAROL, DAVE, COP = ("vka%d" % STAMP, "vkb%d" % STAMP, "vkc%d" % STAMP, "vkd%d" % STAMP, "vkcop%d" % STAMP)

CHAT, ERROR, PLAYER_INFO, FORWARDED, EXT = 9, 30, 34, 85, 254
CHAT_SEND = 8
EXT_AUTH, EXT_RIGHTS = 0x8000, 0x8001
EXT_VOTE_KICK, EXT_VK_STATE, EXT_KPD_CALL, EXT_KPD_RESPONSE, EXT_KPD_RESET, EXT_KPD_DATA, EXT_KPD_RATE = (
    0x0800, 0x0801, 0x0802, 0x0803, 0x0804, 0x0805, 0x0806)
AUTH_SESSION = 1
ST_STATE, ST_OK = 0, 1
RIGHTS_FMT = "<BBBBii32s64s"
VK_STATE_FMT = "<BBBii32s32s"   # NetVoteKickStatePacket
KPD_RESPONSE_FMT = "<BB64s32s"  # NetKpdResponsePacket
KPD_REPORTED, KPD_BUSY, KPD_REFUSED = 0, 1, 2
INFO_FMT = "<iiiB8s"            # NetPlayerInfoPacket
INFO_HACKER = 1 << 4
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
    """Statements on the scratch database (psql / sqlite3); rows as lists of strings, None when not possible."""

    def __init__(self, db):
        self.db = db

    def __call__(self, text):
        if self.db.get("driver") == "sqlite":
            import sqlite3
            con = sqlite3.connect(self.db["path"])
            rows = [["" if v is None else str(v) for v in r] for r in con.execute(text).fetchall()]
            con.commit()
            con.close()
            return rows
        if self.db.get("driver") == "postgres":
            env = dict(os.environ, PGPASSWORD=str(self.db.get("password", "")))
            r = subprocess.run(["psql", "-h", str(self.db["host"]), "-p", str(self.db["port"]), "-U", str(self.db["user"]),
                                "-d", str(self.db["name"]), "-v", "ON_ERROR_STOP=1", "-qAt", "-c",
                                "SET search_path TO %s; %s" % (self.db["search_path"], text)],
                               env=env, capture_output=True, text=True, timeout=30)
            if r.returncode:
                print("sql failed: " + r.stderr.strip()[:200])
                return None
            return [l.split("|") for l in r.stdout.splitlines() if l and l != "SET" and not l.startswith(("UPDATE", "INSERT"))]
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

    def ext(self, subtype, body):
        self.send(EXT, struct.pack("<H", subtype) + body)

    def vote(self, alias, yes=1):
        self.ext(EXT_VOTE_KICK, struct.pack("<32sB", kn_proto.cstr(alias, 32), yes))

    def say(self, text):
        self.send(CHAT_SEND, text.encode() + b"\0")

    def kpd(self, uid):
        self.ext(EXT_KPD_CALL, struct.pack("<Bi", 0, uid))

    def rate(self, tagged):
        self.ext(EXT_KPD_RATE, struct.pack("<B", tagged))

    def vk_states(self, mark=0):
        out = []
        for b in ext_bodies(self.packets, EXT_VK_STATE, mark):
            v = struct.unpack(VK_STATE_FMT, b[:struct.calcsize(VK_STATE_FMT)])
            out.append({"active": v[0], "opened": v[1], "passed": v[2], "votes": v[3], "needed": v[4],
                        "target": kn_proto.uncstr(v[5]), "voter": kn_proto.uncstr(v[6])})
        return out

    def kpd_responses(self, mark=0):
        out = []
        for b in ext_bodies(self.packets, EXT_KPD_RESPONSE, mark):
            v = struct.unpack(KPD_RESPONSE_FMT, b[:struct.calcsize(KPD_RESPONSE_FMT)])
            out.append({"code": v[0], "status": v[1], "message": kn_proto.uncstr(v[2]), "name": kn_proto.uncstr(v[3])})
        return out

    def chats(self, mark=0):
        return [kn_proto.uncstr(struct.unpack(CHAT_FMT, p[:struct.calcsize(CHAT_FMT)])[3])
                for t, p in self.packets[mark:] if t == CHAT and len(p) >= struct.calcsize(CHAT_FMT)]

    def infos(self):
        out = {}
        for t, p in self.packets:
            if t == PLAYER_INFO and len(p) >= struct.calcsize(INFO_FMT):
                uid, account, level, flags, _clan = struct.unpack(INFO_FMT, p[:struct.calcsize(INFO_FMT)])
                out[uid] = {"account": account, "level": level, "flags": flags}
        return out

    def error(self):
        for t, p in self.packets:
            if t == ERROR:
                return kn_proto.error_info(p)
        return None


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
    gs = {"node_id": NODE, "name": "vote kick e2e", "listen_port": PORT, "public_host": "127.0.0.1", "region": "local",
          "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0, "ruleset": "v10",
          "bans_file": os.path.join(WORK, "bans.json"), "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % port_brain,
          "brain_secret": SECRET, "finder_url": "http://127.0.0.1:%d" % port_brain}
    with open(path, "w") as f:
        json.dump({"gameserver": gs}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", path, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    return log


def scenario(port, log, sql, tokens):
    ids = {n: tokens[n][1] for n in tokens}
    # levels (v10: 0.03 * sqrt(score)), carol flagged; the officer is made with /kpd (section 4)
    for name, score in ((ALICE, 1000000), (BOB, 40000), (CAROL, 90000), (DAVE, 10000), (COP, 2800000)):
        sql("UPDATE players SET player_score = %d WHERE player_id = %d" % (score, ids[name]))
    sql("UPDATE players SET player_hack = 1 WHERE player_id = %d" % ids[CAROL])

    peers = {}
    for i, name in enumerate((ALICE, BOB, CAROL, DAVE)):
        p = Peer("203.0.113.%d" % (60 + i))
        r = p.sign_in(tokens[name][0])
        check(f"{name} signs in", r is not None and r["status"] == ST_OK and r["account"] == ids[name], str(r))
        sp = p.spawn()
        check(f"{name} spawns", sp is not None, str(sp))
        p.uid = sp["uid"] if sp else -1
        peers[name] = p
    a, b, c, d = (peers[n] for n in (ALICE, BOB, CAROL, DAVE))
    for p in peers.values():
        p.pump(0.5)

    # 1. carol's PLAYER_INFO: the hacker flag
    info = a.wait(lambda: a.infos().get(c.uid), 5)
    check("1: carol's PLAYER_INFO carries NET_PLAYER_INFO_HACKER", info is not None and info["flags"] & INFO_HACKER and info["level"] == 9,
          str(info))
    info = a.infos().get(b.uid)
    check("1: bob's does not (level 6)", info is not None and not info["flags"] & INFO_HACKER and info["level"] == 6, str(info))

    # 2. K.P.D calls
    mark = len(d.packets)
    d.kpd(c.uid)
    r = d.wait(lambda: d.kpd_responses(mark), 5)
    check("2: dave (level 3) cannot call the K.P.D", r and r[0]["status"] == KPD_REFUSED and "Lvl 5" in r[0]["message"], str(r))
    mark = len(b.packets)
    b.kpd(c.uid)
    r = b.wait(lambda: b.kpd_responses(mark), 8)
    check("2: bob reports carol", r and r[0]["status"] == KPD_REPORTED and r[0]["message"] == "Reported %s to the K.P.D" % CAROL
          and r[0]["name"] == CAROL, str(r))
    rows = sql("SELECT reporter_id, reporter_name, reporter_level, suspect_id, suspect_name, lobby, node, region, map, mode, typ,"
               " status, rating FROM kpd_calls")
    row = rows[0] if rows else []
    check("2: kpd_calls has the call: reporter, suspect, lobby, node, region, map, mode, type, open, unrated",
          len(rows or []) == 1 and row[:5] == [str(ids[BOB]), BOB, "6", str(ids[CAROL]), CAROL] and row[5].startswith(NODE + ":")
          and len(row[5]) > len(NODE) + 1 and row[6:8] == [NODE, "local"] and row[8] and row[9:13] == ["0", "0", "0", ""], str(rows))
    mark = len(a.packets)
    a.kpd(c.uid)
    r = a.wait(lambda: a.kpd_responses(mark), 5)
    check("2: alice right after: the line is busy (kpdCallTime)", r and r[0]["status"] == KPD_BUSY, str(r))

    # 3. the vote kick
    marks = {n: len(p.packets) for n, p in peers.items()}
    a.vote(DAVE)
    opened = b.wait(lambda: [s for s in b.vk_states(marks[BOB]) if s["opened"]], 5)
    s = opened[0] if opened else {}
    check("3: everyone gets vk opened: dave 1/3 by alice", s.get("active") and s.get("votes") == 1 and s.get("needed") == 3
          and s.get("target") == DAVE and s.get("voter") == ALICE, str(s))
    check("3: the target sees the vote", d.wait(lambda: d.vk_states(marks[DAVE]), 3))
    b.vote(DAVE)
    two = c.wait(lambda: [s for s in c.vk_states(marks[CAROL]) if s["votes"] == 2], 5)
    check("3: bob votes yes: 2/3", bool(two), str(c.vk_states(marks[CAROL])))
    check("3: bob is told Vote Submitted - 2/3", b.wait(lambda: "Vote Submitted - 2/3" in b.chats(marks[BOB]), 3))
    c.vote(DAVE)
    err = d.wait(lambda: d.error(), 6)
    check("3: carol votes yes: dave gets ERROR Vote Kicked", err is not None and "Vote Kicked" in err[0], str(err))
    ended = a.wait(lambda: [s for s in a.vk_states(marks[ALICE]) if not s["active"]], 5)
    check("3: vk ended (passed) for everyone", ended and ended[0]["passed"] == 1, str(ended))
    check("3: the lobby hears it", a.wait(lambda: any("was vote kicked" in t for t in a.chats(marks[ALICE])), 3))
    check("3: the server logged it", re.search(r"\[votekick\] %s was vote kicked \(3/3\)" % DAVE, read_log(log)) is not None)
    d.close()
    d2 = Peer("203.0.113.70")
    d2.sign_in(tokens[DAVE][0])
    err = d2.wait(lambda: d2.error(), 6)
    check("3: dave signing in again on a new connection is refused (voteKickBan)", err is not None and "Vote kicked" in err[0]
          and "30 min" in err[0], str(err))
    d2.close()
    b.pump(0.5)  # the end of the last vote is read first
    mark = len(b.packets)
    b.vote(CAROL)
    check("3: a new vote right after: the cooldown", b.wait(lambda: any("just ended" in t for t in b.chats(mark)), 4)
          and not b.vk_states(mark), str(b.chats(mark)))

    # 4. the officer: a developer makes the account a K.P.D officer with /kpd (no SQL)
    mark = len(a.packets)
    a.say("/kpd %s on" % COP)
    a.pump(1.0)
    rows = sql("SELECT player_moderator FROM players WHERE player_id = %d" % ids[COP])
    check("4: /kpd from a player changes nothing", rows == [["0"]] and not any("officer" in t for t in a.chats(mark)), f"{rows} {a.chats(mark)}")
    # bob becomes a developer (signs in again): a DEV is an officer too, but the open call is his own
    sql("UPDATE players SET player_developer = 1 WHERE player_id = %d" % ids[BOB])
    mark = len(b.packets)
    r = b.sign_in(tokens[BOB][0])
    check("4: bob signs in again as a developer (DEV)", r is not None and r["status"] == ST_OK and r["rights"] & 1, str(r))
    b.pump(0.5)
    check("4: his own call is never his case", not ext_bodies(b.packets, EXT_KPD_DATA, mark))
    mark = len(b.packets)
    b.say("/kpd %s on" % COP)
    told = b.wait(lambda: [t for t in b.chats(mark) if "K.P.D" in t], 8)
    check("4: /kpd <officer> on: \"is now a K.P.D officer\"", told and "%s is now a K.P.D officer" % COP in told[0], str(told))
    rows = sql("SELECT player_moderator FROM players WHERE player_id = %d" % ids[COP])
    check("4: the account is a moderator", rows == [["1"]], str(rows))
    mark = len(b.packets)
    b.say("/kpd apps")
    told = b.wait(lambda: [t for t in b.chats(mark) if "application" in t], 8)
    check("4: /kpd apps: none pending", told and "No pending K.P.D applications" in told[0], str(told))
    o = Peer("203.0.113.80")
    mark = len(o.packets)
    r = o.sign_in(tokens[COP][0])
    check("4: the officer signs in (a moderator)", r is not None and r["status"] == ST_OK and r["rights"] & 8, str(r))
    data = o.wait(lambda: ext_bodies(o.packets, EXT_KPD_DATA, mark), 5)
    kd = struct.unpack("<ii", data[0][:8]) if data else None
    check("4: the officer gets kpdData {sus carol, caller bob}", kd == (c.uid, b.uid), f"{kd} vs {(c.uid, b.uid)}")
    check("4: nobody else gets a case", not ext_bodies(a.packets, EXT_KPD_DATA) and not ext_bodies(b.packets, EXT_KPD_DATA))
    given = time.time()
    mark = len(o.packets)
    o.rate(1)
    check("4: a verdict right away is refused (kpdf, kpdWatchTime)", o.wait(lambda: ext_bodies(o.packets, EXT_KPD_RESET, mark), 4)
          and any("longer" in t for t in o.chats(mark)), str(o.chats(mark)))
    rows = sql("SELECT status FROM kpd_calls")
    check("4: the call is still open", rows == [["0"]], str(rows))
    for p in (a, b, c, o):  # keep every socket drained while the officer watches
        p.pump(max(0.0, 10.6 - (time.time() - given)) / 4)
    o.pump(max(0.0, 10.6 - (time.time() - given)))
    mark = len(o.packets)
    o.rate(1)
    rated = wait_for(lambda: (sql("SELECT status, rating, handled_by, handled_at IS NOT NULL FROM kpd_calls") or [[""]])[0][0] == "1", 8)
    rows = sql("SELECT status, rating, handled_by, handled_at IS NOT NULL FROM kpd_calls")
    check("4: rkpd tagged: the call is handled, rating 1, by the officer", rated and rows and rows[0][:3] == ["1", "1", str(ids[COP])]
          and rows[0][3] in ("t", "1", "true"), str(rows))
    o.pump(0.5)
    check("4: no refusal this time", not ext_bodies(o.packets, EXT_KPD_RESET, mark) and not o.chats(mark), str(o.chats(mark)))
    check("4: the server logged the verdict", re.search(r"\[kpd\] officer %s rated %s: tagged" % (COP, CAROL), read_log(log))
          and "the verdict on %s: recorded" % CAROL in read_log(log), read_log(log)[-400:])
    mark = len(o.packets)
    o.rate(0)
    check("4: a second verdict: Case already closed", o.wait(lambda: any("already closed" in t for t in o.chats(mark)), 4))
    rows = sql("SELECT jobrating, jobratingpositive FROM players WHERE player_id = %d" % ids[BOB])
    check("4: bob's Helpful Reports: 1/1 (jobrating / jobratingpositive)", rows == [["1", "1"]], str(rows))
    mark = len(b.packets)
    b.say("/kpd %s off" % COP)
    told = b.wait(lambda: [t for t in b.chats(mark) if "K.P.D" in t], 8)
    check("4: /kpd <officer> off: \"is no longer a K.P.D officer\"", told and "%s is no longer a K.P.D officer" % COP in told[0], str(told))
    rows = sql("SELECT player_moderator FROM players WHERE player_id = %d" % ids[COP])
    check("4: the account is no moderator any more", rows == [["0"]], str(rows))
    rows = sql("SELECT action FROM moderation_log WHERE target_id = %d ORDER BY id" % ids[COP])
    check("4: moderation_log: kpd_grant, kpd_revoke", rows == [["kpd_grant"], ["kpd_revoke"]], str(rows))
    for p in (a, b, c, o):
        p.close()


def main():
    if not os.access(f"{BUILD}/server_bin", os.X_OK) or not os.access(f"{BUILD}/services/brain/kn_brain", os.X_OK):
        print("votekick_e2e: no server_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("votekick_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    db = json.loads(r.stdout)
    if str(db.get("port")) == "5434":  # never the production tunnel
        print("votekick_e2e: refusing database port 5434")
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        return 1
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=votekick-%d" % os.getpid(), "--finder.poll_interval_s=1"],
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
        tokens = {n: register(port, n) for n in (ALICE, BOB, CAROL, DAVE, COP)}
        if all(t for t, _ in tokens.values()):
            scenario(port, log, Sql(db), tokens)
        bad = [l for l in read_log(brain_log).splitlines() if re.search(r"kpd_\w+: .*ERROR", l)]
        check("no K.P.D statement errors in the brain log", not bad, str(bad[:3]))
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

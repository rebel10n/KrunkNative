#!/usr/bin/env python3
"""v10.0.7 ADD_PLAYERS' rankedRank and flagIndex on a brain-linked server_bin (NET_EXT_KN_PLAYER_RANK_FLAG, 0x81B0).

  python3 -I server/tests/player_rank_flag_e2e.py BUILD_DIR      (run from the repository root)

getPlayerAddData (deob-10.0.7.js:176753) gives each viewer [46] `account && showBadges.rank ? rankedRank : 0` and
[26] `anonMode && !viewerIsDev ? -1 : account ? account.stats.flg : -1`. kn_brain on a scratch database
(KN_TEST_DB_DRIVER, default sqlite; brain.ranked.season pinned) with two accounts, both placed in the running ranked
season at Gold 2 (cr 7: 1700 MMR) and with a Country Flag (stats.flg), and one brain-linked server_bin node:
  A  an EXT peer (protocol 1.4) signed in to account A, which shows its rank: everyone gets (8, 12) for A
  B  an EXT peer signed in to account B, in anonMode with rankBadge off (ENT V9: anon_mode 1, PLAYER_SHOW_BADGE_RANK clear):
     everyone, B itself too, gets (0, -1) for B
  G  an EXT guest: (0, -1)
  O  a protocol 1.3.1 peer (no EXT): it gets every PLAYER_INFO at its exact 21-byte size and no EXT at all, so an older
     client's stream is what it was
Each rank / flag packet comes right after its player's PLAYER_INFO (which resets both on the client).
Exit 77 when the database is unavailable. Prints PASS/FAIL lines; exit code 0 only when all passed.
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
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
STAMP = os.getpid() % 100000
NAME = "kn_test_rankflag_%d" % os.getpid()
SECRET = "kn-rankflag-%d" % os.getpid()
PORT = kn_proto.free_ports(1)[0]
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-rankflag-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
ACC_A, ACC_B = "rfa%d" % STAMP, "rfb%d" % STAMP
PW = "pw-rankflag-%d" % os.getpid()
SEASON = 7
FLAG_A, FLAG_B = 12, 30
GOLD2 = 8  # shared/include/ranked.h RANK_GOLD2: 1000 + 100 * cr = 1700

PLAYER_INFO, FORWARDED, EXT = 34, 85, 254
EXT_AUTH, EXT_RIGHTS, EXT_RANK_FLAG = 0x8000, 0x8001, 0x81B0
AUTH_SESSION = 1
ST_STATE, ST_OK = 0, 1
RIGHTS_FMT = "<BBBBii32s64s"
INFO_SIZE = 21  # sizeof(NetPlayerInfoPacket): uid, account_id, level, flags, clan[8]
SHOW_RANK, SHOW_ALL = 1 << 2, 0x0F
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


def http(port, method, path, body=None, token=None):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data is not None else {}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


def ent_v9(anon=0, show_badges=SHOW_ALL):
    """NetEntPacket V9 (101 bytes): V7 (class, secondary, cosmetics, spectate, slid, spray, skins, lagComp), attachIndex,
    challMode, anonMode (V8), the v7 cosmetic slots and showBadges."""
    return (struct.pack("<ii7iBBi3if", 0, -1, *([-1] * 7), 0, 1, 0, -1, -1, -1, 1.0) + struct.pack("<iBB", 0, 0, anon) +
            struct.pack("<9iB", *([-1] * 9), show_badges))


class Peer(kn_proto.Conn):
    """A bridged browser (NET_PACKET_FORWARDED with a TEST-NET address: never the server's own machine)."""

    def __init__(self, port, remote, hello=bytes([1, 4, 2, 1])):
        super().__init__("127.0.0.1", port)
        self.seq = 0
        self.send(FORWARDED, remote.encode() + b"\0")
        self.send(kn_proto.HELLO, hello)
        self.join()
        if hello[1] >= 4:
            self.wait_rights(lambda r: r["status"] == ST_STATE, 8)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if self.read(timeout=max(0.05, end - time.time())) is None and time.time() < end - 0.05:
                return

    def rights_since(self, mark):
        out = []
        for t, p in self.packets[mark:]:
            if t == EXT and len(p) >= 2 and struct.unpack("<H", p[:2])[0] == EXT_RIGHTS:
                v = struct.unpack(RIGHTS_FMT, p[2:2 + struct.calcsize(RIGHTS_FMT)])
                out.append({"status": v[0], "seq": v[1], "account": v[5], "name": kn_proto.uncstr(v[6])})
        return out

    def wait_rights(self, pred, secs=12, mark=0):
        end = time.time() + secs
        while time.time() < end:
            for r in self.rights_since(mark):
                if pred(r):
                    return r
            self.pump(0.1)
        return None

    def sign_in(self, token):
        self.seq = (self.seq + 1) % 256 or 1
        seq, mark = self.seq, len(self.packets)
        body = struct.pack("<BB32s64s", AUTH_SESSION, seq, kn_proto.cstr("", 32), kn_proto.cstr(token, 64))
        self.send(EXT, struct.pack("<H", EXT_AUTH) + body)
        return self.wait_rights(lambda r: r["seq"] == seq and r["status"] != ST_STATE, 12, mark)

    def enter(self, payload):
        self._ent, self._ent_retry = payload, None
        self.send(kn_proto.ENT, payload)
        end = time.time() + 12
        while time.time() < end:
            p = self.read(kn_proto.SPAWN, timeout=max(0.1, end - time.time()))
            info = kn_proto.spawn_info(p) if p else None
            if info and info["is_you"] and info["active"]:
                return info
        return None

    def my_uid(self):
        for t, p in self.packets:
            info = kn_proto.spawn_info(p) if t == kn_proto.SPAWN else None
            if info and info["is_you"]:
                return info["uid"]
        return None

    def rank_flags(self):
        """uid -> (rank, flag) of the last rank / flag packet; also the list of (index, uid) of each one."""
        last, at = {}, []
        for i, (t, p) in enumerate(self.packets):
            if t == EXT and len(p) == 2 + 12 and struct.unpack("<H", p[:2])[0] == EXT_RANK_FLAG:
                uid, rank, flag = struct.unpack("<iii", p[2:])
                last[uid] = (rank, flag)
                at.append((i, uid))
        return last, at

    def follows_info(self):
        """Every rank / flag packet comes right after a PLAYER_INFO of the same uid."""
        _, at = self.rank_flags()
        for i, uid in at:
            t, p = self.packets[i - 1] if i > 0 else (None, b"")
            if t != PLAYER_INFO or len(p) < 4 or struct.unpack("<i", p[:4])[0] != uid:
                return False
        return bool(at)


def register(port, name):
    st, reg = http(port, "POST", "/auth/register", {"username": name, "password": PW})
    token = reg.get("token", "") if isinstance(reg, dict) else ""
    check(f"account {name} registered", st == 200 and token, f"{st} {str(reg)[:160]}")
    return token


def run_sql(db, statements):
    if db.get("path"):
        con = sqlite3.connect(db["path"], timeout=10)
        try:
            for s in statements:
                con.execute(s)
            con.commit()
        finally:
            con.close()
    elif db.get("driver") == "postgres":
        env = dict(os.environ, PGPASSWORD=str(db.get("password", "")), PGOPTIONS="-c search_path=" + str(db.get("search_path", "public")))
        for s in statements:
            subprocess.run(["psql", "-h", str(db.get("host", "127.0.0.1")), "-p", str(db.get("port", 5432)), "-U", str(db.get("user", "krunknative")),
                            "-d", str(db.get("name", "krunknative")), "-v", "ON_ERROR_STOP=1", "-qAt", "-c", s], env=env, check=True,
                           capture_output=True)
    else:
        raise RuntimeError("no SQL access for driver %s" % db.get("driver"))


def account_sql(name, flag):
    pid = "(SELECT id FROM accounts WHERE username = '%s')" % name
    return ["UPDATE players SET player_stats = '{\"flg\":%d}' WHERE player_id = %s" % (flag, pid),
            "INSERT INTO ranked_profiles (account_id, season, region, cr, wins) VALUES (%s, %d, 'eu', 7, 6)" % (pid, SEASON)]


def main():
    if not os.access(f"{BUILD}/server_bin", os.X_OK) or not os.access(f"{BUILD}/services/brain/kn_brain", os.X_OK):
        print("player_rank_flag_e2e: no server_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("player_rank_flag_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    db = json.loads(r.stdout)
    if str(db.get("port")) == "5434":  # never the production tunnel
        print("player_rank_flag_e2e: refusing database port 5434")
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        return 1
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    node_log = os.path.join(WORK, "node.log")
    peers = []
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=rankflag-%d" % os.getpid(), "--finder.poll_interval_s=1",
                                  "--brain.ranked.season=%d" % SEASON],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        gs = {"region": "local", "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0,
              "bans_file": os.path.join(WORK, "bans.json"), "node_id": "RANKF", "name": "rank flag e2e",
              "listen_port": PORT, "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % port,
              "brain_secret": SECRET, "finder_url": "http://127.0.0.1:%d" % port}
        cfg = os.path.join(WORK, "node.json")
        with open(cfg, "w") as f:
            json.dump({"gameserver": gs}, f)
        node = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", cfg, "--cfg", "none"], cwd=ROOT,
                                stdout=open(node_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                start_new_session=True)
        procs.append(node)
        check("the node links to the coordinator", wait_for(lambda: "welcomed" in read_log(node_log) or "linked" in read_log(node_log), 20),
              read_log(node_log)[-300:])
        tokens = {n: register(port, n) for n in (ACC_A, ACC_B)}
        if not all(tokens.values()):
            return 1
        run_sql(db, account_sql(ACC_A, FLAG_A) + account_sql(ACC_B, FLAG_B))

        a = Peer(PORT, "192.0.2.51")
        peers.append(a)
        ok = a.sign_in(tokens[ACC_A])
        check("A signs in", ok is not None and ok["status"] == ST_OK, str(ok))
        b = Peer(PORT, "192.0.2.52")
        peers.append(b)
        ok = b.sign_in(tokens[ACC_B])
        check("B signs in", ok is not None and ok["status"] == ST_OK, str(ok))
        g = Peer(PORT, "192.0.2.53")
        peers.append(g)
        o = Peer(PORT, "192.0.2.54", hello=bytes([1, 3, 1]))
        peers.append(o)

        check("A enters", a.enter(ent_v9()) is not None)
        check("B enters (anonMode, rankBadge off)", b.enter(ent_v9(anon=1, show_badges=SHOW_ALL & ~SHOW_RANK)) is not None)
        check("G enters (a guest)", g.enter(ent_v9()) is not None)
        check("O enters (protocol 1.3.1)", o.enter(ent_v9()) is not None)
        for p in peers:
            p.pump(1.5)
        ua, ub, ug, uo = a.my_uid(), b.my_uid(), g.my_uid(), o.my_uid()
        check("every peer has its uid", None not in (ua, ub, ug, uo), str((ua, ub, ug, uo)))

        seen_a, _ = a.rank_flags()
        seen_b, _ = b.rank_flags()
        seen_g, _ = g.rank_flags()
        check("A: its own rank and flag (Gold 2, 12)", seen_a.get(ua) == (GOLD2, FLAG_A), str(seen_a))
        check("B sees A's rank and flag", seen_b.get(ua) == (GOLD2, FLAG_A), str(seen_b))
        check("G (a guest) sees A's rank and flag", seen_g.get(ua) == (GOLD2, FLAG_A), str(seen_g))
        check("A sees B as anonMode + rankBadge off: no rank, no flag", seen_a.get(ub) == (0, -1), str(seen_a))
        check("B sees itself the same (anonMode hides from itself, showBadges.rank too)", seen_b.get(ub) == (0, -1), str(seen_b))
        check("a guest's: (0, -1)", seen_a.get(ug) == (0, -1) and seen_b.get(ug) == (0, -1), str((seen_a, seen_b)))
        check("A: each one right after its PLAYER_INFO", a.follows_info())
        check("B: each one right after its PLAYER_INFO", b.follows_info())

        infos = [p for t, p in o.packets if t == PLAYER_INFO]
        info_uids = {struct.unpack("<i", p[:4])[0] for p in infos if len(p) >= 4}
        check("O (1.3.1): every PLAYER_INFO at its exact size", infos and all(len(p) == INFO_SIZE for p in infos),
              str(sorted({len(p) for p in infos})))
        check("O (1.3.1): A's and B's PLAYER_INFO", {ua, ub} <= info_uids, str(info_uids))
        check("O (1.3.1): no EXT packet at all (it predates them)", not any(t == EXT for t, _ in o.packets),
              str(sorted({t for t, _ in o.packets})))
    finally:
        for p in peers:
            p.close()
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

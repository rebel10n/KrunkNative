#!/usr/bin/env python3
"""Live sign-in: signing in, switching accounts or signing out mid-session re-verifies the live
connection, no rejoin (NET_EXT_KN_ACCOUNT_AUTH / _RIGHTS, shared/include/net_ext.h; server/src/accounts.c
"live sign-in", client/src/net/online_mode.c).

  python3 -I server/tests/live_signin_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite: no server needed; postgres is
config/config.json's local 127.0.0.1:5432, never 5434) with one DEV account (accounts.developers),
and two server_bin nodes on ports from kn_proto.free_ports():
  B  brain-linked (session tokens are verified through the coordinator's token auth)
  S  standalone online_mode (hasJoined straight to the auth service)
Protocol peers are bridged browsers (NET_PACKET_FORWARDED with a TEST-NET address: never the server's
own machine, which is DEV anyway) announcing protocol 1.4:
  A1  a guest gets the RIGHTS push (no rights, live sign-ins by session token) and may not spawn
  A2  it signs in as the DEV account on the same connection: OK, DEV, the Object Manager's caps
      gain DEV + spawn and a crate spawns; nothing reconnected (no ERROR, same connection id)
  A3  a second guest signing in as a non-dev account: OK, the account, no rights, still refused
  A4  a forged token: REFUSED, nothing changes; more attempts: throttled (BUSY), even a good token
  A5  hasJoined on a server without online_mode: not verifiable, nothing changes
  A6  the DEV's noclip, then logout: back to Guest_N, no rights, noclip turned off, spawn refused
  A7  ban: the DEV bans the non-dev account; a guest signing in to it is kicked at once (Banned)
  A8  one connection per account: signing in to the DEV account elsewhere kicks the first one
  S1  online_mode: a guest signs in through /auth/join + hasJoined: the DEV account, DEV rights
  S2  a forged "hj1:" claiming the DEV's name: REFUSED; a session token: not verifiable there
  C   a real client_bin (headless, the v10 page) joins B as a guest through the matchmaker, enters
      the game, opens the menu, signs in to the DEV account in the login modal, then (same
      connection: one join, one connect, the server's one connection id) opens the Object
      Manager (F1) and spawns a crate; logout: the next spawn is refused, still no rejoin
Needs a headless GL device for C (KRUNKNATIVE_HEADLESS=1: EGL) and a non-loopback IPv4 address
of this machine (a loopback client would be the server's own machine: DEV as a guest); without one
C is skipped. Exit 77 when the database is unavailable.
"""
import json
import os
import re
import secrets
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import hashlib

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
STAMP = os.getpid() % 100000
NAME = "kn_test_livesign_%d" % os.getpid()
SECRET = "kn-livesign-%d" % os.getpid()
PORT_B, PORT_S = kn_proto.free_ports(2)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-livesign-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
DEV, USER, OTHER = "lsdev%d" % STAMP, "lsuser%d" % STAMP, "lsother%d" % STAMP
PW = "pw-livesign-%d" % os.getpid()

SPAWN, CHAT_SEND, ERROR, CHEAT, FORWARDED = 4, 8, 30, 60, 85
SPAWN_REQ, SPAWN_RESULT, EXT = 250, 251, 254
EXT_AUTH, EXT_RIGHTS = 0x8000, 0x8001
AUTH_GUEST, AUTH_SESSION, AUTH_HAS_JOINED = 0, 1, 2
ST_STATE, ST_OK, ST_REFUSED, ST_BUSY, ST_UNAVAILABLE = 0, 1, 2, 3, 4
R_DEV, R_HOST, R_OP, R_MOD = 1, 2, 4, 8
LIVE_SESSION, LIVE_HAS_JOINED = 2, 4
OM_QUERY, OM_SPAWN_OBJECT, OM_OK = 0, 7, 0
CAP_SPAWN, CAP_DEV = 2, 4
CRATE = 10  # object_manager_entries(OM_TAB_OBJECTS)[10]
CHEAT_NOCLIP = 0
RIGHTS_FMT = "<BBBBii32s64s"
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


def lan_address():
    """A non-loopback IPv4 address of this machine (no packet is sent), None without one."""
    for target in ("192.0.2.1", "10.255.255.255"):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.connect((target, 9))
                addr = s.getsockname()[0]
                if addr and not addr.startswith("127."):
                    return addr
        except OSError:
            continue
    return None


def query(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", port))
        data, _ = s.recvfrom(2048)
    return data


def node(name, port, gs):
    log = os.path.join(WORK, name + ".log")
    path = os.path.join(WORK, name + ".json")
    with open(path, "w") as f:
        json.dump({"gameserver": gs}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", path, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    return log


class Peer(kn_proto.Conn):
    """A bridged browser on protocol 1.4 (NET_PACKET_EXT)."""

    def __init__(self, port, remote, online_mode=False):
        super().__init__("127.0.0.1", port)
        self.closed = False
        self.seq = 0
        self.challenge = None
        self.send(FORWARDED, remote.encode() + b"\0")
        self.send(kn_proto.HELLO, bytes([1, 4, 2, 1]))
        self.join()
        if online_mode:  # the challenge, then the guest answer (online_mode.c send_guest_join)
            self.challenge = self.read(until=kn_proto.AUTH_CHALLENGE, timeout=5)
            self.join(client_key="hj0")
        self.push = self.wait_rights(lambda r: r["status"] == ST_STATE, 8)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end and not self.closed:
            if self.read(timeout=max(0.05, end - time.time())) is None and time.time() < end - 0.05:
                self.closed = True  # read() returned early: the socket closed

    def rights_since(self, mark):
        out = []
        for t, p in self.packets[mark:]:
            if t == EXT and len(p) >= 2 and struct.unpack("<H", p[:2])[0] == EXT_RIGHTS:
                v = struct.unpack(RIGHTS_FMT, p[2:2 + struct.calcsize(RIGHTS_FMT)])
                out.append({"status": v[0], "seq": v[1], "rights": v[2], "kinds": v[3], "conn": v[4], "account": v[5],
                            "name": kn_proto.uncstr(v[6]), "message": kn_proto.uncstr(v[7])})
        return out

    def wait_rights(self, pred, secs=12, mark=0):
        end = time.time() + secs
        while time.time() < end and not self.closed:
            for r in self.rights_since(mark):
                if pred(r):
                    return r
            self.pump(0.1)
        for r in self.rights_since(mark):
            if pred(r):
                return r
        return None

    def auth(self, kind, key="", name="", secs=12):
        """One NET_EXT_KN_ACCOUNT_AUTH; its answer (the RIGHTS with its seq), None when none came."""
        self.seq = (self.seq + 1) % 256 or 1
        seq, mark = self.seq, len(self.packets)
        body = struct.pack("<BB32s64s", kind, seq, kn_proto.cstr(name, 32), kn_proto.cstr(key, 64))
        self.send(EXT, struct.pack("<H", EXT_AUTH) + body)
        return self.wait_rights(lambda r: r["seq"] == seq and r["status"] != ST_STATE, secs, mark)

    def om(self, action, item=0):
        """One SPAWN_REQ; its SPAWN_RESULT as (status, caps), None when none came."""
        mark = len(self.packets)
        self.send(SPAWN_REQ, struct.pack("<BBhff", action, 0, item, 0.0, 0.0))
        end = time.time() + 4
        while time.time() < end and not self.closed:
            self.pump(0.1)
            for t, p in self.packets[mark:]:
                if t == SPAWN_RESULT and len(p) == 8:
                    a, status, caps, _extra, it, _limit = struct.unpack("<BBBBhH", p)
                    if a == action and it == item:
                        return status, caps
        return None

    def spawn(self):
        """ENT, then this connection's own SPAWN info (None when none came)."""
        self.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 6
        while time.time() < end and not self.closed:
            p = self.read(until=SPAWN, timeout=1)
            info = kn_proto.spawn_info(p) if p is not None else None
            if info and info["is_you"]:
                return info
        return None

    def say(self, text):
        time.sleep(0.45)  # the chat burst limit (3 / s)
        mark = len(self.packets)
        self.send(CHAT_SEND, text.encode() + b"\0")
        self.pump(0.6)
        return mark

    def error(self):
        for t, p in self.packets:
            if t == ERROR:
                return kn_proto.error_info(p)
        return None

    def cheats(self, flag, since=0):
        return [p[1] for t, p in self.packets[since:] if t == CHEAT and len(p) >= 2 and p[0] == flag]


def register(port, name):
    st, reg = http(port, "/auth/register", {"username": name, "password": PW})
    token = reg.get("token", "") if isinstance(reg, dict) else ""
    st2, me = http(port, "/auth/me?token=" + token) if token else (0, None)
    acc = me.get("account", me) if isinstance(me, dict) else {}
    aid = acc.get("id", 0) if isinstance(acc, dict) else 0
    check(f"account {name} registered", st == 200 and token and aid, f"{st} {str(reg)[:120]} {st2} {str(me)[:160]}")
    return token, aid


def spawn_ok(r):
    return r is not None and r[0] == OM_OK


def part_a(brain, log_b, tokens):
    dev_token, dev_id = tokens[DEV]
    user_token, user_id = tokens[USER]
    # A1: a guest
    g1 = Peer(PORT_B, "203.0.113.51")
    p = g1.push
    check("A1: the guest gets the RIGHTS push (live sign-ins by session token)",
          p is not None and p["account"] == 0 and p["rights"] == 0 and p["kinds"] & LIVE_SESSION, str(p))
    conn = p["conn"] if p else -1
    q = g1.om(OM_QUERY)
    check("A1: the guest's Object Manager caps: no spawn, no dev", q is not None and not q[1] & (CAP_SPAWN | CAP_DEV), str(q))
    check("A1: the guest's crate is refused", not spawn_ok(g1.om(OM_SPAWN_OBJECT, CRATE)))

    # A2: signs in to the DEV account on the same connection
    before = len(read_log(log_b))
    r = g1.auth(AUTH_SESSION, dev_token)
    check("A2: DEV sign-in: OK, the DEV account and rights", r is not None and r["status"] == ST_OK and r["account"] == dev_id
          and r["rights"] & R_DEV, str(r))
    check("A2: the same connection (id, no ERROR, open)", r is not None and r["conn"] == conn and g1.error() is None and not g1.closed,
          f"{r and r['conn']} vs {conn} err={g1.error()}")
    q = g1.om(OM_QUERY)
    check("A2: the Object Manager's caps now: dev + spawn", q is not None and q[1] & CAP_DEV and q[1] & CAP_SPAWN, str(q))
    check("A2: the DEV spawns a crate from the menu", spawn_ok(g1.om(OM_SPAWN_OBJECT, CRATE)))
    tail = read_log(log_b)[before:]
    check("A2: the rights change is logged", re.search(r"\[rights\] \S+ \(id %d, 203\.0\.113\.51\): account 0 -> %d \(session token\), "
                                                       r"rights none -> dev" % (conn, dev_id), tail) is not None, tail[-400:])
    check("A2: no second connection from the peer", tail.count("is bridged for 203.0.113.51") == 0)

    # A3: a non-dev account
    g2 = Peer(PORT_B, "203.0.113.52")
    r = g2.auth(AUTH_SESSION, user_token)
    check("A3: non-dev sign-in: OK, the account, no rights", r is not None and r["status"] == ST_OK and r["account"] == user_id
          and r["rights"] == 0, str(r))
    q = g2.om(OM_QUERY)
    check("A3: its caps: no spawn, no dev", q is not None and not q[1] & (CAP_SPAWN | CAP_DEV), str(q))
    check("A3: its crate is refused", not spawn_ok(g2.om(OM_SPAWN_OBJECT, CRATE)))
    sp = g2.spawn()
    check("A3: it spawns under the account's name", sp is not None and sp["name"] == USER, str(sp))

    # A4: a forged token, then the rate limit
    g3 = Peer(PORT_B, "203.0.113.53")
    before = len(read_log(log_b))
    r = g3.auth(AUTH_SESSION, secrets.token_hex(24))
    check("A4: a forged token is refused", r is not None and r["status"] == ST_REFUSED and r["account"] == 0 and r["rights"] == 0, str(r))
    check("A4: and logged", "sign-in refused" in read_log(log_b)[before:])
    check("A4: nothing changed (caps)", (lambda q: q is not None and not q[1] & (CAP_SPAWN | CAP_DEV))(g3.om(OM_QUERY)))
    statuses = [(g3.auth(AUTH_SESSION, secrets.token_hex(24)) or {}).get("status") for _ in range(5)]
    r = g3.auth(AUTH_SESSION, dev_token)
    check("A4: after 6 attempts a minute the next is throttled, a good token too", statuses == [ST_REFUSED] * 5 and r is not None
          and r["status"] == ST_BUSY and r["account"] == 0, f"{statuses} {r}")
    check("A4: still connected, still a guest", not g3.closed and g3.error() is None)

    # A5: hasJoined where the server does not verify it
    r = g2.auth(AUTH_HAS_JOINED, "hj1:" + secrets.token_hex(16), DEV)
    check("A5: hasJoined without online_mode: not verifiable, unchanged", r is not None and r["status"] == ST_UNAVAILABLE
          and r["account"] == user_id and r["rights"] == 0, str(r))

    # A6: the DEV's noclip, then logout
    mark = g1.say("/noclip")
    check("A6: the DEV turns noclip on", 1 in g1.cheats(CHEAT_NOCLIP, mark), str(g1.cheats(CHEAT_NOCLIP, mark)))
    before = len(read_log(log_b))
    mark = len(g1.packets)
    r = g1.auth(AUTH_GUEST)
    check("A6: logout: OK, a guest again (Guest_N), no rights, same connection", r is not None and r["status"] == ST_OK
          and r["account"] == 0 and r["rights"] == 0 and r["name"].startswith("Guest_") and r["conn"] == conn, str(r))
    g1.pump(0.3)
    check("A6: noclip went with the rights", 0 in g1.cheats(CHEAT_NOCLIP, mark), str(g1.cheats(CHEAT_NOCLIP, mark)))
    tail = read_log(log_b)[before:]
    check("A6: logged", re.search(r"\[rights\] .*account %d -> 0 \(signed out\), rights dev.* -> none, cheats off" % dev_id, tail) is not None,
          tail[-300:])
    q = g1.om(OM_QUERY)
    check("A6: caps back to a guest's", q is not None and not q[1] & (CAP_SPAWN | CAP_DEV), str(q))
    check("A6: its crate is refused", not spawn_ok(g1.om(OM_SPAWN_OBJECT, CRATE)))

    # A7: a ban takes effect at once
    r = g1.auth(AUTH_SESSION, dev_token)
    check("A7: the DEV signs in again", r is not None and r["status"] == ST_OK and r["rights"] & R_DEV, str(r))
    g1.say("/ban %s" % USER)
    g2.pump(1.0)
    check("A7: the banned account's player is kicked", g2.closed or g2.error() is not None, str(g2.error()))
    g4 = Peer(PORT_B, "203.0.113.54")
    r = g4.auth(AUTH_SESSION, user_token, secs=4)
    g4.pump(1.0)
    err = g4.error()
    check("A7: signing in to the banned account kicks the connection (Banned)", err is not None and err[0].startswith("Banned")
          and (r is None or r["account"] == 0), f"{err} {r}")

    # A8: one connection per account
    g5 = Peer(PORT_B, "203.0.113.55")
    r = g5.auth(AUTH_SESSION, dev_token)
    g1.pump(1.0)
    check("A8: the DEV account on a second connection: OK there", r is not None and r["status"] == ST_OK and r["rights"] & R_DEV, str(r))
    err = g1.error()
    check("A8: the first connection is kicked (Logged in from another location)", err is not None and "another location" in err[0], str(err))
    for c in (g1, g2, g3, g4, g5):
        c.close()


def server_hash(sn, pk, cn):
    return hashlib.sha256(b"kn-join-v1\0" + sn.encode() + b"\0" + pk.encode() + b"\0" + cn.encode()).hexdigest()


def part_s(brain, log_s, tokens):
    dev_token, dev_id = tokens[DEV]
    g = Peer(PORT_S, "203.0.113.61", online_mode=True)
    p = g.push
    check("S1: the online_mode guest's push: hasJoined, no session tokens", p is not None and p["account"] == 0
          and p["kinds"] & LIVE_HAS_JOINED and not p["kinds"] & LIVE_SESSION, str(p))
    ch = g.challenge or b""
    sn, pk, cn = kn_proto.uncstr(ch[:33]), kn_proto.uncstr(ch[34:99]), secrets.token_hex(16)
    st, j = http(brain, "/auth/join", {"token": dev_token, "serverHash": server_hash(sn, pk, cn)})
    r = g.auth(AUTH_HAS_JOINED, "hj1:" + cn, DEV)
    check("S1: /auth/join + hasJoined on the live connection: the DEV account and rights", st == 200 and r is not None
          and r["status"] == ST_OK and r["rights"] & R_DEV and r["account"] > 0 and r["conn"] == p["conn"], f"{st} {r}")
    q = g.om(OM_QUERY)
    check("S1: its caps: dev + spawn", q is not None and q[1] & CAP_DEV and q[1] & CAP_SPAWN, str(q))
    f = Peer(PORT_S, "203.0.113.62", online_mode=True)
    r = f.auth(AUTH_HAS_JOINED, "hj1:" + secrets.token_hex(16), DEV)
    check("S2: a forged hj1: claiming the DEV's name is refused", r is not None and r["status"] == ST_REFUSED and r["account"] == 0
          and r["rights"] == 0, str(r))
    r = f.auth(AUTH_SESSION, dev_token)
    check("S2: a session token there: not verifiable, unchanged", r is not None and r["status"] == ST_UNAVAILABLE and r["account"] == 0, str(r))
    g.close()
    f.close()


def vis(text, label):
    m = re.search(r"\[input_script\] vis t=\d+ @%s (.*)" % re.escape(label), text)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def client_script():
    """Enter the game as a guest, Escape to the menu, sign in to the DEV account (the username login),
    back to the game, F1 + a crate, then logout and a crate again."""
    s = ["0 play",
         "1500 down ESC", "1520 up ESC",
         "2200 vis @menu signedInHeaderBar signedOutHeaderBar",
         "2300 clickid knV10LoginBtn",
         "2700 clickid knV10LoginMode",
         "3000 clickid accName", "3050 type " + DEV,
         "3200 clickid accPass", "3250 type " + PW,
         "3400 clickid knV10LoginSubmit",
         "8000 vis @signed signedInHeaderBar signedOutHeaderBar",
         "8100 down F1", "8120 up F1",
         "8800 vis @om knOmGrid",
         "9000 om object %d" % CRATE,
         "10500 call logoutAcc",
         "11500 down ESC", "11520 up ESC",
         "12500 om object %d" % CRATE,
         "14000 quit"]
    return "\n".join(s) + "\n"


def part_c(brain, log_b, lan):
    path = os.path.join(WORK, "c.script")
    with open(path, "w") as f:
        f.write(client_script())
    cfg = os.path.join(WORK, "c-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative", "knRuleset.json"), "w") as f:
        f.write("v10")
    log = os.path.join(WORK, "c.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "c-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain, "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % brain})
    before = len(read_log(log_b))
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], cwd=ROOT, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    try:
        p.wait(150)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
        check("C: client finished its script", False, "timed out; log " + log)
    text = read_log(log)
    server = read_log(log_b)[before:]
    m = vis(text, "menu")
    check("C: in the menu as a guest", m.get("signedOutHeaderBar") == "1", str(m))
    s = vis(text, "signed")
    check("C: signed in from the menu", s.get("signedInHeaderBar") == "1", str(s))
    conns = re.findall(r"\[auth\] connection (\d+): account (\d+) -> (\d+), rights (\S+) -> (\S+) \((pushed|sign-in)\)", text)
    ids = {c[0] for c in conns}
    up = [c for c in conns if c[5] == "sign-in" and c[1] == "0" and c[2] != "0" and "dev" in c[4]]
    down = [c for c in conns if c[5] == "sign-in" and c[2] == "0" and c[1] != "0"]
    check("C: the server took the sign-in on the live connection (DEV rights)", bool(up), str(conns))
    check("C: one connection throughout (push, sign-in and logout on the same id)", len(ids) == 1 and bool(down), str(conns))
    check("C: no rejoin, one join, one connect", "[mm] rejoin" not in text and len(re.findall(r"^connected to server", text, re.M)) == 1
          and "the server re-verifies this connection (no rejoin)" in text, str(re.findall(r"^connected to server.*|\[mm\].*rejoin.*", text, re.M))[:300])
    o = vis(text, "om")
    check("C: F1 opens the Object Manager", o.get("knOmGrid") == "1", str(o))
    results_om = re.findall(r"\[object-manager\] action 7 item %d: (.*?) \(caps (\d+)\)" % CRATE, text)
    check("C: the DEV's crate spawns", bool(results_om) and results_om[0][0] == "ok" and int(results_om[0][1]) & CAP_DEV, str(results_om))
    check("C: after logout the crate is refused", len(results_om) >= 2 and results_om[-1][0] != "ok" and not int(results_om[-1][1]) & CAP_DEV,
          str(results_om))
    addr = re.escape(lan)
    server_ids = set(re.findall(r"\(id (\d+), %s\)" % addr, server))
    check("C: the server logged the sign-in and the logout on that one connection",
          server_ids == ids and re.search(r"\[rights\] .*%s\): account 0 -> \d+ \(session token\), rights none -> dev" % addr, server)
          and re.search(r"\[rights\] .*%s\): account \d+ -> 0 \(signed out\)" % addr, server), f"{server_ids} vs {ids}")


def main():
    if not os.access(f"{BUILD}/server_bin", os.X_OK):
        print("live_signin_e2e: no server_bin in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("live_signin_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    db = json.loads(r.stdout)
    if str(db.get("port")) == "5434":  # never the production tunnel
        print("live_signin_e2e: refusing database port 5434")
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        return 1
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    lan = lan_address()
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=livesign-%d" % os.getpid(),
                                  "--finder.poll_interval_s=1", "--accounts.developers=" + DEV],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        common = {"region": "local", "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0,
                  "ruleset": "v10", "bans_file": os.path.join(WORK, "bans.json")}
        log_b = node("b", PORT_B, dict(common, node_id="LIVEB", name="live sign-in e2e", listen_port=PORT_B,
                                       public_host=lan or "127.0.0.1", brain_url="ws://127.0.0.1:%d/mm/internal/ws" % port,
                                       brain_secret=SECRET, finder_url="http://127.0.0.1:%d" % port))
        log_s = node("s", PORT_S, dict(common, node_id="LIVES", name="live sign-in online_mode", listen_port=PORT_S,
                                       online_mode=True, auth_url="http://127.0.0.1:%d" % port, bans_file=os.path.join(WORK, "s-bans.json")))
        linked = wait_for(lambda: "welcomed" in read_log(log_b) or "linked" in read_log(log_b), 20)
        check("B links to the coordinator", linked, read_log(log_b)[-300:])
        check("S is up", bool(wait_for(lambda: query(PORT_S), 15)))
        tokens = {n: register(port, n) for n in (DEV, USER, OTHER)}
        if not all(t for t, _ in tokens.values()):
            return 1
        part_a(port, log_b, tokens)
        part_s(port, log_s, tokens)
        if not os.access(f"{BUILD}/client_bin", os.X_OK) or os.environ.get("KN_LIVE_SIGNIN_NO_CLIENT"):
            print("SKIP  C: no client_bin")
        elif not lan:
            print("SKIP  C: no non-loopback IPv4 address")
        else:
            part_c(port, log_b, lan)
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

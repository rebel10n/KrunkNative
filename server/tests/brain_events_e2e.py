#!/usr/bin/env python3
"""Krunker v10.0.3's warsClan, spinW / unbS, unb, lotWin and chreq produced by KrunkNative's own server and brain, end to
end (server/src/brain_events.c, services/brain/node_events.c, server/src/ks_server_host.c GAME.PAYMENTS; doc/v10/
s2c-coverage.md). A real kn_brain on a scratch SQLite database, two brain-linked server_bin nodes, raw protocol 1.4
clients signed in to accounts.

  python3 -I server/tests/brain_events_e2e.py BUILD_DIR      (run from the repository root)

  node S (a published map hosted on it: a PRIZE_WHEEL at the spawn, a server script that charges, its studio monetized)
    1. spinW: A's "int" [9, 0] -> CONFIRM_INTERACT [0, 2]; A's "cnfm" 2 -> SPIN_WHEEL_RESPONSE to A (mine, funds 950) and
       to the host O (not mine); the brain took 50 KR and gave the skin
    2. unbS: the wheel stops on the server: UNBOX_MESSAGE (player A) to A, and to O only for rarity >= 2
    3. chreq: PAYMENTS.charge(100): CHARGE_CONSENT_REQUEST [id, 100]; accepted -> the script's callback (true, 850), the
       ledger and the studio's share; declined -> (false, 0); authorizeSpend(30, modal) accepted -> (true, "<id>"), then
       transact(id) -> (true, 820); unanswered -> (false, 0) after 15 s
  node T (public Burg FFA, a Turf Wars map; the brain's day is fixed)
    4. warsClan: yesterday's winner [BEW] reaches every joiner after its INIT; a new round with the clan's logo and paint
       sends it again
    5. unb: A's store spin and Free KR spin on the social socket: UNBOX_MESSAGE (named A) to A, to B for rarity >= 2
    6. lotWin: B wins the raffle it entered: LOTTERY_PRIZE_WON (player B) for every prize, to A and B
Every balance equals its ledger. Prints PASS/FAIL lines; exit 0 only when everything passed, 77 = skipped.
"""
import asyncio
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

try:
    import websockets
except ImportError:  # pragma: no cover
    print("brain_events_e2e: python websockets missing: skipped")
    sys.exit(77)

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
NAME = "kn_test_brainev_%d" % os.getpid()
SECRET = "kn-brainev-%d" % os.getpid()
PW = "pw-brainev-%d" % os.getpid()
STAMP = os.getpid() % 100000
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-brainev-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
S_PORT, T_PORT = kn_proto.free_ports(2)
TURF_DAY = 200
LOTTERY = 3
FREE_KR = [5, 10, 20, 500]

HELLO_EXT = bytes([1, 4, 2, 1])  # protocol 1.4: NET_PACKET_EXT
EXT, INTERACT, CONFIRM, CHAT_SEND, INIT = 254, 61, 126, 8, 2
EXT_AUTH, EXT_RIGHTS = 0x8000, 0x8001
X_UNBOX, X_LOTTERY, X_SPIN, X_CHREQ, X_CHRSP, X_WARS = 0x0304, 0x0305, 0x0306, 0x0500, 0x0501, 0x0602
results = []
procs = []

SERVER_SCRIPT = """
action charged(str playerID, bool success, num value) { GAME.log("charged", toStr success, toStr value); }
action spent(str playerID, bool success, num value) { GAME.log("spent", toStr success, toStr value); }
action authed(str playerID, bool success, str value) {
    GAME.log("authed", toStr success, value);
    if (success) { obj[] ops = obj[]; GAME.PAYMENTS.transact(playerID, ops, value, spent); }
}
public action onChatMessage(str msg, str playerID) {
    if (msg == "charge") { GAME.PAYMENTS.charge(playerID, 100, charged); }
    if (msg == "auth") { GAME.PAYMENTS.authorizeSpend(playerID, 30, authed, true); }
}
"""


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


def http(port, path, body=None, token=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method="POST" if data else "GET",
                                 headers={"Content-Type": "application/json"} if data else {})
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


class Sql:
    def __init__(self, path):
        self.path = path

    def q(self, query, args=()):
        con = sqlite3.connect(self.path, timeout=30)
        try:
            rows = con.execute(query, args).fetchall()
            con.commit()
            return rows
        finally:
            con.close()

    def one(self, query, args=()):
        rows = self.q(query, args)
        return rows[0][0] if rows and rows[0][0] is not None else 0


class Player(kn_proto.Conn):
    def __init__(self, port, seek, token):
        super().__init__("127.0.0.1", port)
        self.closed = False
        self.uid = -1
        self.pos = None
        self.send(kn_proto.HELLO, HELLO_EXT)
        self.join(client_key=seek.get("clientId", ""), session=token, game_id=seek.get("gameId", ""))
        self.read(until=INIT, timeout=10)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end and not self.closed:
            if self.read(timeout=max(0.05, end - time.time())) is None and time.time() < end - 0.05:
                self.closed = True

    def exts(self, subtype, since=0):
        return [p[2:] for t, p in self.packets[since:] if t == EXT and len(p) >= 2 and struct.unpack("<H", p[:2])[0] == subtype]

    def wait_ext(self, subtype, secs, since=0, pred=None):
        end = time.time() + secs
        while True:
            for b in self.exts(subtype, since):
                if pred is None or pred(b):
                    return b
            if time.time() >= end or self.closed:
                return None
            self.pump(0.1)

    def wait_packet(self, ptype, secs, since=0):
        end = time.time() + secs
        while True:
            for t, p in self.packets[since:]:
                if t == ptype:
                    return p
            if time.time() >= end or self.closed:
                return None
            self.pump(0.1)

    def spawn(self):
        self.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 8
        while time.time() < end and not self.closed:
            p = self.read(until=kn_proto.SPAWN, timeout=1)
            if p is None or len(p) < 40:
                continue
            v = struct.unpack("<iBBiifi3f", p[:struct.calcsize("<iBBiifi3f")])
            if v[1] and v[2]:  # ours, alive (the joiner's first SPAWN is its dead one)
                self.uid, self.pos = v[0], v[7:10]
                return True
        return False

    def ext(self, subtype, body=b""):
        self.send(EXT, struct.pack("<H", subtype) + body)

    def say(self, text):
        time.sleep(0.9)  # the chat drops a line sooner than 0.8 s after the last one
        self.send(CHAT_SEND, kn_proto.cstr(text, 288))


def unbox_of(b):
    uid, item, kr, kind = struct.unpack("<iiiB", b[:13])
    return {"uid": uid, "item": item, "kr": kr, "kind": kind, "name": kn_proto.uncstr(b[13:45])}


def spin_of(b):
    funds, uid, rand, skin, mine = struct.unpack("<difiB", b[:21])
    return {"funds": funds, "uid": uid, "rand": rand, "skin": skin, "mine": mine}


def wars_of(b):
    clan_id, has = struct.unpack("<iB", b[:5])
    return {"clan_id": clan_id, "has": has, "name": kn_proto.uncstr(b[5:37]), "logo": kn_proto.uncstr(b[37:101]),
            "color": kn_proto.uncstr(b[101:125])}


def script_lines(path):
    prefix = "[KRUNKSCRIPT] "
    return [ln.split(prefix, 1)[1] for ln in read_log(path).splitlines() if prefix in ln]


async def social(port, frames):
    """sends each (label, frame) on one signed-in social socket and returns the replies"""
    out = []
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port, max_size=8 << 20) as ws:
        for label, frame in frames:
            await ws.send(json.dumps(frame))
            end = time.time() + 15
            while True:
                f = json.loads(await asyncio.wait_for(ws.recv(), end - time.time()))
                if f[0] == label:
                    out.append(f)
                    break
    return out


def sign_in_frame(name):
    return ("a", ["a", 1, [name, PW], "captcha"])


def node_config(path, node_id, port, brain_port, extra):
    cfg = {"gameserver": dict({"region": "local", "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0, "ruleset": "v10",
                               "bans_file": os.path.join(WORK, node_id + "-bans.json"), "node_id": node_id, "name": "brain events " + node_id,
                               "listen_port": port, "public_host": "127.0.0.1",
                               "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port, "brain_secret": SECRET,
                               "brain_http_url": "http://127.0.0.1:%d" % brain_port}, **extra)}
    with open(path, "w") as f:
        json.dump(cfg, f)


def lobby_of(brain_port, prefix):
    code, games = http(brain_port, "/mm/game-list")
    return ([g[0] for g in (games or {}).get("games", []) if g[0].startswith(prefix)] + [None])[0]


def start_node(node_id, port, brain_port, extra, args=()):
    cfg_path = os.path.join(WORK, node_id + ".json")
    node_config(cfg_path, node_id, port, brain_port, extra)
    log_path = os.path.join(WORK, node_id + ".log")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", cfg_path, "--cfg", "none"] + list(args), cwd=ROOT,
                         stdout=open(log_path, "w"), stderr=subprocess.STDOUT, stdin=subprocess.PIPE, text=True, start_new_session=True)
    procs.append(p)
    linked = wait_for(lambda: "welcomed" in read_log(log_path), 20)
    lobby = wait_for(lambda: lobby_of(brain_port, node_id + ":"), 15)
    return p, log_path, linked, lobby


def skin_rarities():
    with open(os.path.join(ROOT, "assets", "skins.json")) as f:
        return [int(s.get("rarity", 0) or 0) for s in json.load(f)]


def hosted_part(brain_port, db, tok, ids, names, rar):
    """node S: a published, monetized map with a prize wheel and a charging script"""
    with open(os.path.join(ROOT, "server/tests/arena.json")) as f:
        arena = json.load(f)
    mapname = "BeMap%d" % STAMP
    arena["name"] = mapname
    arena["spawns"] = [[0, 2, 0, 0, 0, 0]]
    arena["objects"].append({"p": [0, 2, 6], "s": [0.01, 10, 10], "i": 68, "sty": 0})  # PRIZE_WHEEL, store wheel 0 (Starter, 50 KR)
    arena["scripts"] = {"server": SERVER_SCRIPT}
    pub = asyncio.run(social(brain_port, [sign_in_frame(names["o"]), ("pubMR", ["uploadM", json.dumps(arena), None, "brain events e2e", "", 2])]))
    check("O publishes the map", pub[-1][1] == "Map Published!", str(pub[-1][:2]))
    mid = db.one("SELECT map_id FROM published_maps WHERE map_name = ?", (mapname,))
    db.q("INSERT INTO studios (name, owner_id, monetization) VALUES (?, ?, 1)", ("BeStudio%d" % STAMP, ids["o"]))
    sid = db.one("SELECT id FROM studios WHERE name = ?", ("BeStudio%d" % STAMP,))
    db.q("INSERT INTO studio_members (studio_id, account_id, role) VALUES (?, ?, 1)", (sid, ids["o"]))
    db.q("INSERT INTO studio_maps (map_id, studio_id, added_by) VALUES (?, ?, ?)", (mid, sid, ids["o"]))

    with open(os.path.join(WORK, "public.json"), "w") as f:
        json.dump(dict(arena, name="BePub%d" % STAMP, scripts={}), f)
    _, s_log, linked, lobby = start_node("BES", S_PORT, brain_port, {"modes": ["ffa"]}, [os.path.join(WORK, "public.json")])
    check("node S links to the brain", bool(linked) and bool(lobby), read_log(s_log)[-300:])
    code, host = http(brain_port, "/mm/host-game?token=" + tok["o"], {"config": {"customMap": mapname}})
    check("O hosts the published map", code == 200 and host.get("clientId"), "%s %s" % (code, str(host)[:200]))
    if code != 200:
        return
    O = Player(S_PORT, host, tok["o"])
    code, seek = http(brain_port, "/mm/seek-game?token=%s&game=%s" % (tok["a"], host["gameId"]))
    A = Player(S_PORT, seek if code == 200 else {}, tok["a"])
    check("A and O spawn in the hosted game", O.spawn() and A.spawn(), "uids %d %d" % (A.uid, O.uid))
    O.pump(0.5)
    check("the game is a monetized studio game", wait_for(lambda: "is a monetized studio game" in read_log(s_log), 10),
          "\n".join(l for l in read_log(s_log).splitlines() if "payments" in l or "script" in l.lower())[-300:])

    # 1. spinW
    mark_a, mark_o = len(A.packets), len(O.packets)
    A.send(INTERACT, struct.pack("<BBii", 1, 0, 9, 0))
    conf = A.wait_packet(CONFIRM, 5, mark_a)
    check("1: interact 9 in reach of the wheel: CONFIRM_INTERACT [0, 2]", conf is not None and struct.unpack("<iB", conf[:5]) == (0, 2),
          "pos %r: %r" % (A.pos, conf))
    A.send(CONFIRM, struct.pack("<iB", 0, 2))
    mine = A.wait_ext(X_SPIN, 10, mark_a)
    other = O.wait_ext(X_SPIN, 5, mark_o)
    mine, other = (spin_of(mine) if mine else {}), (spin_of(other) if other else {})
    check("1: SPIN_WHEEL_RESPONSE to A: wheel 0, mine, funds 950", mine.get("mine") == 1 and mine.get("uid") == 0 and mine.get("funds") == 950
          and 0 <= mine.get("rand", -1) < 100 and mine.get("skin", -1) > 0, str(mine))
    check("1: ... and to O: the same stop and item, not mine, no funds", other.get("mine") == 0 and other.get("funds") == 0
          and other.get("skin") == mine.get("skin") and abs(other.get("rand", 0) - mine.get("rand", 1)) < 1e-3, str(other))
    skin = mine.get("skin", -1)
    check("1: the brain took 50 KR and gave the item", db.one("SELECT player_funds FROM players WHERE player_id = ?", (ids["a"],)) == 950
          and db.one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = ?", (ids["a"], skin)) >= 1)
    # 2. unbS when the server's pointer stops
    u = A.wait_ext(X_UNBOX, 15, mark_a)
    u = unbox_of(u) if u else {}
    check("2: unbS: UNBOX_MESSAGE (player A, the item) to A when the wheel stops", u.get("kind") == 1 and u.get("uid") == A.uid
          and u.get("item") == skin and u.get("kr") == 0, str(u))
    O.pump(0.5)
    to_o = O.exts(X_UNBOX, mark_o)
    rarity = rar[skin] if 0 <= skin < len(rar) else 0
    check("2: ... to O only for rarity >= 2 (rarity %d)" % rarity, bool(to_o) == (rarity >= 2), "%d line(s)" % len(to_o))

    # 3. chreq
    def consent(answer, want_amount, want_modal, mark):
        A.pump(0.2)
        req = A.wait_ext(X_CHREQ, 8, mark)
        if not req:
            return None
        amount, rid, modal = struct.unpack("<diB", req[:13])
        if answer is not None:
            A.ext(X_CHRSP, struct.pack("<iB", rid, answer))
        return amount == want_amount and modal == want_modal and rid > 0

    mark = len(A.packets)
    A.say("charge")
    check("3: charge(100): CHARGE_CONSENT_REQUEST [id, 100] (no modal); accepted", consent(1, 100, 0, mark))
    got = wait_for(lambda: [l for l in script_lines(s_log) if l.startswith("charged ")], 10) or []
    check("3: the callback (true, 850)", got[:1] == ["charged true 850"], str(got))
    a_funds = lambda: db.one("SELECT player_funds FROM players WHERE player_id = ?", (ids["a"],))
    check("3: 100 KR taken with a 'studio-charge' row; the studio books 90", a_funds() == 850
          and db.one("SELECT count(*) FROM kr_transactions WHERE account_id = ? AND amount = -100 AND source = 'studio-charge'", (ids["a"],)) == 1
          and db.q("SELECT amount, earned FROM studio_charges WHERE studio_id = ?", (sid,)) == [(100, 90)])
    mark = len(A.packets)
    A.say("charge")
    check("3: a second charge is declined", consent(0, 100, 0, mark))
    got = wait_for(lambda: (lambda l: l if len(l) >= 2 else None)([l for l in script_lines(s_log) if l.startswith("charged ")]), 10) or []
    check("3: ... its callback (false, 0), nothing taken", got[1:2] == ["charged false 0"] and a_funds() == 850, str(got))
    mark = len(A.packets)
    A.say("auth")
    check("3: authorizeSpend(30, modal): the request with the modal flag; accepted", consent(1, 30, 1, mark))
    authed = wait_for(lambda: [l for l in script_lines(s_log) if l.startswith("authed ")], 10) or []
    spent = wait_for(lambda: [l for l in script_lines(s_log) if l.startswith("spent ")], 10) or []
    check("3: ... the callback (true, \"<id>\") and transact(id): (true, 820)",
          bool(authed) and re.match(r"authed true \d+$", authed[0]) and spent[:1] == ["spent true 820"] and a_funds() == 820, "%r %r" % (authed, spent))
    mark = len(A.packets)
    A.say("charge")
    check("3: a third charge is left unanswered", consent(None, 100, 0, mark))
    got = wait_for(lambda: (lambda l: l if len(l) >= 3 else None)([l for l in script_lines(s_log) if l.startswith("charged ")]), 20) or []
    check("3: ... it declines after 15 s: (false, 0)", got[2:3] == ["charged false 0"] and a_funds() == 820, str(got))
    check("3: no script errors", "RUNTIME ERROR" not in read_log(s_log))
    A.close()
    O.close()
    time.sleep(1.0)


def public_part(brain_port, db, tok, ids, names, rar):
    """node T: public Burg FFA"""
    db.q("INSERT INTO clans (clan_name, clan_playerid) VALUES ('BEW', ?)", (ids["o"],))
    clan = db.one("SELECT clan_id FROM clans WHERE clan_name = 'BEW'")
    # yesterday is settled by the brain's own timer at its start (nobody played: no clan); [BEW] is made its winner
    check("the brain settled yesterday", wait_for(lambda: db.one("SELECT count(*) FROM turf_days WHERE day = ? AND map_id = 0", (TURF_DAY - 1,)), 15))
    db.q("UPDATE turf_days SET clan_id = ?, kills = 7, total_kills = 9 WHERE day = ? AND map_id = 0", (clan, TURF_DAY - 1))
    t, t_log, linked, lobby = start_node("BET", T_PORT, brain_port, {"maps": ["Burg"], "modes": ["ffa"]})
    check("node T (Burg FFA) links to the brain", bool(linked) and bool(lobby), read_log(t_log)[-300:])
    if not lobby:
        return
    seeks = {}
    for k in ("a", "b"):
        code, seeks[k] = http(brain_port, "/mm/seek-game?token=%s&game=%s" % (tok[k], lobby))
    A, B = Player(T_PORT, seeks["a"], tok["a"]), Player(T_PORT, seeks["b"], tok["b"])
    check("A and B spawn on Burg", A.spawn() and B.spawn(), "uids %d %d" % (A.uid, B.uid))

    # 4. warsClan
    wa, wb = A.wait_ext(X_WARS, 8), B.wait_ext(X_WARS, 2)
    wa, wb = (wars_of(wa) if wa else {}), (wars_of(wb) if wb else {})
    check("4: WARS_CLAN after the INIT: [BEW] holds Burg (no logo, no paint)",
          wa.get("has") == 1 and wa.get("clan_id") == clan and wa.get("name") == "BEW" and wa.get("logo") == "" and wa.get("color") == "", str(wa))
    check("4: ... to B too", wb == wa, str(wb))
    db.q("UPDATE clans SET clan_logo = 4, clan_banner_color = '#ff8800' WHERE clan_id = ?", (clan,))
    mark = len(A.packets)
    t.stdin.write("map Burg\n")
    t.stdin.flush()
    w2 = A.wait_ext(X_WARS, 10, mark)
    w2 = wars_of(w2) if w2 else {}
    check("4: a new round asks again: WARS_CLAN [BEW, logo 4, #ff8800]", w2.get("has") == 1 and w2.get("logo") == "4" and w2.get("color") == "#ff8800", str(w2))
    A.spawn()
    B.spawn()

    # 5. unb (the social socket's spins of A, who plays here)
    mark_a, mark_b = len(A.packets), len(B.packets)
    db.q("UPDATE players SET player_funds = player_funds + 1000 WHERE player_id = ?", (ids["a"],))
    db.q("INSERT INTO kr_transactions (account_id, amount, source) VALUES (?, 1000, 'admin')", (ids["a"],))
    rep = asyncio.run(social(brain_port, [sign_in_frame(names["a"]), ("spin", ["spin", 0, None, False]), ("spinKR", ["fkr"])]))
    item, idx = rep[1][3] if rep[1][1] is None else -1, rep[2][3] if rep[2][1] is not None else -1
    got = []
    end = time.time() + 8
    while time.time() < end and len(got) < 2:
        A.pump(0.2)
        got = [unbox_of(b) for b in A.exts(X_UNBOX, mark_a)]
    B.pump(0.5)
    to_b = [unbox_of(b) for b in B.exts(X_UNBOX, mark_b)]
    store = [g for g in got if g["kr"] == 0]
    free = [g for g in got if g["kr"] > 0]
    check("5: unb: A's store win (named, item %d) to A" % item, store[:1] and store[0]["kind"] == 0 and store[0]["uid"] == -1
          and store[0]["item"] == item and store[0]["name"] == names["a"], str(got))
    check("5: unb: A's Free KR win (item = its index, kr) to A", free[:1] and free[0]["item"] == idx and free[0]["kr"] == FREE_KR[idx]
          if 0 <= idx < 4 else False, str(got))
    want_b = [x for x, r in ((item, rar[item] if 0 <= item < len(rar) else 0), (idx, idx)) if r >= 2]
    check("5: ... to B only for rarity >= 2", sorted(g["item"] for g in to_b) == sorted(want_b), "%r vs %r" % (to_b, want_b))

    # 6. lotWin: B enters the raffle and wins every prize of the window
    db.q("UPDATE lotteries SET active = 1 WHERE lottery_id = ?", (LOTTERY,))
    db.q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 7782, 1)", (ids["b"],))
    http(brain_port, "/api/lottery/%d" % LOTTERY)
    code, body = http(brain_port, "/api/lottery/%d/participate" % LOTTERY, {"cost_type": "skin", "cost_ref": "7782", "cost_quantity": 1}, tok["b"])
    check("6: B enters the raffle", code == 200, "%s %s" % (code, body))
    mark_a, mark_b = len(A.packets), len(B.packets)
    draw = db.one("SELECT id FROM lottery_draws WHERE lottery_id = ? AND drawn_s IS NULL", (LOTTERY,))
    db.q("UPDATE lottery_draws SET starts_s = starts_s - 200000, ends_s = ends_s - 200000 WHERE id = ?", (draw,))
    paid = []
    end = time.time() + 30
    while time.time() < end:
        paid = [r[0] for r in db.q("SELECT prize_ref FROM lottery_prizes WHERE draw_id = ? AND winner_id = ?", (draw, ids["b"]))]
        A.pump(0.3)
        if paid and len(A.exts(X_LOTTERY, mark_a)) >= len(paid):
            break
    B.pump(0.5)
    la = [struct.unpack("<diB", b[:13]) + (kn_proto.uncstr(b[13:64]),) for b in A.exts(X_LOTTERY, mark_a)]
    lb = B.exts(X_LOTTERY, mark_b)
    check("6: lotWin: every prize of B's draw (player B, skin, 1x) reaches A", paid and len(la) == len(paid)
          and all(q == 1 and uid == B.uid and typ == 1 for q, uid, typ, _ in la) and sorted(r for *_, r in la) == sorted(paid),
          "%d of %d: %r" % (len(la), len(paid), la[:2]))
    check("6: ... and B", len(lb) == len(paid), "%d" % len(lb))
    A.close()
    B.close()


def main():
    brain_bin = f"{BUILD}/services/brain/kn_brain"
    if not os.access(f"{BUILD}/server_bin", os.X_OK) or not os.access(brain_bin, os.X_OK):
        print("brain_events_e2e: no server_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", "sqlite", NAME], capture_output=True)
    if r.returncode:
        print("brain_events_e2e: sqlite unavailable: skipped")
        return 77
    dbcfg = json.loads(r.stdout)
    db = Sql(dbcfg["path"])
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()})
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([brain_bin, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([brain_bin, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local", "--brain.id=brainev-%d" % os.getpid(),
                                  "--brain.turf.day=%d" % TURF_DAY, "--brain.lottery.settle_check_ms=1000", "--accounts.login_throttle_s=0",
                                  "--accounts.signup_rewards.enabled=false", "--accounts.skins_file=" + os.path.join(ROOT, "assets", "skins.json")],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                 start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        brain_port = int(m.group(1))
        ids, tok, names = {}, {}, {}
        for k in ("a", "b", "o"):
            names[k] = "be%s%d" % (k, STAMP)
            st, reg = http(brain_port, "/auth/register", {"username": names[k], "password": PW})
            tok[k] = (reg or {}).get("token", "") if isinstance(reg, dict) else ""
            ids[k] = db.one("SELECT id FROM accounts WHERE username = ?", (names[k],))
            check("account %s registers" % k, st == 200 and tok[k], "HTTP %d" % st)
            db.q("UPDATE players SET player_funds = 1000 WHERE player_id = ?", (ids[k],))
            db.q("INSERT INTO kr_transactions (account_id, amount, source) VALUES (?, 1000, 'admin')", (ids[k],))
        rar = skin_rarities()
        hosted_part(brain_port, db, tok, ids, names, rar)
        public_part(brain_port, db, tok, ids, names, rar)
        bad = [k for k in ids if db.one("SELECT player_funds FROM players WHERE player_id = ?", (ids[k],))
               != db.one("SELECT coalesce(sum(amount), 0) FROM kr_transactions WHERE account_id = ?", (ids[k],))]
        check("every balance equals its ledger", not bad, str(bad))
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
    bad = [l for l in read_log(brain_log).splitlines() if re.search(r"failed:|ERROR|FATAL", l)]
    check("no statement errors in the brain log", not bad, str(bad[:3]))
    if results and all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

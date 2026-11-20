#!/usr/bin/env python3
"""The hub's creator tools and friends sidebar through the native client (client/src/ui/social_ui.c), headless:
kn_brain on sqlite + one client_bin at a time, logged in from seeded storage and driven through KRUNKNATIVE_UI_CMDS
(the page's inline handlers, typing into its inputs). Every outcome is checked in the page and the database.

  hub_creator_e2e.py <native build dir> <repo root>

  1. the creator's map page (hub-10.0.7.js glb / sd / dd / dkd / mfnds, v10_catalog.c):
     the Leaderboard tab lists the seeded scores; the Data tab finds a player's keys, deletes one, then Delete By
     Key removes a key of every player; Edit -> Add / Take Out moves the map funds; Update Leaderboard republishes
  2. a mod's Edit (updateMod -> plm): the Update Mod popup prefilled with the mod's name
  3. the chat sidebar (chtb): the friend's bubble, a message from them counts on it ("msg"), Friends lists them,
     Remove unfollows
Screenshots go to the work dir (~/.cache/kn-hub-creator.*, kept on failure or with KN_KEEP=1).
77 = skip (no client_bin / kn_brain / websockets).
"""
import asyncio
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
CLIENT = os.path.join(BUILD, "client_bin")
if not (os.path.exists(BRAIN) and os.path.exists(CLIENT)):
    print("needs client_bin and kn_brain in %s: skip" % BUILD)
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-hub-creator.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
PREFAB_LEADERBOARD = 72


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def q(sql, *args):
    con = sqlite3.connect(DB, timeout=30)
    try:
        rows = con.execute(sql, args).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def one(sql, *args):
    r = q(sql, *args)
    return r[0][0] if r and r[0][0] is not None else 0


def wait(fn, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return fn()


class Sock:
    def __init__(self, port):
        self.port, self.ws, self.inbox = port, None, []

    async def open(self):
        self.ws = await websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % self.port, max_size=None)
        return self

    async def call(self, label, *frame, timeout=15.0):
        await self.ws.send(json.dumps(list(frame)))
        end = time.time() + timeout
        while time.time() < end:
            f = json.loads(await asyncio.wait_for(self.ws.recv(), max(0.05, end - time.time())))
            if f and f[0] == "pi":
                await self.ws.send(json.dumps(["po"]))
            elif f and f[0] == label:
                return f
        return None

    async def close(self):
        await self.ws.close()


async def register(port, name):
    s = await Sock(port).open()
    f = await s.call("a", "a", 0, [name, "hub_pass_1", None])
    assert f and f[1] is None, f
    return s, f[2], f[5]


class Client:
    """One headless client_bin on the hub, as `name`."""

    def __init__(self, port, name, token, query, tag):
        self.cmds = os.path.join(WORK, tag + ".cmds")
        self.log = os.path.join(WORK, tag + ".log")
        self.tag = tag
        cfg = os.path.join(WORK, tag + "-cfg")
        os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
        with open(os.path.join(cfg, "krunknative", "krunker_username.json"), "w") as f:
            f.write(name)
        with open(os.path.join(cfg, "krunknative", "krunker_token.json"), "w") as f:
            f.write(token)
        open(self.cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                    "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UI_CMDS": self.cmds, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                    "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port})
        self.out = open(self.log, "w")
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT, "--hub", query], stdout=self.out, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=env, cwd=ROOT, start_new_session=True)

    def text(self):
        try:
            with open(self.log, errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def cmd(self, line):
        with open(self.cmds, "a") as f:
            f.write(line + "\n")

    def ask(self, line, pattern, timeout=3.0):
        """sends a printing command; the first match of pattern in what the client printed after it"""
        mark = len(self.text())
        self.cmd(line)
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pattern, self.text()[mark:])
            if m:
                return m
            time.sleep(0.1)
        return None

    def has(self, elem_id, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.ask("rect " + elem_id, r"#%s (rect|: no box)" % re.escape(elem_id), 1.5)
            if m and m.group(1) == "rect":
                return True
            time.sleep(0.3)
        return False

    def textof(self, sel):
        m = self.ask("textof " + sel, r"textof %s = '(.*)'" % re.escape(sel))
        return m.group(1) if m else None

    def text_has(self, sel, needle, timeout=15.0):
        return wait(lambda: needle in (self.textof(sel) or ""), timeout)

    def value(self, elem_id):
        m = self.ask("value " + elem_id, r"value %s = '(.*)'" % re.escape(elem_id))
        return m.group(1) if m else None

    def count(self, sel):
        m = self.ask("count " + sel, r"count %s = (\d+)" % re.escape(sel))
        return int(m.group(1)) if m else -1

    def ui(self, calls):
        self.cmd("ui " + calls)
        time.sleep(0.4)

    def type(self, elem_id, chars):
        self.cmd("clickid " + elem_id)
        time.sleep(0.4)
        self.cmd("text " + chars)
        time.sleep(0.5)

    def shot(self, name):
        path = os.path.join(WORK, "%s-%s.png" % (self.tag, name))
        self.cmd("shot " + path)
        wait(lambda: os.path.exists(path), 10)
        return path

    def close(self):
        self.cmd("quit")
        try:
            self.proc.wait(15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(5)
        self.out.close()


def map_page(port, ids, tokens, names, map_name, map_id, stamp):
    A, B = ids
    c = Client(port, names[0], tokens[0], "p=map&q=" + map_name, "creator")
    try:
        check(c.has("mapTabsP", 60), "the creator's map page")
        c.ui("updateMapTab(2, true)")
        ok = c.text_has("#mapLeaders", names[1])
        board = c.textof("#mapLeaders") or ""
        check(ok and names[0] in board and board.index(names[0]) < board.index(names[1]),
              "Leaderboard tab: both players, the lower score first (Score / ASC): %r" % board)
        c.shot("leaderboard")

        c.ui("updateMapTab(3, true)")
        check(c.has("searchDInput"), "the Data tab")
        c.type("searchDInput", names[1])
        c.ui("searchPData()")
        check(c.text_has("#dataList", "coins") and "gems" in (c.textof("#dataList") or ""), "Search lists the player's keys")
        c.shot("data")
        c.ui("deleteData('%s', 'coins')" % names[1])
        check(wait(lambda: not q("SELECT 1 FROM ks_storage WHERE map_id = ? AND account_id = ? AND skey = 'coins'", map_id, B)),
              "the row's delete removed that key of that player")
        check(wait(lambda: "coins" not in (c.textof("#dataList") or "coins")), "and the row left the list")
        c.ui("deleteDataPop()")
        check(c.has("delKeyInput"), "Delete By Key asks for the key")
        c.type("delKeyInput", "gems")
        c.ui("deleteDataConf()")
        check(c.has("confBtn"), "and asks again")
        c.cmd("clickid confBtn")
        check(wait(lambda: not q("SELECT 1 FROM ks_storage WHERE map_id = ? AND skey = 'gems'", map_id)),
              "Delete By Key removed the key of every player")

        c.ui("clearPops();openEdit('%s', %d, 0)" % (map_name, map_id))
        check(c.has("mapFundsInp"), "Edit opens the creator's popup")
        c.shot("edit")
        c.type("mapFundsInp", "300")
        c.ui("chngMapFunds(%d, 0)" % map_id)
        check(wait(lambda: one("SELECT map_funds FROM published_maps WHERE map_id = ?", map_id) == 300) and
              one("SELECT player_funds FROM players WHERE player_id = ?", A) == 700, "Add: 300 KR into the map")
        c.ui("clearPops();openEdit('%s', %d, 300)" % (map_name, map_id))
        c.type("mapFundsInp", "100")
        c.ui("chngMapFunds(%d, 1)" % map_id)
        check(wait(lambda: one("SELECT map_funds FROM published_maps WHERE map_id = ?", map_id) == 200) and
              one("SELECT player_funds FROM players WHERE player_id = ?", A) == 800, "Take Out: 100 KR back")
        check(c.text_has("#profileKR", "800"), "the KR in the nav bar follows")

        upd = one("SELECT map_updatecounter FROM published_maps WHERE map_id = ?", map_id)
        c.ui("clearPops();openEdit('%s', %d, 200)" % (map_name, map_id))
        c.type("leaderTypeCust", "Laps")
        c.ui("updateMapLB(%d)" % map_id)
        check(wait(lambda: one("SELECT map_updatecounter FROM published_maps WHERE map_id = ?", map_id) == upd + 1),
              "Update Leaderboard republished the map")
        data = json.loads(one("SELECT map_data FROM published_maps WHERE map_id = ?", map_id) or "{}")
        lbs = [o for o in data.get("objects", []) if o.get("i") == PREFAB_LEADERBOARD]
        check(lbs and lbs[0].get("ldc") == "Laps", "the LEADERBOARD carries the custom stat: %r" % lbs)
        check(c.text_has("#mapLeaders", "Laps") and "Nobody" in (c.textof("#mapLeaders") or ""),
              "the Leaderboard tab follows the new display (a custom stat: Box Office keeps no custom scores)")
        c.shot("leaderboard-custom")

        # 2: a mod's Edit
        c.ui("clearPops();updateMod('Hub Pack %s')" % stamp)
        check(c.has("modNameNew"), "Edit on a mod opens the Update Mod popup")
        check(c.value("modNameNew") == "Hub Pack " + stamp, "prefilled with the mod's name: %r" % c.value("modNameNew"))
        check("Update" in (c.textof(".pubHed") or ""), "titled as an update: %r" % c.textof(".pubHed"))
        c.shot("update-mod")
        c.ui("clearPops();updateMod('Not Mine %s')" % stamp)
        check(c.text_has("#genericPop", "Mod not found"), "another account's mod: Mod not found")
    finally:
        c.close()


async def friends(port, ids, tokens, names, bob_sock):
    A, B = ids
    c = Client(port, names[0], tokens[0], "p=feed", "friends")
    try:
        check(c.has("chatBubHold", 60) and wait(lambda: c.count("#msgCnt%d" % B) == 1, 20), "the chat sidebar shows the friend's bubble")
        check(c.count("#chatBubHold .chatBlobI") == 1, "one bubble: the one friend")
        r = await bob_sock.call("chf", "ict", A, "hi from bob")
        check(r and r[1] is True, "bob sent a message")
        check(wait(lambda: c.textof("msgCnt%d" % B) == "1", 10), "the message counts on bob's bubble (msg push)")
        c.ui("showFriends()")
        check(c.text_has("#genericPop", names[1]) and "Online" in (c.textof("#genericPop") or ""), "Friends lists bob, online")
        c.shot("friends")
        c.ui("removeFriend(0)")
        check(wait(lambda: not q("SELECT 1 FROM follows WHERE follower_id = ? AND followee_id = ?", A, B)), "Remove unfollowed bob")
        check(wait(lambda: c.count("#chatBubHold .chatBlobImg[onclick]") == 0, 10), "and his bubble left the sidebar")
    finally:
        c.close()


def main():
    (port,) = free_ports(1)
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")  # its own config: never the developer's config/config.json
    with open(cfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "hubc-e2e-%d" % os.getpid()},
                   "accounts": {"signup_rewards": {"enabled": False}, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0,
                                "password_iterations": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        for _ in range(200):
            try:
                with open(os.path.join(WORK, "brain.log")) as f:
                    if re.search(r"listening on http://127\.0\.0\.1:%d" % port, f.read()):
                        break
            except OSError:
                pass
            time.sleep(0.1)
        stamp = str(int(time.time()) % 100000)
        names = ["hcral%s" % stamp, "hcrbo%s" % stamp]

        async def setup():
            socks, ids, tokens = [], [], []
            for n in names:
                s, i, t = await register(port, n)
                socks.append(s)
                ids.append(i)
                tokens.append(t)
            q("UPDATE players SET player_score = 2000000, player_funds = 1000 WHERE player_id = ?", ids[0])
            map_name = "Hubc_" + stamp
            doc = {"name": map_name, "modes": [0, 1], "spawns": [[0, 1, 0], [4, 1, 0]],
                   "objects": [{"p": [0, 0, 0], "s": [10, 1, 10]}, {"i": PREFAB_LEADERBOARD, "p": [0, 5, 0], "s": [4, 2, 1]}]}
            r = await socks[0].call("pubMR", "uploadM", json.dumps(doc), None, "", "", 1)
            check(r is not None, "the creator published a map with a LEADERBOARD: %r" % (r,))
            return socks, ids, tokens, map_name

        socks, ids, tokens, map_name = asyncio.run(setup())
        map_id = one("SELECT map_id FROM published_maps WHERE map_name = ?", map_name)
        for aid, score in ((ids[0], 300), (ids[1], 500)):
            q("INSERT INTO match_results (match_id, account_id, map, score) VALUES (?, ?, ?, ?)", "m-%d" % aid, aid, map_name, score)
        q("INSERT INTO ks_storage (account_id, map_id, skey, svalue, scope) VALUES (?, ?, 'coins', '5', 0), (?, ?, 'gems', '2', 0), "
          "(?, ?, 'gems', '9', 0)", ids[1], map_id, ids[1], map_id, ids[0], map_id)
        q("INSERT INTO published_mods (mod_id, mod_name, creator_id, mod_url, mod_info) VALUES (?, ?, ?, 'ua', '{}'), (?, ?, ?, 'ua', '{}')",
          1000000900 + int(stamp) % 50, "Hub Pack " + stamp, ids[0], 1000000950 + int(stamp) % 50, "Not Mine " + stamp, ids[1])
        map_page(port, ids, tokens, names, map_name, map_id, stamp)

        q("INSERT INTO follows (follower_id, followee_id) VALUES (?, ?), (?, ?)", ids[0], ids[1], ids[1], ids[0])

        async def rest():
            s = await Sock(port).open()
            f = await s.call("a", "a", 1, [names[1], None, tokens[1]])
            check(f and f[1] is None, "bob's socket stays signed in (online)")
            await friends(port, ids, tokens, names, s)
            await s.close()

        asyncio.run(rest())
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        brain.terminate()
        try:
            brain.wait(10)
        except subprocess.TimeoutExpired:
            brain.kill()
        log.close()
    print("hub creator e2e (native): " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

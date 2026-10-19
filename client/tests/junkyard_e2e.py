#!/usr/bin/env python3
"""The Junkyard's craft screen (client/src/ui/v10_junkyard.c) end to end against a local kn_brain
(sqlite, temp dir, fixture recipes from its config) and one headless client_bin on the v10 page:

  1. alice opens the Store (#menuBtnShop) and the home tab's Junkyard tile, which calls
     windows[13].craftMenu() -> the exchange's Junkyard tab
  2. the recipe cards are listed: a craftable recipe with its Craft button, a paid recipe the
     account cannot afford ("Not enough Junk"), a recipe short of a material ("Missing materials")
  3. Craft -> the confirmation -> Craft: the materials are taken and the result granted (database),
     the result popup shows the skin
  4. the search bar: a name that matches nothing shows "No items match your search."

  python3 -I client/tests/junkyard_e2e.py BUILD_DIR REPO_ROOT
Headless client_bin (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs) driven through
KRUNKNATIVE_UI_CMDS (ui / clickid / rect / textof / text / shot / quit); ports from server/tests/kn_proto.
Screenshots of each step are kept in the work directory when the run fails (KN_KEEP=1 keeps all).
77 = skip (no kn_brain / client_bin / websockets).
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
WORK = tempfile.mkdtemp(prefix="kn-junkyard.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")

# skins.json store indices: 1 Wood, 3 Wires (materials); 3283 Krystallizer, 3921 Plasma Cutter (craftable)
WOOD, WIRES, KRYSTALLIZER, PLASMA = 1, 3, 3283, 3921
RECIPES = [
    {"SID": 1, "SOUT": KRYSTALLIZER, "SIN": "%d,2,%d,1" % (WOOD, WIRES), "JUNK": 0, "DUR": 0, "CNT": -1, "DAT": 1700000000000},
    {"SID": 2, "SOUT": PLASMA, "SIN": "%d,1" % WOOD, "JUNK": 5, "DUR": 0, "CNT": -1, "DAT": 1700000000000},
    {"SID": 3, "SOUT": KRYSTALLIZER, "SIN": "%d,4" % WIRES, "JUNK": 0, "DUR": 0, "CNT": -1, "DAT": 1700000000000},
]


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


def wait(fn, timeout=25.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return fn()


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, "junk_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


class Client:
    """One headless client_bin on the v10 page, logged in as `name`."""

    def __init__(self, port, name, token, tag):
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
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                    "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UI_CMDS": self.cmds,
                    "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                    "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port})
        self.out = open(self.log, "w")
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT, "--offline"], stdout=self.out, stderr=subprocess.STDOUT,
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

    def ui(self, calls):
        self.cmd("ui " + calls)
        time.sleep(0.5)

    def click(self, elem_id, timeout=25.0):
        """clickid #id once it has a box (the exchange re-renders)."""
        end = time.time() + timeout
        while time.time() < end:
            mark = len(self.text())
            self.cmd("clickid " + elem_id)
            t0 = time.time()
            while time.time() - t0 < 1.5:
                tail = self.text()[mark:]
                if "#%s rect" % elem_id in tail:
                    time.sleep(0.5)
                    return True
                if "#%s: no box" % elem_id in tail:
                    break
                time.sleep(0.1)
            time.sleep(0.3)
        return False

    def has(self, elem_id, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            mark = len(self.text())
            self.cmd("rect " + elem_id)
            t0 = time.time()
            while time.time() - t0 < 1.5:
                tail = self.text()[mark:]
                if "#%s rect" % elem_id in tail:
                    return True
                if "#%s: no box" % elem_id in tail:
                    break
                time.sleep(0.1)
            time.sleep(0.3)
        return False

    def gone(self, elem_id, timeout=10.0):
        """the element has no box (closed popup) within the timeout"""
        end = time.time() + timeout
        while time.time() < end:
            mark = len(self.text())
            self.cmd("rect " + elem_id)
            t0 = time.time()
            while time.time() - t0 < 1.5:
                tail = self.text()[mark:]
                if "#%s: no box" % elem_id in tail:
                    return True
                if "#%s rect" % elem_id in tail:
                    break
                time.sleep(0.1)
            time.sleep(0.5)
        return False

    def type(self, elem_id, chars):
        self.click(elem_id)
        self.cmd("text " + chars)
        time.sleep(0.6)

    def textof(self, target, timeout=10.0):
        """[ui_cmds] textof: the element's text (whitespace folded), '' when it has none"""
        mark = len(self.text())
        self.cmd("textof " + target)
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(r"\[ui_cmds\] textof %s = '(.*)'" % re.escape(target), self.text()[mark:])
            if m:
                return m.group(1)
            time.sleep(0.1)
        return ""

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


def scenario(port, aid, token, name):
    c = Client(port, name, token, "alice")
    try:
        # 1: the Store's Junkyard tile (windows[13].craftMenu(), its onclick) opens the exchange's Junkyard
        check(c.click("menuBtnShop"), "alice: the Store button on the v10 page")
        c.ui("windows[13].craftMenu()")
        check(c.has("v10mkSide3", 25), "the Junkyard tile opens the exchange (the Junkyard tab)")

        # 2: the recipes: the craftable card, the paid one, the short one
        check(c.has("v10jyCraft1", 25), "the recipe list shows the craftable recipe's Craft button")
        grid = c.textof("jyGrid")
        check("Krystallizer" in grid, "the card names the result skin (Krystallizer)")
        check("Not enough Junk" in grid, "the paid recipe says Not enough Junk")
        check("Missing materials" in grid, "the recipe short of Wires says Missing materials")
        c.shot("junkyard")

        # 3: craft recipe 1 through the confirmation
        c.click("v10jyCraft1")
        check(c.has("v10jyCraftGo", 15), "Craft opens the confirmation (Craft / Cancel)")
        c.shot("confirm")
        c.click("v10jyCraftGo")
        check(c.has("v10jyDoneOk", 25), "the result popup shows after the craft")
        c.shot("result")
        check(wait(lambda: one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = ?", aid, KRYSTALLIZER) == 1, 10),
              "the result skin is granted (Krystallizer x1)")
        check(one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = ?", aid, WOOD) == 3, "2 Wood were taken (5 -> 3)")
        check(one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = ?", aid, WIRES) == 0, "1 Wires was taken (1 -> 0)")
        c.click("v10jyDoneOk")
        check(c.gone("v10jyDoneOk", 10), "the result popup closes")

        # 4: the search bar: no match, then the list comes back
        c.type("v10jySearch", "zzzq")
        time.sleep(0.5)
        check("No items match your search." in c.textof("jyGrid"), "a search that matches nothing: the empty line")
        c.shot("search")
    finally:
        c.close()


def main():
    (port,) = free_ports(1)
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")  # its own config: never the developer's config/config.json
    with open(cfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "junkyard-e2e-%d" % os.getpid()},
                   "accounts": {"signup_rewards": {"enabled": False}, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0,
                                "password_iterations": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json"),
                                "junkyard_recipes": {"recipes": RECIPES}}}, f)
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
        name = "junk%s" % (str(int(time.time()) % 100000))
        aid, token = asyncio.run(register(port, name))
        q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 5)", aid, WOOD)
        q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", aid, WIRES)
        scenario(port, aid, token, name)
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
    print("junkyard e2e (native): " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

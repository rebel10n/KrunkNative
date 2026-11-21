#!/usr/bin/env python3
"""v10.0.7 item styles and map-version revert through the native client, signed in against a local kn_brain
(sqlite, temp dir), one headless client_bin at a time:

  1. style picker (client/src/ui/style_picker.c from v10_market.c's My Inventory): the owner of KrunkCup Jerseys
     (store 7794, 48 styles) opens Market & Trading -> My Inventory -> the card's style (checkroom) button, picks style 3,
     Apply Style: the social `ist` frame persists item_styles (skin 7794, copy 0, style 3), the account's owned list
     then carries m.style 3, and a fresh client's picker opens with style 3 checked
  2. the next game join (server/src/accounts.c accounts_skin_styles_send, NET_EXT_KN_SKIN_STYLES): the owner, wearing
     the jerseys, plays on a brain-linked server_bin; the server's SKIN_STYLES reaches the client (`cosm` prints
     body=7794 style=3)
  3. map versions (client/src/ui/social_ui.c: updateMapTab(4) / revertMapVersion / revertMapVersionGo): a map published
     twice through `uploadM` (versions 1 and 2); its creator's hub map page shows the Versions tab, v2 (Current) and a
     Revert on v1; Revert -> the confirmation -> YES: the success message, v1's content is live as v3, the list
     refreshes to v3 (Current), v2, v1; another account's map page has no Versions tab

  python3 -I client/tests/item_style_map_revert_e2e.py BUILD_DIR REPO_ROOT
Headless (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs), driven through KRUNKNATIVE_UI_CMDS
(clickid / clicksel / rect / textof / shot), ports from kn_proto.free_ports(). Scratch under ~/.cache.
77 = skip (no kn_brain / client_bin / server_bin, or python websockets missing).
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

BUILD, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.getcwd())
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
CLIENT = os.path.join(BUILD, "client_bin")
SERVER = os.path.join(BUILD, "server_bin")
if not all(os.access(b, os.X_OK) for b in (BRAIN, CLIENT, SERVER)):
    print("needs kn_brain, client_bin and server_bin in %s: skip" % BUILD)
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

JERSEY, STYLE = 7794, 3  # KrunkCup Jerseys: 48 styles, a body cosmetic (type 2); deob-10.0.7.js:27102-27106
with open(os.path.join(ROOT, "assets", "skins.json")) as _f:
    STYLE_NAMES = json.load(_f)[JERSEY]["styles"]  # the swatch labels (style_picker.c style_html)
FAILS = []
PROCS = []
WORK = tempfile.mkdtemp(prefix="kn-style-map-revert.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "brain.db")
SECRET = "style-map-e2e-%d" % os.getpid()


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)
    return cond


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


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait(fn, timeout=25.0, step=0.2):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


class Sock:
    def __init__(self, port):
        self.port, self.ws = port, None

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


class Client:
    """One headless client_bin signed in as `name` (saved session), driven through KRUNKNATIVE_UI_CMDS."""

    def __init__(self, port, name, token, tag, args, script=None, storage=None, extra_env=None):
        self.cmds = os.path.join(WORK, tag + ".cmds")
        self.log = os.path.join(WORK, tag + ".log")
        self.tag = tag
        cfg = os.path.join(WORK, tag + "-cfg")
        os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
        for key, value in dict({"krunker_username": name, "krunker_token": token}, **(storage or {})).items():
            with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
                f.write(value)
        open(self.cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                    "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UPDATES_URL": "off",
                    "KRUNKNATIVE_UI_CMDS": self.cmds, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                    "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port})
        if script is not None:
            path = os.path.join(WORK, tag + ".script")
            with open(path, "w") as f:
                f.write(script)
            env["KRUNKNATIVE_INPUT_SCRIPT"] = path
        env.update(extra_env or {})
        self.out = open(self.log, "w")
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT] + args, stdout=self.out, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=env, cwd=ROOT, start_new_session=True)

    def text(self):
        return read(self.log)

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

    def click(self, elem_id, timeout=25.0):
        """clickid #id once it has a box"""
        end = time.time() + timeout
        while time.time() < end:
            m = self.ask("clickid " + elem_id, r"#%s (rect|: no box)" % re.escape(elem_id), 1.5)
            if m and m.group(1) == "rect":
                time.sleep(0.5)
                return True
            time.sleep(0.3)
        return False

    def clicksel(self, nth, sel, timeout=20.0):
        """clicksel the nth match of a selector once it has a box"""
        end = time.time() + timeout
        pat = r"sel %s\[%d\] (rect|: no box)" % (re.escape(sel), nth)
        while time.time() < end:
            m = self.ask("clicksel %d %s" % (nth, sel), pat, 1.5)
            if m and m.group(1) == "rect":
                time.sleep(0.5)
                return True
            time.sleep(0.3)
        return False

    def count(self, sel):
        m = self.ask("count " + sel, r"count %s = (\d+)" % re.escape(sel))
        return int(m.group(1)) if m else -1

    def textof(self, sel):
        m = self.ask("textof " + sel, r"textof %s = '(.*)'" % re.escape(sel))
        return m.group(1) if m else ""

    def text_has(self, sel, needle, timeout=15.0):
        return wait(lambda: needle in self.textof(sel), timeout)

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


def open_style_picker(c):
    """#menuBtnMarket -> Market -> My Inventory -> the jerseys card's style button (v10mk.style(7794, sid))"""
    check(c.has("menuBtnMarket", 60), "%s: the v10 page's Market & Trading item" % c.tag)
    c.click("menuBtnMarket")
    check(c.has("v10mkSide0", 25), "%s: the exchange popup" % c.tag)
    c.click("v10mkSide0")
    c.click("v10mkTab1")  # My Inventory
    ok = check(c.clicksel(0, ".style-icon", 30), "%s: My Inventory's jerseys card has the style (checkroom) button" % c.tag)
    return ok and check(wait(lambda: c.count(".swatch") > 0, 15), "%s: the style button opens the style picker" % c.tag)


def style_phase(port, aid, name, token):
    c = Client(port, name, token, "style", ["--offline"])
    try:
        if not open_style_picker(c):
            return
        c.shot("picker")
        row = c.textof(".swatch-row")
        check(c.count(".swatch") == 5 and all(n in row for n in STYLE_NAMES[:5]) and c.count(".swatch.selected") == 1
              and c.textof(".swatch.selected") == STYLE_NAMES[0],
              "the picker's first page: styles 0-4, style 0 (%s) checked: %r / %r" % (STYLE_NAMES[0], row, c.textof(".swatch.selected")))
        c.clicksel(STYLE, ".swatch")
        check(wait(lambda: c.textof(".swatch.selected") == STYLE_NAMES[STYLE], 10) and c.count(".swatch.selected") == 1,
              "stylePick(3) checks the fourth swatch, %s (%r)" % (STYLE_NAMES[STYLE], c.textof(".swatch.selected")))
        c.shot("picked")
        check(not q("SELECT 1 FROM item_styles WHERE account_id = ?", aid), "nothing is saved before Apply")
        c.clicksel(0, ".btn-apply")
        row = wait(lambda: q("SELECT style FROM item_styles WHERE account_id = ? AND skin_id = ? AND copy_id = 0", aid, JERSEY), 20)
        check(row == [(STYLE,)], "Apply Style: the social `ist` frame persisted item_styles style 3: %r" % (row,))
        check(wait(lambda: c.count(".swatch") == 0, 15),
              "the brain's istr reply closes the picker")
        c.shot("applied")
    finally:
        c.close()

    async def owned():
        s = await Sock(port).open()
        try:
            f = await s.call("a", "a", 1, [name, None, token])
            return [e for e in ((f[4][10] if f and f[4] else []) or []) if isinstance(e, dict) and e.get("ind") == JERSEY]
        finally:
            await s.close()
    rows = asyncio.run(owned())
    m = rows[0].get("m") if rows else None
    if isinstance(m, str):
        m = json.loads(m or "{}")
    check(rows and isinstance(m, dict) and m.get("style") == STYLE, "the account's owned list carries m.style 3: %r" % rows)

    # a fresh client reads the saved style back from its account: the picker opens on style 3
    c = Client(port, name, token, "style2", ["--offline"])
    try:
        if open_style_picker(c):
            sel = c.textof(".swatch.selected")
            check(c.count(".swatch.selected") == 1 and sel == STYLE_NAMES[STYLE],
                  "a new session's picker opens with style 3 (%s) checked: %r" % (STYLE_NAMES[STYLE], sel))
            c.shot("reopened")
    finally:
        c.close()


def join_phase(port, name, token):
    """the owner wears the jerseys and plays on a brain-linked server_bin: SKIN_STYLES carries style 3"""
    (node_port,) = free_ports(1)
    node_log = os.path.join(WORK, "node.log")
    node_cfg = os.path.join(WORK, "node.json")
    gs = {"node_id": "STYLEREV", "name": "style revert e2e", "listen_port": node_port, "public_host": "127.0.0.1",
          "region": "local", "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0,
          "ruleset": "v10", "bans_file": os.path.join(WORK, "bans.json"),
          "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % port, "brain_secret": SECRET,
          "finder_url": "http://127.0.0.1:%d" % port}
    with open(node_cfg, "w") as f:
        json.dump({"gameserver": gs}, f)
    node = subprocess.Popen([SERVER, "--headless", "--config", node_cfg, "--cfg", "none"], cwd=ROOT, stdout=open(node_log, "w"),
                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    PROCS.append(node)
    if not check(wait(lambda: "welcomed" in read(node_log) or "linked" in read(node_log), 30), "the game server links to the brain"):
        return
    script = "0 play\n" + "".join("%d cosm @g%d\n" % (t, t) for t in range(8000, 60001, 4000))
    c = Client(port, name, token, "join", [], script=script, storage={"bodyIndex": str(JERSEY)},
               extra_env={"KRUNKNATIVE_NET_TRANSPORT": "tcp", "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % port})
    try:
        m = wait(lambda: re.search(r"who=me hat=-?\d+ body=%d .*body_style=%d\b" % (JERSEY, STYLE), c.text()), 70, 1.0)
        check("connected to server" in c.text(), "the owner joined the brain-linked game server")
        check(bool(re.search(r"who=me hat=-?\d+ body=%d" % JERSEY, c.text())), "in game the owner's player wears the jerseys (body=%d)" % JERSEY)
        last = re.findall(r"who=me hat=-?\d+ body=-?\d+ .*?body_style=(-?\d+)", c.text())
        check(bool(m), "the server's SKIN_STYLES gave the owner's worn jerseys style 3 on join (body_style %r)" % (last[-3:],))
        check("inventory" in read(node_log) or "owns" in read(node_log), "the server read the owner's inventory from the brain")
    finally:
        c.close()
        node.terminate()
        try:
            node.wait(10)
        except subprocess.TimeoutExpired:
            node.kill()


def versions_phase(port, names, tokens, map_name, map_id, v1_data, v2_data):
    c = Client(port, names[0], tokens[0], "owner", ["--hub", "p=map&q=" + map_name])
    try:
        check(c.has("mapTabsP", 60), "the creator's map page")
        tabs = c.textof("#mapTabsP")
        check("Versions" in tabs, "the creator's map page has a Versions tab: %r" % tabs)
        check(c.clicksel(4, ".mapTabP") and c.text_has("#mapTabsP .activeTab", "Versions"), "click Versions (updateMapTab(4, true))")
        # the rows' text, whitespace folded: "v2 Current ... v1 ... Revert" (the v10.0.7 md-ver-* table) or "v2 (Current)"
        check(wait(lambda: re.search(r"v2 \(?Current", c.textof("#mapVersions")) and "v1" in c.textof("#mapVersions"), 20),
              "the Versions tab loaded the authenticated history")
        hist = c.textof("#mapVersions")
        check(re.search(r"v2 \(?Current.*v1", hist) and c.count("#mapVersions button") == 1 and "Revert" in c.textof("#mapVersions button"),
              "v2 is Current, v1 has the only Revert: %r" % hist)
        c.shot("versions")
        check(c.clicksel(0, "#mapVersions button"), "click v1's Revert (revertMapVersion(map, 1))")
        ask = c.textof("#genericPop") if c.has("confBtn", 15) else ""
        check(re.search(r"version 1\b|Restore this version", ask) and "YES" in ask, "Revert asks for confirmation: %r" % ask)
        c.shot("confirm")
        check(one("SELECT map_updatecounter FROM published_maps WHERE map_id = ?", map_id) == 2, "nothing changes before YES")
        c.click("confBtn")
        check(wait(lambda: one("SELECT map_updatecounter FROM published_maps WHERE map_id = ?", map_id) == 3, 20),
              "YES posts the revert (POST /maps/:id/versions/1/revert): the map's live version is now 3")
        done = wait(lambda: "Live version is now v3" in c.textof("#mapVersions") or "Map version restored." in c.textof("#genericPop"), 15)
        check(done, "the client reports the revert's success: %r / %r" % (c.textof("#mapVersions")[:200], c.textof("#genericPop")))
        live = one("SELECT map_data FROM published_maps WHERE map_id = ?", map_id)
        check(json.loads(live or "{}").get("objects") == json.loads(v1_data)["objects"], "v1's content is live again: %r" % (live or "")[:160])
        vs = q("SELECT version, map_data FROM map_versions WHERE map_id = ? ORDER BY version", map_id)
        check([v for v, _ in vs] == [1, 2, 3] and json.loads(vs[2][1])["objects"] == json.loads(v1_data)["objects"]
              and json.loads(vs[1][1])["objects"] == json.loads(v2_data)["objects"],
              "map_versions: v3 holds v1's content and v2 (the previous live data) is kept: %r" % [v for v, _ in vs])
        ok = wait(lambda: re.search(r"v3 \(?Current.*v2 .*v1 ", c.textof("#mapVersions")) and c.count("#mapVersions button") == 2, 15)
        check(ok, "the list refreshes: v3 (Current), v2 and v1 revertible: %r" % c.textof("#mapVersions"))
        c.shot("reverted")
    finally:
        c.close()

    c = Client(port, names[1], tokens[1], "other", ["--hub", "p=map&q=" + map_name])
    try:
        check(c.has("mapTabsP", 60), "another account's view of the map page")
        tabs = c.textof("#mapTabsP")
        check("Leaderboard" in tabs and "Versions" not in tabs and "Data" not in tabs and c.count(".mapTabP") == 3,
              "a non-owner gets About / Servers / Leaderboard only, no Versions tab: %r" % tabs)
        check(c.count("#mapVersions") == 0, "and no #mapVersions panel")
        c.shot("non-owner")
    finally:
        c.close()


def main():
    (port,) = free_ports(1)
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")  # its own config: never the developer's config/config.json
    with open(cfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "region": "local", "internal_secret": SECRET},
                   "finder": {"poll_interval_s": 1},
                   "accounts": {"signup_rewards": {"enabled": False}, "sale_grace_s": 0, "free_premium": False, "starting_kr": 0,
                                "login_throttle_s": 0, "market_min_level": 0, "trade_min_level": 0, "gift_min_level": 0,
                                "password_iterations": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, start_new_session=True)
    try:
        if not wait(lambda: re.search(r"listening on http://127\.0\.0\.1:%d" % port, read(os.path.join(WORK, "brain.log"))), 30, 0.1):
            raise RuntimeError("kn_brain did not start")
        stamp = str(int(time.time() * 1000) % 100000)
        names = ["styal%s" % stamp, "stybo%s" % stamp]
        map_name = "StyleRev_" + stamp
        v1 = {"name": map_name, "modes": [0], "spawns": [[0, 1, 0], [4, 1, 0]], "objects": [{"p": [0, 0, 0], "s": [10, 1, 10]}]}
        v2 = dict(v1, objects=[{"p": [0, 0, 0], "s": [30, 1, 30]}, {"p": [5, 1, 5], "s": [2, 2, 2]}])

        async def setup():
            ids, tokens = [], []
            for n in names:
                s = await Sock(port).open()
                f = await s.call("a", "a", 0, [n, "style_pass_1", None])
                assert f and f[1] is None, f
                ids.append(f[2])
                tokens.append(f[5])
                if n == names[0]:  # the creator publishes, then republishes: map_versions 1 and 2
                    r = await s.call("pubMR", "uploadM", json.dumps(v1), None, "first", "", 1)
                    check(r is not None and r[1] == "Map Published!", "the creator published the map: %r" % (r,))
                    r = await s.call("pubMR", "uploadM", json.dumps(v2), None, "second", "", 1)
                    check(r is not None and r[1] == "Map Updated!", "and republished it: %r" % (r,))
                await s.close()
            return ids, tokens

        ids, tokens = asyncio.run(setup())
        q("UPDATE players SET player_score = 2000000 WHERE player_id = ?", ids[0])
        q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", ids[0], JERSEY)
        map_id = one("SELECT map_id FROM published_maps WHERE map_name = ?", map_name)
        vs = q("SELECT version FROM map_versions WHERE map_id = ? ORDER BY version", map_id)
        check(map_id and vs == [(1,), (2,)] and one("SELECT map_updatecounter FROM published_maps WHERE map_id = ?", map_id) == 2,
              "the map has versions 1 and 2, v2 live: %r" % vs)

        style_phase(port, ids[0], names[0], tokens[0])
        join_phase(port, names[0], tokens[0])
        versions_phase(port, names, tokens, map_name, map_id, json.dumps(v1), json.dumps(v2))
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        for p in PROCS:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    p.kill()
        brain.terminate()
        try:
            brain.wait(10)
        except subprocess.TimeoutExpired:
            brain.kill()
        log.close()
    print("item style + map revert e2e (native): " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

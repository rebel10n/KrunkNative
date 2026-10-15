#!/usr/bin/env python3
"""The Social hub's account actions and statuses end to end (social.html, client/src/ui/social_ui.c).

  python3 -I services/brain/tests/social_hub_staff_e2e.py <native build dir> <repo root>

kn_brain on sqlite, a developer's headless hub client (`client_bin --hub`) and a plain account's raw
social socket. The developer clicks the hub's own buttons on another player's profile: Verify /
Unverify (ver), Give Badge / Remove (sbdg), Flag / Unflag (flg), Report (rpt), the KR giveaway
switch, and the chat header's presence. Every click is checked in the database and in the page
(textof / count / rect). The plain account's raw socket must be refused by the server.
77 = skip (no client_bin / kn_brain / websockets).
"""
import asyncio, json, os, re, shutil, sqlite3, subprocess, sys, tempfile, threading, time
try:
    import websockets
except ImportError:
    print("python websockets missing: skip"); sys.exit(77)

BUILD = os.path.abspath(sys.argv[1])
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.getcwd()
BRAIN, CLIENT = os.path.join(BUILD, "services", "brain", "kn_brain"), os.path.join(BUILD, "client_bin")
if not (os.path.exists(BRAIN) and os.path.exists(CLIENT)):
    print("needs client_bin and kn_brain in %s: skip" % BUILD); sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-hub-staff.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
PORT = free_ports(1)[0]


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


def wait_db(sql, args, pred, timeout=12.0):
    end = time.time() + timeout
    while time.time() < end:
        rows = q(sql, *args)
        if pred(rows):
            return rows
        time.sleep(0.25)
    return q(sql, *args)


async def register(name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT) as ws:
        await ws.send(json.dumps(["a", 0, [name, "social_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


def online_session(name, token, hold_s):
    """A plain social socket that stays signed in for hold_s seconds (presence: Online). The
    brain's shutdown at the end of the run closes it; that is not an error here."""
    async def run():
        try:
            async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT) as ws:
                await ws.send(json.dumps(["a", 1, [name, None, token]]))
                end = time.time() + hold_s
                while time.time() < end:
                    try:
                        f = json.loads(await asyncio.wait_for(ws.recv(), 0.5))
                        if f and f[0] == "pi":
                            await ws.send(json.dumps(["po"]))
                    except asyncio.TimeoutError:
                        pass
        except Exception:
            return
    t = threading.Thread(target=lambda: asyncio.run(run()), daemon=True)
    t.start()
    return t


def refused_frames(name, token, frames):
    """A plain account's socket: each frame is sent and its first reply returned."""
    async def run():
        out = []
        async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT) as ws:
            await ws.send(json.dumps(["a", 1, [name, None, token]]))
            while True:
                f = json.loads(await asyncio.wait_for(ws.recv(), 20))
                if f[0] == "a":
                    break
            for fr in frames:
                await ws.send(json.dumps(fr))
                while True:
                    r = json.loads(await asyncio.wait_for(ws.recv(), 20))
                    if r[0] in ("verr", "sbdgr", "fgr", "po"):
                        out.append(r)
                        break
        return out
    return asyncio.run(run())


class Hub:
    """client_bin --hub <query>, headless, logged in as name (its token in the config directory)."""

    def __init__(self, name, token, tag, query):
        self.tag = tag
        self.cmds = os.path.join(WORK, tag + ".cmds")
        self.log = os.path.join(WORK, tag + ".log")
        cfg = os.path.join(WORK, tag + "-cfg")
        os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
        open(os.path.join(cfg, "krunknative", "krunker_username.json"), "w").write(name)
        open(os.path.join(cfg, "krunknative", "krunker_token.json"), "w").write(token)
        open(self.cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                    "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UI_CMDS": self.cmds, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                    "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % PORT})
        self.out = open(self.log, "w")
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT, "--hub", query], stdout=self.out, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=env, cwd=ROOT, start_new_session=True)

    def text(self):
        try:
            return open(self.log, errors="replace").read()
        except OSError:
            return ""

    def cmd(self, line):
        open(self.cmds, "a").write(line + "\n")

    def mark(self):
        return len(self.text())

    def wait_line(self, pattern, mark, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pattern, self.text()[mark:])
            if m:
                return m
            time.sleep(0.15)
        return None

    def rect(self, elem_id, timeout=6.0):
        """(w, h) of #elem_id, or None when it is not in the page."""
        end = time.time() + timeout
        while time.time() < end:
            m = self.mark()
            self.cmd("rect " + elem_id)
            r = self.wait_line(r"#%s rect ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)" % re.escape(elem_id), m, 2.0)
            if r:
                return float(r.group(3)), float(r.group(4))
            time.sleep(0.2)
        return None

    def visible(self, elem_id, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            r = self.rect(elem_id, 2.0)
            if r and r[0] > 0 and r[1] > 0:
                return True
            time.sleep(0.3)
        return False

    def text_of(self, selector, timeout=6.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.mark()
            self.cmd("textof " + selector)
            r = self.wait_line(r"\[ui_cmds\] textof %s = '(.*)'" % re.escape(selector), m, 2.0)
            if r:
                return r.group(1)
            time.sleep(0.2)
        return None

    def count(self, selector, timeout=6.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.mark()
            self.cmd("count " + selector)
            r = self.wait_line(r"\[ui_cmds\] count %s = (\d+)" % re.escape(selector), m, 2.0)
            if r:
                return int(r.group(1))
            time.sleep(0.2)
        return None

    def click(self, elem_id):
        self.cmd("clickid " + elem_id)
        time.sleep(0.6)

    def ui(self, calls):
        self.cmd("ui " + calls)
        time.sleep(0.8)

    def type(self, elem_id, chars):
        self.click(elem_id)
        self.cmd("text " + chars)
        time.sleep(0.5)

    def shot(self, name):
        path = os.path.join(WORK, "%s-%s.png" % (self.tag, name))
        self.cmd("shot " + path)
        time.sleep(0.8)
        return path

    def close(self):
        self.cmd("quit")
        try:
            self.proc.wait(15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(5)
        self.out.close()


def main():
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
               "brain": {"host": "127.0.0.1", "port": PORT, "federation": False, "internal_secret": "hubstaff-%d" % os.getpid()},
               "accounts": {"signup_rewards": {"enabled": False}, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0,
                            "password_iterations": 1000, "news": ["<b>KN news</b>"], "giveaway_check_ms": 500,
                            "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, open(cfg, "w"))
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    hub = None
    peer = None
    try:
        for _ in range(200):
            try:
                if re.search(r"listening on http://127\.0\.0\.1:%d" % PORT, open(os.path.join(WORK, "brain.log")).read()):
                    break
            except OSError:
                pass
            time.sleep(0.1)
        stamp = str(int(time.time()) % 100000)
        names = ["hsdev%s" % stamp, "hstgt%s" % stamp, "hsout%s" % stamp, "hsoff%s" % stamp]
        ids, tokens = [], []
        for n in names:
            i, t = asyncio.run(register(n))
            ids.append(i)
            tokens.append(t)
        dev, tgt, outsider, offline = ids
        dev_n, tgt_n, out_n, off_n = names
        q("UPDATE players SET player_developer = 1 WHERE player_id = ?", dev)
        q("UPDATE players SET player_score = 50000 WHERE player_id IN (?, ?)", tgt, dev)
        # the target is online (its own social socket); the fourth account never signs in
        peer = online_session(tgt_n, tokens[1], 150)
        time.sleep(1.0)

        hub = Hub(dev_n, tokens[0], "dev", "p=profile&q=" + tgt_n)
        check(hub.wait_line(r"\[hub\] enter", 0, 25.0) is not None, "the hub entered its page (p=profile)")
        check(hub.visible("verifyBtn", 20.0), "a developer sees Verify on another player's profile")
        check(hub.visible("badgeBtn"), "a developer sees Give Badge")
        check(hub.visible("flagBtn"), "a developer sees the flag button")
        check(hub.count(".onStatProf[title=\"Online\"]") == 1, "the target's profile dot is Online (its socket is signed in)")
        check(hub.text_of("verifyBtn") == "Verify", "the button reads Verify (the target is not verified)")

        # ---- verify, then unverify (ver -> verr; the page shows the new state)
        hub.click("verifyBtn")
        check(hub.visible("confBtn"), "Verify opens the confirm popup")
        hub.click("confBtn")
        rows = wait_db("SELECT player_featured FROM players WHERE player_id = ?", (tgt,), lambda r: r and r[0][0] == 1)
        check(rows and rows[0][0] == 1, "Verify: players.player_featured = 1 (the ver request reached kn_brain)")
        check("SUCCESS" in (hub.text_of(".responsePop") or ""), "Verify: the SUCCESS popup shows")
        hub.ui("clearPops()")
        time.sleep(1.0)
        check(hub.text_of("verifyBtn") == "Unverify", "after Verify the button reads Unverify (the profile reloaded)")
        hub.shot("verified")
        hub.click("verifyBtn")
        check(hub.visible("confBtn"), "Unverify opens the confirm popup")
        hub.click("confBtn")
        rows = wait_db("SELECT player_featured FROM players WHERE player_id = ?", (tgt,), lambda r: r and r[0][0] == 0)
        check(rows and rows[0][0] == 0, "Unverify: players.player_featured = 0")
        hub.ui("clearPops()")
        time.sleep(1.0)
        check(hub.text_of("verifyBtn") == "Verify", "after Unverify the button reads Verify again")

        # ---- give a badge, then remove it (sbdg -> sbdgr; the Badges tab shows the earned badge)
        hub.click("badgeBtn")
        check(hub.visible("confirmBtn"), "Give Badge opens the badge popup (Give / Remove)")
        hub.click("confirmBtn")
        rows = wait_db("SELECT badge FROM player_badges WHERE account_id = ?", (tgt,), lambda r: bool(r))
        check(rows and rows[0][0] == 0, "Give Badge: player_badges has the target's badge 0 (sbdg reached kn_brain)")
        log_rows = q("SELECT action FROM moderation_log WHERE target_id = ? AND action = 'badge'", tgt)
        check(bool(log_rows), "Give Badge: logged in moderation_log as 'badge'")
        check("SUCCESS" in (hub.text_of(".responsePop") or ""), "Give Badge: the SUCCESS popup shows")
        hub.ui("clearPops()")
        time.sleep(1.0)
        check("Legacy Challenger" in (hub.text_of("pTabContent_badges") or ""),
              "the profile's Badges tab lists the badge after Give (the page reloaded)")
        hub.click("badgeBtn")
        check(hub.visible("declineBtn"), "Remove is offered in the badge popup")
        hub.click("declineBtn")
        rows = wait_db("SELECT badge FROM player_badges WHERE account_id = ?", (tgt,), lambda r: not r)
        check(not rows, "Remove: the badge row is gone from player_badges")
        hub.ui("clearPops()")
        time.sleep(1.0)
        check("No Badges found" in (hub.text_of("pTabContent_badges") or "") or "No Badges" in (hub.text_of("pTabContent_badges") or ""),
              "after Remove the Badges tab shows the empty state")

        # ---- flag and unflag (fgr: players.player_hack)
        hub.click("flagBtn")
        check(hub.visible("confBtn"), "Flag opens the confirm popup")
        hub.click("confBtn")
        rows = wait_db("SELECT player_hack FROM players WHERE player_id = ?", (tgt,), lambda r: r and r[0][0] == 1)
        check(rows and rows[0][0] == 1, "Flag: players.player_hack = 1")
        hub.ui("clearPops()")
        time.sleep(1.0)
        hub.click("flagBtn")
        hub.click("confBtn")
        rows = wait_db("SELECT player_hack FROM players WHERE player_id = ?", (tgt,), lambda r: r and r[0][0] == 0)
        check(rows and rows[0][0] == 0, "Unflag: players.player_hack = 0")
        hub.ui("clearPops()")

        # ---- report (rpt -> rptr): too short is refused in the page, a full report is stored
        hub.click("reprtBtn")
        check(hub.visible("reportBtn"), "Report opens the report popup")
        hub.type("reportTxt", "short")
        hub.click("reportBtn")
        time.sleep(0.8)
        check((hub.text_of("reportError") or "") == "More information required.", "Report: details under 10 characters are refused in the page")
        hub.type("reportTxt", "This player is aimbotting in every game")
        hub.click("reportBtn")
        rows = wait_db("SELECT reason FROM reports WHERE reporter_id = ? AND target_id = ?", (dev, tgt), lambda r: bool(r))
        check(rows and rows[0][0] == "Hacking", "Report: a reports row with the chosen reason (rpt reached kn_brain)")
        check("SUCCESS" in (hub.text_of(".responsePop") or ""), "Report: the SUCCESS popup shows")
        hub.ui("clearPops()")

        # ---- presence: a signed-out target is Offline in the profile and the chat header
        hub.ui("updateWindow('profile','%s')" % off_n)
        time.sleep(2.0)
        check(hub.count(".onStatProf[title=\"Offline\"]") == 1, "the signed-out account's profile dot is Offline")
        hub.ui("updateWindow('profile','%s')" % tgt_n)
        time.sleep(2.0)
        check(hub.count(".onStatProf[title=\"Online\"]") == 1, "back on the online target: the dot is Online again")
        hub.ui("startChat(%d,0,'%s',null,1)" % (tgt, tgt_n))
        time.sleep(2.0)
        check(hub.text_of("chatStatus") == "Online", "chat header: the peer reads Online")
        hub.ui("startChat(%d,0,'%s',null,1)" % (offline, off_n))
        time.sleep(2.0)
        check(hub.text_of("chatStatus") == "Offline", "chat header: a signed-out peer reads Offline")
        hub.shot("chat-status")

        # ---- the KR giveaway switch of the post box (togglPos)
        hub.ui("updateWindow('feed')")
        time.sleep(2.0)
        hub.click("givaButton")
        check(hub.visible("krInptHol"), "the KR giveaway switch shows the KR field")
        hub.click("givaButton")
        time.sleep(0.5)
        check(not hub.visible("krInptHol", 2.0), "the switch hides the KR field again")

        # ---- the server refuses a plain account (no developer role)
        got = refused_frames(out_n, tokens[2],
                             [["ver", tgt_n, 1], ["sbdg", tgt_n, 0, 1], ["flg", tgt_n, 1]])
        check(len(got) == 3 and got[0][0] == "verr" and got[0][1] is None, "a plain account's ver is refused (verr null)")
        check(len(got) == 3 and got[1][0] == "sbdgr" and got[1][1] is None, "a plain account's sbdg is refused (sbdgr null)")
        check(len(got) == 3 and got[2][0] == "fgr" and got[2][1] is None, "a plain account's flg is refused (fgr null)")
        check(q("SELECT player_featured, player_hack FROM players WHERE player_id = ?", tgt)[0] == (0, 0),
              "the refused requests changed nothing")
        check(not q("SELECT 1 FROM player_badges WHERE account_id = ?", tgt), "the refused badge left no row")
    except Exception as e:
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        if hub:
            hub.close()
        brain.terminate()
        try:
            brain.wait(10)
        except subprocess.TimeoutExpired:
            brain.kill()
        log.close()
    print("social hub staff e2e: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

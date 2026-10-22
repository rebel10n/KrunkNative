#!/usr/bin/env python3
"""Stuck "Loading" screens of the native client (Junkyard recipes, the exchange's Trading tab), end to end
against a local kn_brain (sqlite, temp dir) and headless client_bin on the v10 page:

  junkyard-race   the Junkyard is opened before the session is restored (the brain is still down at
                  launch); when the brain answers the account is logged in and the recipes load
  junkyard-lost   the brain goes away while the Junkyard waits for its recipes: the screen says so
                  (the recipes were requested, no answer came) instead of "Loading Recipes..." for good
  trading-race    the same for the exchange's Trading tab (its trades load once logged in)
  trading-lost    the brain goes away while Trading waits for its trades: "Couldn't load trades."
  inbox-lost      the brain goes away while the Inbox (window 30) waits for its mail list: the list says
                  it could not load the notifications
  mail-lost       the brain goes away while a message's text is on its way: the message says so
  mail-refused    the brain answers a message as gone (its row was deleted): "Loading..." is replaced
  clan-lost       the brain is gone when the Clan window opens (no clan yet): the window says so, with Retry
  clan-settings-lost     the clan is loaded, the brain goes away, Settings is opened: the message of the day,
                  the loot config and the recruitment post each say they could not load, with Retry
  clan-settings-refused  the clan is loaded, the clan is deleted (the brain answers its routes 404), Settings
                  is opened: the loot config says it could not load
  lottery-lost    the brain is gone when the lottery window opens: the window says so, with Retry
  lottery-refused the brain answers the lottery as not found: the window says so, with Retry

  python3 -I client/tests/loading_screens_e2e.py BUILD_DIR REPO_ROOT [scenario ...]
Headless client_bin (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs) driven through
KRUNKNATIVE_UI_CMDS (ui / clickid / rect / textof / shot / quit); ports from server/tests/kn_proto.
The text of the exchange's content (#v10mkContent) is what each step reads. 77 = skip.
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
WORK = tempfile.mkdtemp(prefix="kn-loading.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
TAB_TRADING, TAB_JUNKYARD = 1, 3  # v10_market.c's TAB_* (the sidebar's v10mkSide<n>)
MAILS = {}        # the seeded messages' ids: "lost" (the brain goes away) and "refused" (its row is deleted)
ACCOUNT_ID = 0    # alice's account id
WOOD, WIRES, KRYSTALLIZER, PLASMA = 1, 3, 3283, 3921
RECIPES = [
    {"SID": 1, "SOUT": KRYSTALLIZER, "SIN": "%d,2,%d,1" % (WOOD, WIRES), "JUNK": 0, "DUR": 0, "CNT": -1, "DAT": 1700000000000},
    {"SID": 2, "SOUT": PLASMA, "SIN": "%d,1" % WOOD, "JUNK": 5, "DUR": 0, "CNT": -1, "DAT": 1700000000000},
]
# the brain's own timing: a reply that never comes is shown as failed after the screen's deadline (15 s)
WAIT_S = 30.0


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

    def __init__(self, port, name, token, tag, extra_env=None):
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
        # no production: krunker.io, its matchmaker and finder point at a closed local port (connection refused)
        for k in ("KRUNKNATIVE_KRUNKER_URL", "KRUNKNATIVE_KRUNKER_MATCHMAKER", "KRUNKNATIVE_FINDER_URL", "KRUNKNATIVE_KRUNKER_API",
                  "KRUNKNATIVE_KRUNKER_AUTH", "KRUNKNATIVE_KRUNKER_SOCIAL", "KRUNKNATIVE_KRUNKER_EVENT_BUS", "KRUNKNATIVE_KRUNKER_DATA_API"):
            env[k] = "http://127.0.0.1:9"
        env.update(extra_env or {})
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




class Brain:
    """kn_brain on a scratch database: start, stop (the brain goes away), start again on the same port."""

    def __init__(self, port):
        self.port = port
        self.log = os.path.join(WORK, "brain.log")
        self.proc = None

    def start(self):
        cfg = os.path.join(WORK, "brain.json")  # its own config: never the developer's config/config.json
        with open(cfg, "w") as f:
            json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                       "brain": {"host": "127.0.0.1", "port": self.port, "federation": False,
                                 "internal_secret": "loading-e2e-%d" % os.getpid()},
                       "accounts": {"signup_rewards": {"enabled": False}, "sale_grace_s": 0, "free_premium": False, "starting_kr": 0,
                                    "login_throttle_s": 0, "market_min_level": 0, "trade_min_level": 0, "gift_min_level": 0,
                                    "password_iterations": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json"),
                                    "junkyard_recipes": {"recipes": RECIPES}}}, f)
        out = open(self.log, "a")
        self.proc = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                                     start_new_session=True)
        out.close()
        for _ in range(300):
            try:
                with open(self.log) as f:
                    if len(re.findall(r"listening on http://127\.0\.0\.1:%d" % self.port, f.read())) >= 1 and self.up():
                        return True
            except OSError:
                pass
            time.sleep(0.1)
        return False

    def up(self):
        import socket
        try:
            with socket.create_connection(("127.0.0.1", self.port), 1):
                return True
        except OSError:
            return False

    def stop(self):
        if self.proc is None:
            return
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.proc = None
        for _ in range(100):
            if not self.up():
                return
            time.sleep(0.1)


def open_exchange(c, tab):
    """the Store's button, then the exchange: the Junkyard tile (windows[13].craftMenu()) opens its Junkyard tab,
    and the sidebar's tab is clicked for any other"""
    check(c.click("menuBtnShop"), "the Store button on the v10 page")
    c.ui("windows[13].craftMenu()")
    if tab != TAB_JUNKYARD:
        c.click("v10mkSide%d" % tab)
    return c.has("v10mkSide%d" % tab, 25)


def content(c):
    return c.textof("v10mkContent", timeout=10.0)


def wait_text(c, elem, pred, timeout=WAIT_S):
    end = time.time() + timeout
    text = ""
    while time.time() < end:
        text = c.textof(elem, timeout=10.0)
        if pred(text):
            return True, text
        time.sleep(1.0)
    return False, text


def wait_content(c, pred, timeout=WAIT_S):
    end = time.time() + timeout
    text = ""
    while time.time() < end:
        text = content(c)
        if pred(text):
            return True, text
        time.sleep(1.0)
    return False, text


def logged_in(c, timeout=25.0):  # the account's login line in the client log
    end = time.time() + timeout
    while time.time() < end:
        if "[account] logged in as" in c.text():
            return True
        time.sleep(0.3)
    return False


def new_account(brain, name):
    aid, token = asyncio.run(register(brain.port, name))  # the brain is up
    q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 5)", aid, WOOD)
    q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", aid, WIRES)
    q("UPDATE players SET player_score = 2000000, player_premium = 0, player_funds = 5000 WHERE player_id = ?", aid)
    for key, subject in (("lost", "Lost mail"), ("refused", "Refused mail")):
        q("INSERT INTO mail (account_id, sender, subject, body, kr) VALUES (?, 'Krunker', ?, 'Read me', 0)", aid, subject)
        MAILS[key] = one("SELECT id FROM mail WHERE account_id = ? AND subject = ?", aid, subject)
    global ACCOUNT_ID
    ACCOUNT_ID = aid
    return aid, token


def make_clan(aid):
    """alice leads a clan of her own (the Clan window and its Settings are hers)"""
    q("INSERT INTO clans (clan_name, clan_playerid, clan_funds) VALUES ('LoadClan', ?, 50000)", aid)
    cid = one("SELECT clan_id FROM clans WHERE clan_name = 'LoadClan'")
    q("INSERT INTO clan_members (account_id, clan_id, rank) VALUES (?, ?, 0)", aid, cid)
    return cid


def drop_clan(aid):
    q("DELETE FROM clan_members WHERE account_id = ?", aid)
    q("DELETE FROM clans WHERE clan_name = 'LoadClan'")


def scenario_junkyard_race(brain, name, token):
    print("junkyard-race: the Junkyard is opened before the session is restored", flush=True)
    brain.stop()  # the brain is down at launch: the client opens as a guest
    c = Client(brain.port, name, token, "jy-race")
    try:
        check(c.click("menuBtnShop"), "the Store button on the v10 page (guest, brain down)")
        c.ui("windows[13].craftMenu()")
        check(c.has("v10mkSide%d" % TAB_JUNKYARD, 25), "the Junkyard tab opens")
        time.sleep(1.0)
        brain.start()
        check(logged_in(c), "the saved session is restored once the brain answers")
        ok, text = wait_content(c, lambda t: "Krystallizer" in t)
        check(ok, "the Junkyard shows the recipes once logged in (not Register / Loading): %r" % text[:120])
        c.shot("junkyard-race")
    finally:
        c.close()


def scenario_junkyard_lost(brain, name, token):
    print("junkyard-lost: the brain goes away while the Junkyard waits for its recipes", flush=True)
    assert brain.start(), "kn_brain did not come up"
    c = Client(brain.port, name, token, "jy-lost")
    try:
        check(logged_in(c), "alice is logged in")
        open_exchange(c, TAB_JUNKYARD)
        ok, text = wait_content(c, lambda t: "Krystallizer" in t)
        check(ok, "the recipes load while the brain is up")
        brain.stop()
        c.ui("windows[13].craftMenu()")  # the Junkyard tab entered again: its recipes are asked for
        ok, text = wait_content(c, lambda t: "Couldn't load the recipes" in t, WAIT_S)
        check(ok, "no answer: the Junkyard says it could not load the recipes (not Loading for good): %r" % text[:120])
        c.shot("junkyard-lost")
    finally:
        c.close()


def scenario_trading_race(brain, name, token):
    print("trading-race: the Trading tab is opened before the session is restored", flush=True)
    brain.stop()
    c = Client(brain.port, name, token, "tr-race")
    try:
        check(c.click("menuBtnShop"), "the Store button on the v10 page (guest, brain down)")
        c.ui("windows[13].craftMenu()")
        check(c.click("v10mkSide%d" % TAB_TRADING), "the Trading tab opens")
        time.sleep(1.0)
        brain.start()
        check(logged_in(c), "the saved session is restored once the brain answers")
        ok, text = wait_content(c, lambda t: "No incoming trade offers" in t or "No trade history" in t)
        check(ok, "Trading loads its trades once logged in (not Loading / locked for good): %r" % text[:120])
        c.shot("trading-race")
    finally:
        c.close()


def scenario_trading_lost(brain, name, token):
    print("trading-lost: the brain goes away while the Trading tab waits for its trades", flush=True)
    assert brain.start(), "kn_brain did not come up"
    c = Client(brain.port, name, token, "tr-lost")
    try:
        check(logged_in(c), "alice is logged in")
        open_exchange(c, TAB_TRADING)
        ok, text = wait_content(c, lambda t: "No incoming trade offers" in t)
        check(ok, "the trades load while the brain is up")
        brain.stop()
        c.click("v10mkSide%d" % TAB_TRADING)  # entered again: the trades are asked for
        ok, text = wait_content(c, lambda t: "Couldn't load trades" in t, WAIT_S)
        check(ok, "no answer: Trading says it could not load the trades (not Loading trades... for good): %r" % text[:120])
        c.shot("trading-lost")
    finally:
        c.close()


def scenario_inbox_lost(brain, name, token):
    print("inbox-lost: the brain goes away while the Inbox waits for its mail list", flush=True)
    assert brain.start(), "kn_brain did not come up"
    c = Client(brain.port, name, token, "inbox-lost")
    try:
        check(logged_in(c), "alice is logged in")
        c.ui("showWindow(30)")
        ok, text = wait_text(c, "mailList", lambda t: "Loading" not in t and t.strip() != "")
        check(ok, "the Inbox lists its mail while the brain is up: %r" % text[:80])
        brain.stop()
        c.ui("windows[29].switchTab(3, 1)")  # another tab (forced): its list is asked for, and no answer comes
        ok, text = wait_text(c, "mailList", lambda t: "Couldn't load your notifications" in t, WAIT_S)
        check(ok, "no answer: the Inbox says it could not load the notifications (not Loading... for good): %r" % text[:80])
        c.shot("inbox-lost")
    finally:
        c.close()


def open_inbox(c):
    c.ui("showWindow(30)")
    c.ui("windows[29].switchTab(3, 1)")  # the Inbox tab, forced: its list is asked for
    return wait_text(c, "mailList", lambda t: "Lost mail" in t and "Refused mail" in t)


def scenario_mail_lost(brain, name, token):
    print("mail-lost: the brain goes away while a message's text is on its way", flush=True)
    assert brain.start(), "kn_brain did not come up"
    c = Client(brain.port, name, token, "mail-lost")
    try:
        check(logged_in(c), "alice is logged in")
        ok, text = open_inbox(c)
        check(ok, "the Inbox lists the messages while the brain is up: %r" % text[:80])
        brain.stop()
        el = "mail_%d" % MAILS["lost"]
        check(c.click(el), "the message is opened while the brain is gone")
        ok, text = wait_text(c, "#%s .mailText" % el, lambda t: "Couldn't load this message" in t, WAIT_S)
        check(ok, "no answer: the message says it could not load (not Loading... for good): %r" % text[:80])
        c.shot("mail-lost")
    finally:
        c.close()


def scenario_mail_refused(brain, name, token):
    print("mail-refused: the brain answers a message as gone (its row was deleted)", flush=True)
    assert brain.start(), "kn_brain did not come up"
    c = Client(brain.port, name, token, "mail-refused")
    try:
        check(logged_in(c), "alice is logged in")
        ok, text = open_inbox(c)
        check(ok, "the Inbox lists the messages while the brain is up: %r" % text[:80])
        q("DELETE FROM mail WHERE id = ?", MAILS["refused"])
        el = "mail_%d" % MAILS["refused"]
        check(c.click(el), "the message is opened (the brain answers it with no row)")
        ok, text = wait_text(c, "#%s .mailText" % el, lambda t: "Couldn't load this message" in t, WAIT_S)
        check(ok, "a refused read says it could not load (not Loading... for good): %r" % text[:80])
        c.shot("mail-refused")
    finally:
        c.close()


def clan_loaded(c):
    """the Clan window shows the clan (its tab content is out of the loading state)"""
    ok, text = wait_text(c, "#clanErr", lambda t: "LOADING" not in t and t.strip() != "")
    return ok, text


def scenario_clan_lost(brain, name, token):
    print("clan-lost: the brain is gone when the Clan window opens", flush=True)
    assert brain.start(), "kn_brain did not come up"
    make_clan(ACCOUNT_ID)
    c = Client(brain.port, name, token, "clan-lost")
    try:
        check(logged_in(c), "alice is logged in (a clan member)")
        brain.stop()
        c.ui("showWindow(13)")  # the Clan window (windows[12] is its handler namespace)
        ok, text = wait_text(c, "#clanErr", lambda t: "Couldn't load the clan" in t, WAIT_S)
        check(ok, "no answer: the Clan window says it could not load (not LOADING... for good): %r" % text[:80])
        c.shot("clan-lost")
    finally:
        c.close()
        drop_clan(ACCOUNT_ID)


def scenario_clan_settings_lost(brain, name, token):
    print("clan-settings-lost: the clan is loaded, the brain goes away, Settings is opened", flush=True)
    assert brain.start(), "kn_brain did not come up"
    make_clan(ACCOUNT_ID)
    c = Client(brain.port, name, token, "clan-settings-lost")
    try:
        check(logged_in(c), "alice is logged in (a clan leader)")
        c.ui("showWindow(13)")  # the Clan window (windows[12] is its handler namespace)
        ok, text = clan_loaded(c)
        check(ok, "the clan loads while the brain is up: %r" % text[:80])
        brain.stop()
        c.ui("windows[12].switchTab(4)")  # Settings: its sections are asked for now
        want = ("Couldn't load the message of the day", "Couldn't load the loot config", "Couldn't load the recruitment post")
        ok, text = wait_text(c, "#clanErr", lambda t: all(w in t for w in want), WAIT_S)
        check(ok, "no answer: each Settings section says it could not load, not Loading... for good: %r" % text[:160])
        c.shot("clan-settings-lost")
    finally:
        c.close()
        drop_clan(ACCOUNT_ID)


def scenario_clan_settings_refused(brain, name, token):
    print("clan-settings-refused: the account leaves the clan, the brain refuses its clan routes", flush=True)
    assert brain.start(), "kn_brain did not come up"
    make_clan(ACCOUNT_ID)
    c = Client(brain.port, name, token, "clan-settings-refused")
    try:
        check(logged_in(c), "alice is logged in (a clan leader)")
        c.ui("showWindow(13)")  # the Clan window (windows[12] is its handler namespace)
        ok, text = clan_loaded(c)
        check(ok, "the clan loads while the brain is up: %r" % text[:80])
        q("DELETE FROM clan_members WHERE account_id = ?", ACCOUNT_ID)
        q("DELETE FROM clans WHERE clan_name = 'LoadClan'")  # the brain answers the clan's routes 404 (Clan not found)
        c.ui("windows[12].switchTab(4)")
        ok, text = wait_text(c, "#clanErr", lambda t: "Couldn't load the loot config" in t, WAIT_S)
        check(ok, "a refused loot config says it could not load (not the silent Clan Bank form): %r" % text[:160])
        c.shot("clan-settings-refused")
    finally:
        c.close()
        drop_clan(ACCOUNT_ID)


def scenario_lottery_lost(brain, name, token):
    print("lottery-lost: the brain is gone when the lottery window opens", flush=True)
    brain.stop()
    c = Client(brain.port, name, token, "lottery-lost")
    try:
        c.ui("openLottery(3)")
        ok = c.has("knLotRetry", WAIT_S)
        ok2, text = wait_text(c, "#genericPop", lambda t: "Couldn't load the lottery" in t, 5.0)
        check(ok and ok2, "no answer: the lottery says it could not load, with Retry (not Loading...): %r" % text[:120])
        c.shot("lottery-lost")
    finally:
        c.close()


def scenario_lottery_refused(brain, name, token):
    print("lottery-refused: the brain answers the lottery as not found", flush=True)
    assert brain.start(), "kn_brain did not come up"
    q("DELETE FROM lotteries WHERE lottery_id = 3")
    c = Client(brain.port, name, token, "lottery-refused")
    try:
        c.ui("openLottery(3)")
        ok = c.has("knLotRetry", WAIT_S)
        ok2, text = wait_text(c, "#genericPop", lambda t: "Couldn't load the lottery" in t, 5.0)
        check(ok and ok2, "a refused lottery says it could not load, with Retry: %r" % text[:120])
        c.shot("lottery-refused")
    finally:
        c.close()
        q("INSERT INTO lotteries (lottery_id, title, theme, frequency_h, offset_s, active) VALUES (3, 'Anniversary', 'neutral', 12, 3600, 0)")


SCENARIOS = {
    "junkyard-race": scenario_junkyard_race,
    "junkyard-lost": scenario_junkyard_lost,
    "trading-race": scenario_trading_race,
    "trading-lost": scenario_trading_lost,
    "inbox-lost": scenario_inbox_lost,
    "mail-lost": scenario_mail_lost,
    "mail-refused": scenario_mail_refused,
    "clan-lost": scenario_clan_lost,
    "clan-settings-lost": scenario_clan_settings_lost,
    "clan-settings-refused": scenario_clan_settings_refused,
    "lottery-lost": scenario_lottery_lost,
    "lottery-refused": scenario_lottery_refused,
}


def main():
    names = sys.argv[3:] or list(SCENARIOS)
    (port,) = free_ports(1)
    brain = Brain(port)
    stamp = str(int(time.time()) % 100000)
    try:
        assert brain.start(), "kn_brain did not come up"
        account_name = "load%s" % stamp
        aid, token = new_account(brain, account_name)
        brain.stop()
        for n in names:
            # each scenario starts the brain as it needs it (the race ones start it mid-run); the account is kept
            SCENARIOS[n](brain, account_name, token)
            brain.stop()
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        brain.stop()
    print("loading screens e2e: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

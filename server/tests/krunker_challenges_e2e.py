#!/usr/bin/env python3
"""The Challenges window (v10's windows[32] / 33, client/src/ui/v10_rewards.c) on both account backends.

  python3 -I server/tests/krunker_challenges_e2e.py BUILD_DIR      (run from the repository root)

  T1  Box Office (kn_brain, signed in, the default seed): "Map Challenges" has a card for each map with a
      challenge, as v10.0.3 draws (`if (counts["map_" + i])`, deob-10.0.3.js:250887-250895): the seeded Burg /
      Sandstorm / Kanji by name (shared/src/map_catalog.c OFFICIAL entries, first letter capitalised as v10's
      UTILS.capFirst) and no other map; no "Loading" and no error text.
  T2  official krunker.io (the krunker_account_e2e stub; its account's activeChallenges carry one map challenge
      per official map, a "Complete all" map challenge with no mp, weapon rows, a nightmare row and a ranked
      row, each named "STUB ..."): the window lists all 46 maps, the Complete-all card, the nightmare and the
      "Ranked Challenges" header with its card; none of the brain's seeded names appear.
  T3  official, the stub account without an activeChallenges array: the headers, no map card, and no
      "Loading" / error text.
  T4  Box Office, signed in but kn_brain gone before the window's first request: the headers, no map card,
      and no "Loading" / error text.
  T5  the stub's activeChallenges as a capture, converted by tools/brain/challenges_from_capture.py into a
      migration; a kn_brain on it lists the same map and weapon rows (names, types, DAT, REW, icon) and the
      window shows the stub's no-map, nightmare and ranked rows. Doc: doc/v10/challenges.md 4c.

Headless client_bin (KRUNKNATIVE_HEADLESS=1, no DISPLAY, private XDG dirs) driven through KRUNKNATIVE_UI_CMDS
(ui / clickid / text / rect / textof / shot). The official runs go through xvfb-run when it is there (the
challenge webview needs a display; the game itself stays headless). Exit 0 when every check passed, 77 when
it cannot run.
"""
import asyncio
import json
import os
import re
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (its krunker.io stub, sign-in constants; its WORK dir is removed below)

try:
    import websockets
except ImportError:  # pragma: no cover
    websockets = None

shutil.rmtree(kae.WORK, ignore_errors=True)
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
CLIENT = os.path.join(BUILD, "client_bin")
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-challenges-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
DB = os.path.join(WORK, "coordinator.db")
ACCOUNT = "chal%d" % (int(time.time()) % 100000)
PASSWORD_SALT = "chal-e2e-%d" % os.getpid()
# the brain's seeded challenge names (services/db/migrations/sqlite/0107_progression.sql): none may show on the official type
SEEDED = ["Get 25 kills on Burg", "Win 3 matches on Sandstorm", "Get 5000 score on Kanji", "Get 10 melee kills with the Combat Knife"]
ERROR_RE = re.compile(r"Loading|Couldn.?t load|could not load|load(ing)? (failed|error)|Retry|error|failed", re.I)
FAILS = []
RESULTS = []
PROCS = []


def check(name, ok, detail=""):
    RESULTS.append(bool(ok))
    if not ok:
        FAILS.append(name)
    show = detail and (not ok or len(str(detail)) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)[:400]) if show else ''}", flush=True)


def map_catalogue():
    """(display name, mapIndex) of the 46 official maps, in mapIndex order, from shared/src/map_catalog.c"""
    with open(os.path.join(ROOT, "shared", "src", "map_catalog.c")) as f:
        src = f.read()
    rows = re.findall(r'OFFICIAL(?:_PIC)?\(\s*"[^"]*",\s*"([^"]*)",\s*(\d+)', src)
    maps = sorted(((int(i), n) for n, i in rows))
    return [(n[:1].upper() + n[1:], i) for i, n in maps]


MAPS = map_catalogue()
# the default seed's map challenges (services/db/migrations/sqlite/0107_progression.sql): Burg, Sandstorm, Kanji
SEEDED_MAPS = [0, 2, 10]


def has_name(text, name):
    # the folded window text runs names into counts ("47Burg0/1"): only letters and "_" bound a name
    return re.search(r"(?<![A-Za-z_])" + re.escape(name) + r"(?![A-Za-z_])", text) is not None


def wait(fn, secs=20.0):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.25)
    return None


def q(sql, *args):
    con = sqlite3.connect(DB, timeout=30)
    try:
        rows = con.execute(sql, args).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, "junk_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


class Brain:
    """kn_brain on a scratch database: start, stop, start again on the same port."""

    def __init__(self, port, db=DB, migrations=os.path.join(ROOT, "services", "db", "migrations")):
        self.port = port
        self.db = db
        self.migrations = migrations
        self.log = os.path.join(WORK, "brain.log")
        self.proc = None

    def start(self):
        cfg = os.path.join(WORK, "brain-%d.json" % self.port)
        with open(cfg, "w") as f:
            json.dump({"database": {"driver": "sqlite", "path": self.db, "migrations_dir": self.migrations},
                       "brain": {"host": "127.0.0.1", "port": self.port, "federation": False,
                                 "internal_secret": "challenges-e2e-%d" % os.getpid()},
                       "accounts": {"signup_rewards": {"enabled": False}, "sale_grace_s": 0, "free_premium": False, "starting_kr": 0,
                                    "login_throttle_s": 0, "market_min_level": 0, "trade_min_level": 0, "gift_min_level": 0,
                                    "password_iterations": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
        out = open(self.log, "a")
        self.proc = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                                     start_new_session=True)
        out.close()
        for _ in range(300):
            try:
                with open(self.log) as f:
                    if "listening on http://127.0.0.1:%d" % self.port in f.read() and self.up():
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
        if self.proc.poll() is None:
            os.killpg(self.proc.pid, signal.SIGTERM)  # our own brain (we started it)
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(5)
        self.proc = None


class Client:
    """One headless client_bin on the v10 page, driven through KRUNKNATIVE_UI_CMDS."""

    def __init__(self, tag, env_extra, port, name=None, token=None, prefix=()):
        self.cmds = os.path.join(WORK, tag + ".cmds")
        self.log = os.path.join(WORK, tag + ".log")
        self.tag = tag
        cfg = os.path.join(WORK, tag + "-cfg")
        os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
        if name:  # the Box Office account's login (the token files), never printed
            with open(os.path.join(cfg, "krunknative", "krunker_username.json"), "w") as f:
                f.write(name)
            with open(os.path.join(cfg, "krunknative", "krunker_token.json"), "w") as f:
                f.write(token)
        open(self.cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                    "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UI_CMDS": self.cmds,
                    "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_FINDER_URL": "off",
                    "KRUNKNATIVE_ASSET_FETCH": "off", "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS})  # the full assets tree (a worktree may lack models/)
        if port:
            env["KRUNKNATIVE_BRAIN_URL"] = "http://127.0.0.1:%d" % port
        # no production: krunker.io's services point at a closed local port unless the official run's stub is given
        for k in ("KRUNKNATIVE_KRUNKER_URL", "KRUNKNATIVE_KRUNKER_MATCHMAKER", "KRUNKNATIVE_KRUNKER_API", "KRUNKNATIVE_KRUNKER_AUTH",
                  "KRUNKNATIVE_KRUNKER_SOCIAL", "KRUNKNATIVE_KRUNKER_EVENT_BUS", "KRUNKNATIVE_KRUNKER_DATA_API"):
            env[k] = "http://127.0.0.1:9"
        env.update(env_extra)
        self.out = open(self.log, "w")
        # --debug: the sign-in checks read debug-level lines ("[account] logged in as", klog.h)
        self.proc = subprocess.Popen(list(prefix) + ["stdbuf", "-oL", "-eL", CLIENT, "--offline", "--debug"], stdout=self.out, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=env, cwd=ROOT, start_new_session=True)
        PROCS.append(self.proc)

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
                if "#%s rect" % elem_id in self.text()[mark:]:
                    return True
                if "#%s: no box" % elem_id in self.text()[mark:]:
                    break
                time.sleep(0.1)
            time.sleep(0.3)
        return False

    def type(self, elem_id, chars):
        self.click(elem_id)
        self.cmd("text " + chars)
        time.sleep(0.6)

    def window_text(self, timeout=10.0):
        """textContent of #menuWindow (the Challenges window), whitespace folded"""
        mark = len(self.text())
        self.cmd("textof menuWindow")
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(r"\[ui_cmds\] textof menuWindow = '(.*)'", self.text()[mark:])
            if m:
                return m.group(1)
            time.sleep(0.1)
        return ""

    def wait_window(self, marker, timeout=30.0):
        end = time.time() + timeout
        text = ""
        while time.time() < end:
            text = self.window_text()
            if marker in text:
                return text
            time.sleep(1.0)
        return text

    def shot(self, dest):
        path = os.path.join(WORK, "%s.png" % self.tag)
        self.cmd("shot " + path)
        if wait(lambda: os.path.exists(path), 15):
            shutil.copyfile(path, dest)
            return dest
        return None

    def close(self):
        self.cmd("quit")
        try:
            self.proc.wait(15)
        except subprocess.TimeoutExpired:
            os.killpg(self.proc.pid, signal.SIGKILL)  # our own client
            self.proc.wait(5)
        self.out.close()


def layout_checks(tag, text, want=None):
    """the layout: a card for exactly the maps in `want` (mapIndex list; None = every official map), the headers,
    and no load / error text"""
    want = [i for _, i in MAPS] if want is None else want
    names = [n for n, i in MAPS if i in want]
    missing = [n for n in names if not has_name(text, n)]
    check(f"{tag}: Map Challenges has the cards of its {len(names)} maps with challenges", not missing,
          "missing: " + ", ".join(missing[:12]) if missing else "")
    # a few names that are not also words of the window's text (challenge names, headers)
    extra = [n for n, i in MAPS if i not in want and n in ("Littletown", "Subzero", "Shipment", "Freight", "Lostworld")
             and has_name(text, n)]
    check(f"{tag}: no card for a map without challenges", not extra, ", ".join(extra))
    for head in ("Map Challenges", "Weapon Challenges", "Nightmare Challenges", "Ranked Challenges"):
        check(f"{tag}: the '{head}' header is shown", head in text, text[:120])
    err = ERROR_RE.search(text)
    check(f"{tag}: no 'Loading' and no error text", err is None, err.group(0) if err else "")


def box_office(brain_port, stub_results):
    """T1 and T4 on kn_brain: a signed-in Box Office account"""
    brain = Brain(brain_port)
    assert brain.start(), "kn_brain did not come up"
    aid, token = asyncio.run(register(brain_port, ACCOUNT))
    q("UPDATE players SET player_score = 2000000, player_premium = 0, player_funds = 5000 WHERE player_id = ?", aid)
    brain.stop()
    brain.start()
    c = Client("bo1", {}, brain_port, ACCOUNT, token)
    try:
        wait(lambda: "[account] logged in as" in c.text(), 40)
        check("T1: the Box Office account is signed in", "[account] logged in as" in c.text(), c.text()[-200:])
        c.ui("windows[32].openHome()")
        text = c.wait_window("Map Challenges", 40)
        shot = c.shot("/tmp/challenges-box-office.png")
        check("T1: the Challenges window is open with the brain's account", "Map Challenges" in text, text[:160])
        layout_checks("T1", text, SEEDED_MAPS)
        check("T1: the screenshot was saved", bool(shot), shot)
    finally:
        c.close()
    # T4: signed in, the brain goes away before the window's first request (no answer ever comes)
    c2 = Client("bo2", {}, brain_port, ACCOUNT, token)
    try:
        wait(lambda: "[account] logged in as" in c2.text(), 40)
        brain.stop()
        time.sleep(2)
        c2.ui("windows[32].openHome()")
        text = c2.wait_window("Map Challenges", 25)
        layout_checks("T4", text, [])
    finally:
        c2.close()
        brain.stop()


def official_run(tag, challenges, env_stub, brain_port):
    """an official krunker.io game as a krunker.io account, the stub's account data as given"""
    http_port, ws_port = kn_proto.free_ports(2)
    stub = kae.Stub(http_port, ws_port)
    stub.account_data = [challenges] if challenges is not None else []
    stub.start()
    base = "http://127.0.0.1:%d" % http_port
    env = {"KRUNKNATIVE_KRUNKER_MATCHMAKER": base + "/mm", "KRUNKNATIVE_KRUNKER_API": base + "/api",
           "KRUNKNATIVE_KRUNKER_AUTH": base + "/frvr/v1/auth", "KRUNKNATIVE_KRUNKER_CAPTCHA_URL": base + kae.CAPTCHA_PATH,
           "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS}
    prefix = ["xvfb-run", "-a"] if shutil.which("xvfb-run") else []
    c = Client(tag, env, brain_port, prefix=prefix)
    try:
        wait(lambda: c.has("menuBtnChall", 5), 60)
        c.ui('krunkerJoin("%s")' % kae.GAME_PICK)
        check(f"{tag}: the official game makes krunker.io the active account", wait(
            lambda: "[account] active account backend: krunker.io" in c.text(), 60) is not None, c.text()[-300:])
        c.ui("loginOrRegister()")
        c.click("knV10LoginMode")
        c.type("accName", kae.LOGIN)
        c.type("accPass", kae.PASSWORD)
        c.click("knV10LoginSubmit")
        time.sleep(8)
        for i, d in enumerate(kae.CODE):
            c.type("knV10Code%d" % i, d)
        got = wait(lambda: "the krunker.io account's data" in c.text(), 90)
        check(f"{tag}: signed in to krunker.io (its account data arrived)", got is not None, c.text()[-300:])
        c.ui("windows[32].openHome()")
        text = c.wait_window("Map Challenges", 40)
        return c, text, stub
    except Exception:
        c.close()
        raise


def official_challenges():
    """the stub's activeChallenges: one map challenge per official map, a Complete-all, weapons, nightmare, ranked"""
    rows = []
    ci = 100
    for _, idx in MAPS:
        rows.append({"CI": ci, "T": "map", "DAT": {"ky": "k", "val": 25, "mp": idx}, "REW": {"kr": 10}, "IC": 0,
                     "N": "STUB map %d" % idx, "PV": 3, "PT": 0})
        ci += 1
    rows.append({"CI": 200, "T": "map", "DAT": {"ky": "k", "val": 1000}, "REW": {"kr": 50}, "IC": 1, "N": "STUB complete all", "PV": 0, "PT": 0})
    rows.append({"CI": 300, "T": "weapon", "DAT": {"ky": "k", "val": 50, "wep": 1}, "REW": {"kr": 15}, "IC": 2, "N": "STUB weapon 1", "PV": 0, "PT": 0})
    rows.append({"CI": 301, "T": "weapon", "DAT": {"ky": "k", "val": 50, "wep": 3}, "REW": {"kr": 15}, "IC": 2, "N": "STUB weapon 3", "PV": 0, "PT": 0})
    rows.append({"CI": 400, "T": "nightmare", "DAT": {"ky": "kstk", "val": 15}, "REW": {"kr": 50}, "IC": 8, "N": "STUB nightmare", "PV": 0, "PT": 0})
    rows.append({"CI": 500, "T": "ranked", "DAT": {"ky": "k", "val": 10}, "REW": {"kr": 20}, "IC": 3, "N": "STUB ranked", "PV": 0, "PT": 0})
    return {"activeChallenges": rows}


def capture_run(port):
    """T5: the captured official list on Box Office. The stub's activeChallenges is written as a capture
    (the client's KRUNKNATIVE_CHALLENGE_CAPTURE shape), tools/brain/challenges_from_capture.py turns it into the
    migration in a copy of the migrations tree, and a kn_brain on that tree must list the same map and weapon
    rows (and the same no-map, nightmare and ranked rows the official window shows in T2)."""
    cap = os.path.join(WORK, "official-capture.json")
    with open(cap, "w", encoding="utf-8") as f:
        json.dump({"format": 1, "source": "krunker.io", "game": "10.0.3", "captured_at": "2026-10-22T00:00:00Z",
                   "activeChallenges": official_challenges()["activeChallenges"]}, f)
    mig = os.path.join(WORK, "migrations")
    shutil.copytree(os.path.join(ROOT, "services", "db", "migrations"), mig)
    tool = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "brain", "challenges_from_capture.py"), cap, "--out", mig],
                          capture_output=True, text=True)
    check("T5: the capture tool writes the migration", tool.returncode == 0, tool.stderr)
    if tool.returncode != 0:
        return
    db = os.path.join(WORK, "capture-brain.db")
    brain = Brain(port, db=db, migrations=mig)
    try:
        assert brain.start(), "kn_brain (capture migration) did not come up"
        aid, token = asyncio.run(register(port, ACCOUNT + "c"))
        c = Client("bo3", {}, port, ACCOUNT + "c", token)
        try:
            wait(lambda: "[account] logged in as" in c.text(), 40)
            c.ui("windows[32].openHome()")
            text = c.wait_window("Map Challenges", 40)
            for marker in ("STUB complete all", "STUB nightmare", "STUB ranked"):
                check(f"T5: Box Office shows the captured row '{marker}'", marker in text, text[:160])
        finally:
            c.close()
    finally:
        brain.stop()
    # the rows the brain lists (active) against the stub's list: the map and weapon rows, names and fields
    con = sqlite3.connect(db)
    got = sorted((r[0], r[1], json.loads(r[2]), json.loads(r[3]), r[4]) for r in con.execute(
        "SELECT name, ctype, dat, rew, icon FROM challenges WHERE active = 1 AND ctype IN ('map', 'weapon')"))
    con.close()
    want = sorted((r["N"], r["T"], r["DAT"], r["REW"], r["IC"]) for r in official_challenges()["activeChallenges"]
                  if r["T"] in ("map", "weapon"))
    check("T5: Box Office's active map and weapon rows are the stub's (names, types, DAT, REW, icon)", got == want,
          "%d brain rows, %d stub rows" % (len(got), len(want)))


def main():
    if websockets is None or kae.websockets is None:
        print("SKIP: python websockets is not installed")
        return 77
    if not (os.access(CLIENT, os.X_OK) and os.access(BRAIN, os.X_OK)):
        print("krunker_challenges_e2e: no client_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    if len(MAPS) != 46:
        print("krunker_challenges_e2e: the catalogue has %d official maps, expected 46" % len(MAPS))
        return 77
    http_brain, = kn_proto.free_ports(1)
    brain = Brain(http_brain)
    try:
        # the account's brain run (T1, T4)
        box_office(http_brain, None)
        # the official runs (T2, T3); the brain is up for the client's own requests
        assert brain.start(), "kn_brain did not come up"
        c, text, stub = official_run("off2", official_challenges(), None, http_brain)
        try:
            layout_checks("T2", text)
            for marker in ("STUB complete all", "STUB nightmare", "STUB ranked"):
                check(f"T2: the stub row '{marker}' is shown", marker in text, text[:160])
            check("T2: the Ranked Challenges header has its card (challRankedHeadr rect)", c.has("challRankedHeadr", 10), "")
            seeded = [s for s in SEEDED if s in text]
            check("T2: no Box Office seeded challenge is shown on the official type", not seeded, seeded)
            shot = c.shot("/tmp/challenges-official.png")
            check("T2: the official screenshot was saved", bool(shot), shot)
        finally:
            c.close()
        stub.stop()
        c3, text3, stub3 = official_run("off3", None, None, http_brain)
        try:
            layout_checks("T3", text3, [])
        finally:
            c3.close()
            stub3.stop()
        capture_run(kn_proto.free_ports(1)[0])
    except AssertionError as e:
        check("run: " + str(e), False, str(e))
    finally:
        brain.stop()
        for p in PROCS:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)  # only the clients this test started
    print(f"{sum(RESULTS)}/{len(RESULTS)} passed")
    if not FAILS:
        shutil.rmtree(WORK, ignore_errors=True)
        return 0
    print("logs kept in " + WORK)
    return 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""The HTML engine's layout at a real account's scale (client/src/html/engine/layout_*.c): a scratch kn_brain
(sqlite, temp dir) whose account owns every skin of the catalogue (assets/skins.json, thousands of inventory
rows, painted copies of hundreds of them), a second account with open listings, and a headless client_bin on
the v10 page logged in with a token:

  skins     Customize > Select Skin (window 15, the ui_loadout.c picker, a VGrid of ~600 primary skins): hover
            over cards, scroll the list with the wheel, type a search, hover again
  stats     Market & Trading > Stats (v10_market.c view_stats: the brain's 50-row answer) and hover / scroll

Every frame's layout is traced (KN_HTML_LAYOUT_TRACE=1: boxes, memo hits / misses, time) and the test fails when
a layout pass (or a whole engine frame) takes longer than LAYOUT_FREEZE_LIMIT_MS (default 1000: the freeze watchdog
reports at 5 s; a layout above a second is the super-linear case these regressions guard against). The user's
freeze reports of 2026-10-22 (14:31 Customize skins, 15:42 market Stats) put the main thread in layout_block.c.

  python3 -I client/tests/layout_freeze_e2e.py BUILD_DIR REPO_ROOT [scenario ...]
Headless client_bin (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs) driven by a timed input
script (KRUNKNATIVE_INPUT_SCRIPT). No production: krunker.io, its matchmaker and finder point at a closed local
port. LAYOUT_FREEZE_SHOTS=<dir> keeps the screenshots there. 77 = skip (no websockets, no kn_brain / client_bin).
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
WORK = tempfile.mkdtemp(prefix="kn-layout-freeze.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
LIMIT_MS = float(os.environ.get("LAYOUT_FREEZE_LIMIT_MS", "1000"))
WAIT_S = 30.0


def check(what, cond, detail=""):
    print(("  ok   " if cond else "  FAIL ") + what + ("" if cond or not detail else "  [%s]" % detail), flush=True)
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


def qmany(sql, rows):
    con = sqlite3.connect(DB, timeout=30)
    try:
        con.executemany(sql, rows)
        con.commit()
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
    """kn_brain on a scratch database."""

    def __init__(self, port):
        self.port = port
        self.log = os.path.join(WORK, "brain.log")
        self.proc = None

    def start(self):
        cfg = os.path.join(WORK, "brain.json")  # its own config: never the developer's config/config.json
        with open(cfg, "w") as f:
            json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                       "brain": {"host": "127.0.0.1", "port": self.port, "federation": False,
                                 "internal_secret": "layout-freeze-e2e-%d" % os.getpid()},
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
                    if re.search(r"listening on http://127\.0\.0\.1:%d" % self.port, f.read()) and self.up():
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


def catalogue():
    with open(os.path.join(ROOT, "assets", "skins.json")) as f:
        return json.load(f)


def seed_accounts(brain):
    """The collector owns every catalogue skin (1..3 units, a painted copy of every fourth one); the trader has an
    open listing of every tenth skin so the Stats rows carry On Sale / Low / High."""
    skins = catalogue()
    aid, token = asyncio.run(register(brain.port, "Collector"))
    tid, _ = asyncio.run(register(brain.port, "Trader"))
    qmany("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, ?)",
          [(aid, s["index"], 1 + (i % 3)) for i, s in enumerate(skins)])
    paints = ["#dc3c3c", "#3c8cdc", "#3cdc6e", "#dcc83c", "#b03cdc"]
    qmany("INSERT INTO skin_copies (account_id, skin_id, paint_color) VALUES (?, ?, ?)",
          [(aid, s["index"], paints[i % len(paints)]) for i, s in enumerate(skins) if i % 4 == 0])
    qmany("INSERT INTO market_listings (seller_id, skin_id, price) VALUES (?, ?, ?)",
          [(tid, s["index"], 100 + (i % 900)) for i, s in enumerate(skins) if i % 10 == 0])
    q("UPDATE players SET player_score = 2000000, player_premium = 0, player_funds = 5000 WHERE player_id = ?", aid)
    owned = q("SELECT count(*), sum(count) FROM inventory WHERE account_id = ?", aid)[0]
    copies = q("SELECT count(*) FROM skin_copies WHERE account_id = ?", aid)[0][0]
    print("seeded: %d owned skins (%d units), %d painted copies, %d listings" %
          (owned[0], owned[1], copies, q("SELECT count(*) FROM market_listings")[0][0]), flush=True)
    return aid, token


def run_client(tag, script, brain_url, token, name):
    cmds = os.path.join(WORK, tag + ".cmds")
    open(cmds, "w").close()
    cfg = os.path.join(WORK, tag + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative", "krunker_username.json"), "w") as f:
        f.write(name)
    with open(os.path.join(cfg, "krunknative", "krunker_token.json"), "w") as f:
        f.write(token)
    path = os.path.join(WORK, tag + ".script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, tag + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_UI_DIR")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_UI_CMDS": cmds, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": brain_url, "KN_HTML_LAYOUT_TRACE": "1", "KRUNKNATIVE_HTML_PROFILE": "1",
                "KRUNKNATIVE_HTML_SLOW_FRAME_MS": os.environ.get("LAYOUT_FREEZE_SLOW_MS", "100")})
    # no production: krunker.io, its matchmaker and finder point at a closed local port (connection refused)
    for k in ("KRUNKNATIVE_KRUNKER_URL", "KRUNKNATIVE_KRUNKER_MATCHMAKER", "KRUNKNATIVE_FINDER_URL", "KRUNKNATIVE_KRUNKER_API",
              "KRUNKNATIVE_KRUNKER_AUTH", "KRUNKNATIVE_KRUNKER_SOCIAL", "KRUNKNATIVE_KRUNKER_EVENT_BUS", "KRUNKNATIVE_KRUNKER_DATA_API"):
        env[k] = "http://127.0.0.1:9"
    # the sandbox game (--offline: the client's own server, the HUD and chat as the user had them) with the
    # brain's account; the menu is the v10 page (no finder: no Lost Connection screen replaces it)
    cmd = [CLIENT, "--offline", "--brain", brain_url, "--finder", "off"]
    if os.environ.get("LAYOUT_FREEZE_PERF"):  # LAYOUT_FREEZE_PERF=1: perf record the run (<work>/<tag>.perf.data)
        cmd = ["perf", "record", "-F", "499", "-g", "-o", os.path.join(WORK, tag + ".perf.data"), "--"] + cmd
    with open(log, "w") as out:
        proc = subprocess.Popen(cmd, cwd=ROOT, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                                start_new_session=True)
    return proc, log, cmds


def ui_cmd(cmds, line):
    with open(cmds, "a") as f:
        f.write(line + "\n")


def wait_log(proc, log, what, timeout=60.0):
    end = time.time() + timeout
    while time.time() < end and proc.poll() is None:
        with open(log, errors="replace") as f:
            if what in f.read():
                return True
        time.sleep(0.2)
    return False


def finish(proc, log, tag, timeout=240):
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        check("%s: client_bin finished its script" % tag, False, "timeout, log %s" % log)
    with open(log, errors="replace") as f:
        return f.read()


def rects(log, label):
    m = re.search(r"\[input_script\] rects t=\d+ @%s n=(\d+)" % re.escape(label), log)
    return int(m.group(1)) if m else -1


def shot_taken(path):
    dest_dir = os.environ.get("LAYOUT_FREEZE_SHOTS")
    if os.path.exists(path) and dest_dir:
        os.makedirs(dest_dir, exist_ok=True)
        shutil.copy(path, os.path.join(dest_dir, os.path.basename(path)))
    return os.path.exists(path)


TRACE = re.compile(r"\[layout trace\] (\S+) boxes=(\d+) laid=(\d+) time=([\d.]+)ms memo hit=(\d+) miss=(\d+) floats_iter=(\d+) "
                   r"boundaries=(\d+) relayouts=(\d+)")
SLOW = re.compile(r"\[html_engine slow frame\] ([\d.]+) ms: restyle ([\d.]+) \((\d+) elems\) layout ([\d.]+)")
HANDLERS = re.compile(r"handlers (\d+)/(\d+) \((\d+) tombs\)")


def layout_report(tag, log):
    """The slowest layout passes and engine frames of the run (the trace lines); the time bound check."""
    passes = [(float(m.group(4)), m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(5)), int(m.group(6)), int(m.group(7)),
               int(m.group(8)), int(m.group(9))) for m in TRACE.finditer(log)]
    frames = [(float(m.group(1)), float(m.group(2)), int(m.group(3)), float(m.group(4))) for m in SLOW.finditer(log)]
    passes.sort(reverse=True)
    frames.sort(reverse=True)
    print("  %s: %d layout passes, slowest:" % (tag, len(passes)), flush=True)
    for p in passes[:5]:
        print("    %8.1f ms %-8s boxes=%d laid=%d memo hit=%d miss=%d floats_iter=%d boundaries=%d relayouts=%d" % p, flush=True)
    for fr in frames[:5]:
        print("    frame %8.1f ms: restyle %.1f (%d elems) layout %.1f" % fr, flush=True)
    worst_pass = passes[0][0] if passes else 0.0
    worst_frame = frames[0][0] if frames else 0.0
    check("%s: layout passes were traced" % tag, len(passes) > 0)
    check("%s: no layout pass above %.0f ms (worst %.1f ms)" % (tag, LIMIT_MS, worst_pass), worst_pass < LIMIT_MS)
    check("%s: no engine frame above %.0f ms (worst %.1f ms)" % (tag, LIMIT_MS, worst_frame), worst_frame < LIMIT_MS)
    # the inline handler cache (events.c handler_lookup): the re-rendered cards leave tombstones; the
    # table must keep an empty slot (live + tombs under 3/4 of its capacity) or a probe never ends
    tables = [(int(m.group(1)), int(m.group(2)), int(m.group(3))) for m in HANDLERS.finditer(log)]
    worst = max(((live + tombs) * 100 // cap if cap else 0, live, cap, tombs) for live, cap, tombs in tables) if tables else None
    check("%s: the handler cache kept an empty slot (worst load %s%%: %s)" % (tag, worst[0] if worst else "-", worst),
          worst is not None and worst[0] <= 75)
    return passes, frames


def hover_lines(t, elem, n, step=120):
    """n cursor moves over an element's box (hover: tooltips, :hover restyles) in a zigzag."""
    out = []
    for i in range(n):
        fx = 0.1 + 0.8 * ((i * 7) % 10) / 10.0
        fy = 0.1 + 0.8 * ((i * 3) % 10) / 10.0
        out.append("%d cursorid %s %.2f %.2f" % (t + i * step, elem, fx, fy))
    return out, t + n * step


def wheel_lines(t, n, direction=-1, step=100):
    return ["%d wheel %d" % (t + i * step, direction) for i in range(n)], t + n * step


def scenario_skins(brain_url, token, name):
    print("scenario skins: Select Skin over the collector's inventory: hover, scroll, search, hover", flush=True)
    lines = ["0 boot", "1500 call skinSelector 1", "3000 rects @cards .skinCard", "3100 cursorid skinList 0.5 0.5"]
    t = 3200
    h, t = hover_lines(t, "skinList", 16)
    lines += h
    w, t = wheel_lines(t, 25)
    lines += w
    t += 400
    lines.append("%d rects @scrolled .skinCard" % t)
    t += 100
    h, t = hover_lines(t, "skinList", 10)
    lines += h
    lines += ["%d clickid itemSearch" % t, "%d type a" % (t + 300), "%d type k" % (t + 600), "%d type 4" % (t + 900),
              "%d rects @searched .skinCard" % (t + 1600)]
    t += 1800
    h, t = hover_lines(t, "skinList", 8)
    lines += h
    lines += ["%d clickid itemSearch" % t, "%d down BACKSPACE" % (t + 200), "%d down BACKSPACE" % (t + 400), "%d down BACKSPACE" % (t + 600)]
    t += 1200
    w, t = wheel_lines(t, 30)
    lines += w
    t += 300
    h, t = hover_lines(t, "skinList", 8)
    lines += h
    lines.append("%d rects @final .skinCard" % t)
    lines.append("%d quit" % (t + 500))
    proc, log, cmds = run_client("skins", "\n".join(lines) + "\n", brain_url, token, name)
    png = os.path.join(WORK, "skins.png")
    if wait_log(proc, log, "@scrolled n="):
        ui_cmd(cmds, "shot " + png)
    log = finish(proc, log, "skins")
    check("the Select Skin picker lists the owned cards (VGrid first chunk)", rects(log, "cards") >= 40, "n=%d" % rects(log, "cards"))
    check("the list scrolled (cards still rendered)", rects(log, "scrolled") >= 40, "n=%d" % rects(log, "scrolled"))
    check("the search narrowed the list", 0 <= rects(log, "searched") < rects(log, "cards"), "n=%d" % rects(log, "searched"))
    check("screenshot of the scrolled picker", shot_taken(png))
    layout_report("skins", log)


def scenario_stats(brain_url, token, name):
    print("scenario stats: Market & Trading > Stats (50 rows) and hover / scroll", flush=True)
    lines = ["0 boot", "1500 clickid menuBtnMarket", "2500 call v10mk.mtab 4", "4500 rects @stat .stat-card",
             "4600 cursorid v10mkGridHolder 0.5 0.5"]
    t = 4700
    h, t = hover_lines(t, "v10mkGridHolder", 16)
    lines += h
    w, t = wheel_lines(t, 25)
    lines += w
    t += 300
    h, t = hover_lines(t, "v10mkGridHolder", 12)
    lines += h
    w, t = wheel_lines(t, 20, 1)
    lines += w
    lines.append("%d rects @final .stat-card" % t)
    lines.append("%d quit" % (t + 500))
    proc, log, cmds = run_client("stats", "\n".join(lines) + "\n", brain_url, token, name)
    png = os.path.join(WORK, "stats.png")
    if wait_log(proc, log, "@stat n="):
        ui_cmd(cmds, "shot " + png)
    log = finish(proc, log, "stats")
    check("the Stats tab lists the brain's 50 rows", rects(log, "stat") == 50, "n=%d" % rects(log, "stat"))
    check("screenshot of the Stats tab", shot_taken(png))
    layout_report("stats", log)


SCENARIOS = {"skins": scenario_skins, "stats": scenario_stats}


def main():
    names = sys.argv[3:] or list(SCENARIOS)
    port = free_ports(1)[0]
    brain = Brain(port)
    if not brain.start():
        print("kn_brain did not start: skip (log %s)" % brain.log)
        sys.exit(77)
    try:
        aid, token = seed_accounts(brain)
        url = "http://127.0.0.1:%d" % port
        for n in names:
            SCENARIOS[n](url, token, "Collector")
    finally:
        brain.stop()
    if FAILS:
        print("FAILED: " + "; ".join(FAILS) + "  (work dir %s)" % WORK)
        sys.exit(1)
    print("layout freeze e2e: all passed (work dir %s)" % WORK)
    shutil.rmtree(WORK, ignore_errors=True)


if __name__ == "__main__":
    main()

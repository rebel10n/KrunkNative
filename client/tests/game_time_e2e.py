#!/usr/bin/env python3
"""The game time tile (client/src/ui/hud_ui.c hud_gametime_step, client/src/ui/game_time.c): the time this
device has spent in games, kept locally, opt-in through Settings > Client > Game > Show Game Time (default
off). Headless client_bin, offline Kanji, against a local kn_brain (sqlite, temp dir) for one signed-in run.

  python3 -I client/tests/game_time_e2e.py BUILD_DIR      (repository root)

  1. default off: a guest's game never shows the tile, and the time is still counted and stored
     (html_storage "knGameTime" = <cfg>/krunknative/knGameTime.json, written when the client exits)
  2. Show Game Time on, a guest, the stored total 8073900 ms (2h 14m 33.9s): the tile shows at once,
     reads "2h 14m <s>s" and ticks one second at a time; the ESC menu hides it and closing the menu
     brings it back; the scoreboard held open (Tab) and the 1280x720 / 844x390 boxes are clear of the
     board, the kill feed and the window; the exit stores the total the game added
  3. the same device again (the stored total): the first reading is the stored total plus this run's
     play (the count continues across a restart)
  4. the phone layout (844x390, KRUNKNATIVE_MOBILE=1), the same stored total
  5. a signed-in account whose brain says timePlayed is 999 hours: the tile reads the device's own total
     (2h 14m), not the account's; `con reload` (the offline sandbox restarts in place) keeps it up
Fixed frames (KRUNKNATIVE_FIXED_DELTA=16.667): the game clock is the summed frames, so the seconds are exact.
Prints PASS/FAIL lines; exit code 0 only when everything passed. 77 = skip (no kn_brain / client_bin / websockets).
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

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
CLIENT = os.path.join(BUILD, "client_bin")
if not (os.path.exists(BRAIN) and os.path.exists(CLIENT)):
    print("needs client_bin and kn_brain in %s: skip" % BUILD)
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

STORED_MS = 8073900          # 2h 14m 33.9s: the device's stored total for the runs that seed it
ACCOUNT_MS = 999 * 3600000   # the brain's timePlayed for the signed-in run: the tile must not show it
PASSWORD = "gametime_pass_1"
FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-game-time.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
# a worktree lacks the untracked font / asset dirs: the main checkout's assets (as hud_chat_e2e.py does)
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or next(
    (d for d in (os.path.join(ROOT, "assets"), os.path.expanduser("~/src/KrunkNative/assets"))
     if os.path.isfile(os.path.join(d, "css", "fonts", "font2.ttf"))),
    os.path.join(ROOT, "assets"))
TILE_RE = re.compile(r"^(\d+)h (\d+)m (\d+)s$")


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


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, PASSWORD, None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


def cfg_dir(tag):
    return os.path.join(WORK, tag + "-cfg", "krunknative")


def seed(tag, client_store=None, stored=None):
    """the device's files before a run: the Client tab's store and the stored total (html_storage keys)"""
    d = cfg_dir(tag)
    os.makedirs(d, exist_ok=True)
    if client_store is not None:
        with open(os.path.join(d, "knClient.json"), "w") as f:
            json.dump(client_store, f)
    if stored is not None:
        with open(os.path.join(d, "knGameTime.json"), "w") as f:
            f.write(str(stored))


def stored_total(tag):
    try:
        with open(os.path.join(cfg_dir(tag), "knGameTime.json")) as f:
            return float(f.read())
    except (OSError, ValueError):
        return None


def client_run(tag, script, token=None, extra_env=None, timeout=240):
    """one headless client_bin --offline on Kanji with the input script; returns its log lines. The
    config directory is WORK/<tag>-cfg (a run that reuses another's directory shares its stored total)."""
    if token:
        d = cfg_dir(tag)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "krunker_username.json"), "w") as f:
            f.write(token[0])
        with open(os.path.join(d, "krunker_token.json"), "w") as f:
            f.write(token[1])
    events = sorted((l for l in script if l.strip()), key=lambda l: int(l.split()[0]))
    script_path = os.path.join(WORK, tag + ".script")
    with open(script_path, "w") as f:
        f.write("\n".join(events) + "\n")
    log_path = os.path.join(WORK, tag + ".log")
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_UI_DIR", "KRUNKNATIVE_MOBILE")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, tag + "-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script_path,
                "KRUNKNATIVE_FIXED_DELTA": "16.667", "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_ASSETS_DIR": ASSETS})
    env.update(extra_env or {})
    if token:
        env["KRUNKNATIVE_BRAIN_URL"] = "http://127.0.0.1:%d" % BRAIN_PORT[0]
    with open(log_path, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT, "--offline", "assets/maps/kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, cwd=ROOT, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(False, "%s: client_bin finished its script (timeout, log %s)" % (tag, log_path))
    with open(log_path, errors="replace") as f:
        return f.read().splitlines()


BRAIN_PORT = [0]


def probe_last(lines, cmd, label):
    """the rest of the last `[input_script] <cmd> t=<ms> @<label> ...` line ("" when none)"""
    rest = ""
    for l in lines:
        m = re.match(r"\[input_script\] %s t=\d+ @%s (.*)$" % (cmd, label), l)
        if m:
            rest = m.group(1)
    return rest


def boxes_of(rest):
    """`id=x,y,w,h,ow,oh` pairs of a rect probe -> {id: [x, y, w, h] | None}"""
    out = {}
    for part in rest.split():
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = None if v == "-" else [float(x) for x in v.split(",")[:4]]
    return out


def script_samples(first_ms, last_ms, step_ms):
    """the tile's visibility and text every `step_ms` of the game (labels carry the scheduled time)"""
    lines = []
    for t in range(first_ms, last_ms + 1, step_ms):
        lines.append("%d vis @v%d gameTimeCount" % (t, t))
        lines.append("%d text @x%d [id=gameTimeVal]" % (t, t))
    return lines


def seconds_of(text):
    m = TILE_RE.match(text or "")
    if not m:
        return None
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))


def tile_rows(lines, first_ms, last_ms):
    """(scheduled t, shown ("1" / "0" / "-" / None), text) for each sampled frame"""
    vis, texts = {}, {}
    for l in lines:
        m = re.match(r"\[input_script\] vis t=\d+ @v(\d+) gameTimeCount=(\S+)", l)
        if m:
            vis[int(m.group(1))] = m.group(2)
        m = re.match(r"\[input_script\] text t=\d+ @x(\d+) n=\d+ checked=\d+ value=\S* text=(.*)$", l)
        if m:
            texts[int(m.group(1))] = m.group(2)
    return sorted((t, v, texts.get(t)) for t, v in vis.items() if first_ms <= t <= last_ms)


def overlap(a, b):
    return a is not None and b is not None and a[0] < b[0] + b[2] and b[0] < a[0] + a[2] and a[1] < b[1] + b[3] and b[1] < a[1] + a[3]


def layout_check(name, boxes, window):
    tile, board, feed = boxes.get("gameTimeCount"), boxes.get("leaderboardHolder"), boxes.get("killFeed")
    check(tile is not None, "%s: the tile has a box (%s)" % (name, tile))
    if tile is None:
        return
    check(tile[0] >= 0 and tile[1] >= 0 and tile[0] + tile[2] <= window[0] and tile[1] + tile[3] <= window[1],
          "%s: inside the %dx%d window (%s)" % (name, window[0], window[1], tile))
    check(not overlap(tile, board), "%s: clear of the leaderboard (%s / %s)" % (name, tile, board))
    check(not overlap(tile, feed), "%s: clear of the kill feed (%s / %s)" % (name, tile, feed))
    board_open = boxes.get("centerLeaderDisplay")
    if board_open is not None:
        check(not overlap(tile, board_open), "%s: clear of the scoreboard held open (%s / %s)" % (name, tile, board_open))


def ticking(name, rows, first_t=None):
    """the readings of the shown samples rise one second at a time with the game's clock"""
    shown = [(t, txt) for t, v, txt in rows if v == "1" and txt]
    check(len(shown) >= 10, "%s: the tile shows in the game (%d samples)" % (name, len(shown)))
    if len(shown) < 2:
        return None
    first_t, first_txt = shown[0]
    last_t, last_txt = shown[-1]
    gain = seconds_of(last_txt) - seconds_of(first_txt)
    game_secs = (last_t - first_t) / 1000.0
    check(abs(gain - game_secs) <= 1.0 and gain > 0,
          "%s: it ticks with the game (%.1f s of game, the reading rose by %d s)" % (name, game_secs, gain))
    steps = [seconds_of(b) - seconds_of(a) for (_, a), (_, b) in zip(shown, shown[1:])]
    check(steps and all(s in (0, 1) for s in steps), "%s: one second at a time (steps %s)" % (name, sorted(set(steps))))
    return shown


def desktop_run(tag, token=None, reuse_stored=False):
    """a game with the tile on: the reading, the menu, the Tab scoreboard, the boxes; the exit stores the total"""
    step = 250
    script = ["0 play"] + script_samples(100, 6000, step)
    script += ["6100 down ESC", "6150 up ESC"] + script_samples(6200, 6400, 100)  # the menu over the game
    script += ["6500 down ESC", "6550 up ESC"] + script_samples(6700, 8000, 100)
    script += ["8050 down TAB", "8070 rect @r3 gameTimeCount leaderboardHolder killFeed centerLeaderDisplay", "8090 up TAB"]
    script += ["8100 rect @r1 gameTimeCount leaderboardHolder killFeed", "8200 quit"]
    lines = client_run(tag, script, token)
    rows = tile_rows(lines, 100, 8000)
    shown = ticking(tag, [r for r in rows if r[0] <= 6000])
    if shown:
        first_sec = seconds_of(shown[0][1])
        check(first_sec is not None and 8073 <= first_sec <= 8075,
              "%s: the first reading is the stored total plus the game's count (%r at %d ms)" % (tag, shown[0][1], shown[0][0]))
        check(all(seconds_of(txt) is not None and txt.startswith("2h 14m ") for _, txt in shown),
              "%s: every reading is the v7.1.4 readable format (2h 14m Ns)" % tag)
    menu = [v for t, v, _ in rows if 6100 <= t <= 6400]
    check(menu and all(v == "0" for v in menu), "%s: hidden over the game's menu (%s)" % (tag, menu))
    back = [v for t, v, _ in rows if 6700 <= t <= 6900]
    check(back and all(v == "1" for v in back), "%s: shown again once the menu closes (%s)" % (tag, back))
    board = boxes_of(probe_last(lines, "rect", "r3"))
    check(board.get("centerLeaderDisplay") is not None, "%s: the scoreboard is open (Tab) for the overlap check" % tag)
    layout_check(tag, boxes_of(probe_last(lines, "rect", "r1")), (1280, 720))
    if board.get("centerLeaderDisplay") is not None:
        check(not overlap(board.get("gameTimeCount"), board["centerLeaderDisplay"]), "%s: clear of the Tab scoreboard" % tag)
    return lines


def phone_run(tag):
    script = ["0 play"] + script_samples(100, 1500, 250)
    script += ["1600 down TAB", "1620 rect @r2 gameTimeCount leaderboardHolder killFeed centerLeaderDisplay", "1650 up TAB",
               "1800 quit"]
    lines = client_run(tag, script, None, {"KRUNKNATIVE_MOBILE": "1", "KRUNKNATIVE_WINDOW_SIZE": "844x390"})
    rows = tile_rows(lines, 100, 1500)
    check(any(v == "1" for _, v, _ in rows), "%s: the tile is up in the game (%s)" % (tag, [v for _, v, _ in rows]))
    layout_check(tag, boxes_of(probe_last(lines, "rect", "r2")), (844, 390))


def off_run():
    """default: the tile hidden, the time counted and stored"""
    seed("off", client_store=None, stored=None)  # no Client store: Show Game Time is off
    script = ["0 play"] + script_samples(100, 4000, 250) + ["4100 quit"]
    lines = client_run("off", script)
    rows = tile_rows(lines, 100, 4000)
    sampled = [v for _, v, _ in rows if v is not None]
    check(sampled and all(v == "0" for v in sampled), "default off: the tile never shows in a game (%d samples)" % len(sampled))
    total = stored_total("off")
    check(total is not None and total >= 3000, "default off: the play is still counted and stored (%s ms)" % total)


def reload_run(tag, token):
    """`con reload` (the offline sandbox restarts in place): the tile stays up, the count goes on"""
    script = ["0 play"] + script_samples(100, 3000, 250) + ["3100 con reload"] + script_samples(3150, 6000, 250) + ["6100 quit"]
    lines = client_run(tag, script, token)
    rows = tile_rows(lines, 100, 6000)
    before = [txt for t, v, txt in rows if t <= 3000 and v == "1" and txt]
    after_rows = [(t, v, txt) for t, v, txt in rows if t > 3100]
    check(before, "%s: the tile shows before the reload (%d readings)" % (tag, len(before)))
    check(after_rows and all(v == "1" for _, v, _ in after_rows),
          "%s: the tile stays up through the in-place restart (%d samples after)" % (tag, len(after_rows)))
    after = [txt for _, _, txt in after_rows if txt]
    if before and after:
        check(seconds_of(after[0]) >= seconds_of(before[-1]),
              "%s: the count goes on (%s before, %s after)" % (tag, before[-1], after[0]))


def main():
    (port,) = free_ports(1)
    BRAIN_PORT[0] = port
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")  # its own config: never the developer's config/config.json
    with open(cfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False,
                             "internal_secret": "gametime-e2e-%d" % os.getpid()},
                   "accounts": {"signup_rewards": {"enabled": False}, "sale_grace_s": 0, "free_premium": False, "starting_kr": 0,
                                "login_throttle_s": 0, "password_iterations": 1000,
                                "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                             start_new_session=True)
    try:
        for _ in range(200):
            try:
                with open(os.path.join(WORK, "brain.log")) as f:
                    if re.search(r"listening on http://127\.0\.0\.1:%d" % port, f.read()):
                        break
            except OSError:
                pass
            time.sleep(0.1)
        name = "gametime%d" % (int(time.time()) % 100000)
        aid, token = asyncio.run(register(port, name))
        q("UPDATE players SET player_timeplayed = ? WHERE player_id = ?", ACCOUNT_MS, aid)

        print("default off:", flush=True)
        off_run()

        print("Show Game Time on, a guest (desktop 1280x720):", flush=True)
        seed("guest", client_store={"gameTime": True}, stored=STORED_MS)
        desktop_run("guest")
        total = stored_total("guest")
        check(total is not None and total >= STORED_MS + 5000,
              "guest: the exit stores the total the game added (%s ms, was %d)" % (total, STORED_MS))

        print("the same device, a new run (the stored total carries over):", flush=True)
        before = total
        script = ["0 play"] + script_samples(100, 2000, 250) + ["2100 quit"]
        lines = client_run("guest", script)  # the same config directory
        rows = tile_rows(lines, 100, 2000)
        shown = [txt for _, v, txt in rows if v == "1" and txt]
        check(shown and seconds_of(shown[0]) * 1000 >= before - 1000,
              "carry over: the first reading starts from the stored total (%s, stored %d ms)" % (shown[:1], before))

        print("phone layout (844x390):", flush=True)
        seed("phone", client_store={"gameTime": True}, stored=STORED_MS)
        phone_run("phone")

        print("signed in, the brain's timePlayed 999 h (not shown):", flush=True)
        seed("account", client_store={"gameTime": True}, stored=STORED_MS)
        script = ["0 play"] + script_samples(100, 2000, 250) + ["2100 quit"]
        lines = client_run("account", script, (name, token))
        rows = tile_rows(lines, 100, 2000)
        shown = [txt for _, v, txt in rows if v == "1" and txt]
        check(shown and shown[0].startswith("2h 14m "), "signed in: the tile reads the device's own total (%s)" % (shown[:1],))
        reload_run("account", (name, token))  # the same device: the stored total goes on through the reload
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
    print("game time tile e2e: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

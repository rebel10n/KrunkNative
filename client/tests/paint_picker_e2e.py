#!/usr/bin/env python3
"""The owned card's paint picker end to end (client/src/ui/paint_picker.c, ui_loadout.c owned_card) against a
scratch kn_brain (sqlite, temp dir) and headless client_bin on the v10 page, online with a token login:

  apply     the Hat chooser (window 16) lists the account's Bullseye Halo (an unpainted unit, a stack); its card
            has the paint button; the popup lists the owned Red Paint (x2); Paint applies it: POST
            /items/7882/paint {item_id, paint_item_id}, the brain makes a copy with the paint (skin_copies), one paint
            unit is taken, the popup closes, the toast says "Painted Bullseye Halo with Red Paint", and the card's
            badge reads "Painted: Red Paint"
  empty     the account owns the hat but no paint: the popup shows v10's empty state, no error, no Paint button
  refused   the brain goes away while the popup's Paint is pressed: the popup says the paint failed

  python3 -I client/tests/paint_picker_e2e.py BUILD_DIR REPO_ROOT [scenario ...]
Headless client_bin (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs) driven by a timed input
script (KRUNKNATIVE_INPUT_SCRIPT: call, attr, rects, grep, clicksel, shot). No production: krunker.io, its
matchmaker and finder point at a closed local port. PAINT_PICKER_SHOTS=<dir> keeps the screenshots there.
77 = skip (no websockets, no kn_brain / client_bin).
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
WORK = tempfile.mkdtemp(prefix="kn-paint-picker.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
HAT = 7882   # Bullseye Halo: a hat (type 1) with paintTargets, the Hat chooser's card
PAINT = 7776  # Red Paint: a type-18 store paint (paintColor #dc3c3c)
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


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, "junk_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


class Brain:
    """kn_brain on a scratch database: start, stop (the brain goes away)."""

    def __init__(self, port):
        self.port = port
        self.log = os.path.join(WORK, "brain.log")
        self.proc = None

    def start(self):
        cfg = os.path.join(WORK, "brain.json")  # its own config: never the developer's config/config.json
        with open(cfg, "w") as f:
            json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                       "brain": {"host": "127.0.0.1", "port": self.port, "federation": False,
                                 "internal_secret": "paint-picker-e2e-%d" % os.getpid()},
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


def new_account(brain, name, paints):
    """Registers `name` and seeds its owned list: one unpainted Bullseye Halo (a stack unit) and `paints` Red Paint."""
    aid, token = asyncio.run(register(brain.port, name))
    q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", aid, HAT)
    if paints:
        q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, ?)", aid, PAINT, paints)
    q("UPDATE players SET player_score = 2000000, player_premium = 0, player_funds = 5000 WHERE player_id = ?", aid)
    return aid, token


def run_client(tag, script, brain_url, token, name):
    """Starts one headless client_bin run of `script`, logged in as `name`; returns (proc, log, cmds): cmds is the
    KRUNKNATIVE_UI_CMDS file (a `shot <png>` line there writes the composited page, which the script's shot cannot)."""
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
                "KRUNKNATIVE_BRAIN_URL": brain_url})
    # no production: krunker.io, its matchmaker and finder point at a closed local port (connection refused)
    for k in ("KRUNKNATIVE_KRUNKER_URL", "KRUNKNATIVE_KRUNKER_MATCHMAKER", "KRUNKNATIVE_FINDER_URL", "KRUNKNATIVE_KRUNKER_API",
              "KRUNKNATIVE_KRUNKER_AUTH", "KRUNKNATIVE_KRUNKER_SOCIAL", "KRUNKNATIVE_KRUNKER_EVENT_BUS", "KRUNKNATIVE_KRUNKER_DATA_API"):
        env[k] = "http://127.0.0.1:9"
    with open(log, "w") as out:
        proc = subprocess.Popen([CLIENT, "--mode", "online", "--brain", brain_url, "--finder", "off"], cwd=ROOT, stdout=out,
                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
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


def finish(proc, log, tag, timeout=120):
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        check("%s: client_bin finished its script" % tag, False, "timeout, log %s" % log)
    with open(log, errors="replace") as f:
        return f.read()


def run(tag, script, brain_url, token, name):
    proc, log, _ = run_client(tag, script, brain_url, token, name)
    return finish(proc, log, tag)


def rects(log, label):
    m = re.search(r"\[input_script\] rects t=\d+ @%s n=(\d+)" % re.escape(label), log)
    return int(m.group(1)) if m else -1


def attr(log, label):
    m = re.search(r"\[input_script\] attr t=\d+ @%s n=\d+ [^=\s]+=(.*)" % re.escape(label), log)
    return m.group(1).strip() if m else "-"


def hits(log, label):
    m = re.search(r"\[input_script\] grep t=\d+ @%s needle=\S+ n=\d+ hits=(\d+)" % re.escape(label), log)
    return int(m.group(1)) if m else -1


def shot_taken(path):
    """The screenshot is on disk; PAINT_PICKER_SHOTS=<dir> keeps a copy there."""
    dest_dir = os.environ.get("PAINT_PICKER_SHOTS")
    if os.path.exists(path) and dest_dir:
        os.makedirs(dest_dir, exist_ok=True)
        shutil.copy(path, os.path.join(dest_dir, os.path.basename(path)))
    return os.path.exists(path)


def scenario_apply(brain, name, token, brain_url):
    print("scenario apply: the owned Bullseye Halo, Red Paint x2, Paint applies it", flush=True)
    script = """0 boot
1500 call showWindow 16
2500 rects @cards .skinCard[data-index="%d"]
2600 attr @btn onclick .selctPaintBtn
2700 clicksel .selctPaintBtn
3400 rects @popup .paint-container
3450 rects @swatch .swatch
3500 attr @picked aria-checked .swatch
3600 grep @label Red .btn-apply
4200 clicksel .btn-apply
5000 rects @popupgone .paint-container
5100 attr @badge title .skinPaintBadge
5150 grep @toast Painted body
6500 quit
""" % HAT
    proc, log, cmds = run_client("apply", script, brain_url, token, name)
    picker_png, badge_png = os.path.join(WORK, "picker.png"), os.path.join(WORK, "badge.png")
    if wait_log(proc, log, "@popup n=1"):  # the popup is up: the page's picture, before Paint
        ui_cmd(cmds, "shot " + picker_png)
    if wait_log(proc, log, "@badge n=1 title=Painted"):  # the badge is on the card after the apply
        ui_cmd(cmds, "shot " + badge_png)
    log = finish(proc, log, "apply")
    check("the Hat chooser lists the owned Bullseye Halo (its card)", rects(log, "cards") == 1, "n=%d" % rects(log, "cards"))
    check("the card has the paint button (paintOpen: deob:150226, paintTargets)", "paintOpen(%d,%d)" % (HAT, HAT) in attr(log, "btn"),
          "onclick=%s" % attr(log, "btn"))
    check("the popup opens over the chooser", rects(log, "popup") == 1, "n=%d" % rects(log, "popup"))
    check("the popup lists the owned Red Paint (one swatch, picked)", rects(log, "swatch") == 1 and attr(log, "picked") == "true",
          "swatches=%d picked=%s" % (rects(log, "swatch"), attr(log, "picked")))
    check("Paint is labelled with the paint's name (Paint (Red Paint))", hits(log, "label") >= 1, "hits=%d" % hits(log, "label"))
    check("screenshot of the popup (the page, the popup open)", shot_taken(picker_png))
    check("Paint closes the popup on success", rects(log, "popupgone") == 0, "n=%d" % rects(log, "popupgone"))
    check("the toast says Painted Bullseye Halo with Red Paint", hits(log, "toast") >= 1, "Painted hits=%d" % hits(log, "toast"))
    check("the card's badge reads Painted: Red Paint", attr(log, "badge") == "Painted: Red Paint", "title=%s" % attr(log, "badge"))
    check("screenshot of the painted badge (the page, the card with its badge)", shot_taken(badge_png))
    copies = q("SELECT copy_id, paint_color FROM skin_copies WHERE account_id = (SELECT id FROM accounts WHERE username = ?) AND skin_id = ?",
               name, HAT)
    check("the brain made one copy of the hat with the paint (skin_copies, #dc3c3c)",
          len(copies) == 1 and copies[0][1] == "#dc3c3c" and copies[0][0] >= 1000000, "rows=%r" % copies)
    left = q("SELECT count FROM inventory WHERE account_id = (SELECT id FROM accounts WHERE username = ?) AND skin_id = ?", name, PAINT)
    check("one Red Paint was taken (2 -> 1)", left and left[0][0] == 1, "count=%r" % left)
    # inventory.count counts every owned unit, painted or not; the unpainted ones are count - painted copies (doc/v10/paint-copy.md)
    total = q("SELECT count FROM inventory WHERE account_id = (SELECT id FROM accounts WHERE username = ?) AND skin_id = ?", name, HAT)
    total = total[0][0] if total else 0
    check("the hat's one unit is the painted copy (no unpainted unit left: total 1 = 1 copy)", total == 1 and len(copies) == 1,
          "total=%r copies=%d" % (total, len(copies)))


def scenario_empty(brain, name, token, brain_url):
    print("scenario empty: the hat is paintable, no paint owned: v10's empty state, no error", flush=True)
    script = """0 boot
1500 call showWindow 16
2500 clicksel .selctPaintBtn
3400 rects @popup .paint-container
3450 rects @empty .paint-empty
3500 rects @error .paint-error
3550 rects @applybtn .btn-apply
3600 quit
"""
    log = run("empty", script, brain_url, token, name)
    check("the empty popup opens", rects(log, "popup") == 1, "n=%d" % rects(log, "popup"))
    check("v10's empty state is shown (You don’t own any paints yet ...)", rects(log, "empty") == 1, "n=%d" % rects(log, "empty"))
    check("no error and no Paint button without a paint", rects(log, "error") == 0 and rects(log, "applybtn") == 0,
          "error=%d apply=%d" % (rects(log, "error"), rects(log, "applybtn")))


def scenario_refused(brain, name, token, brain_url):
    print("scenario refused: the brain refuses the Paint (the paint is gone from the account): the popup says why", flush=True)
    script = """0 boot
1500 call showWindow 16
2500 clicksel .selctPaintBtn
3400 rects @popup .paint-container
3450 rects @swatch .swatch
4000 rects @wait .btn-apply
6000 clicksel .btn-apply
7000 rects @error .paint-error
7050 grep @errtext Paint .paint-error
7100 quit
"""
    proc, log, _ = run_client("refused", script, brain_url, token, name)
    # the popup is open once its swatch is on the log; then the paint goes from the account, so the brain refuses it
    opened = wait_log(proc, log, "@swatch n=1")
    if opened:
        uid = q("SELECT id FROM accounts WHERE username = ?", name)[0][0]
        q("DELETE FROM inventory WHERE account_id = ? AND skin_id = ?", uid, PAINT)
    text = finish(proc, log, "refused")
    check("the popup is open before Paint", opened and rects(text, "popup") == 1 and rects(text, "swatch") == 1,
          "popup=%d swatch=%d" % (rects(text, "popup"), rects(text, "swatch")))
    check("a refused Paint shows the reason in the popup (no silent close)", rects(text, "error") == 1 and rects(text, "popup") == 1,
          "error=%d" % rects(text, "error"))
    check("the popup says why (the error text is not empty)", hits(text, "errtext") >= 1, "hits=%d" % hits(text, "errtext"))


def main():
    names = sys.argv[3:] or ["apply", "empty", "refused"]
    (port,) = free_ports(1)
    brain = Brain(port)
    brain_url = "http://127.0.0.1:%d" % port
    stamp = str(int(time.time()) % 100000)
    try:
        assert brain.start(), "kn_brain did not come up"
        for n in names:
            paints = 0 if n == "empty" else 2
            account = "paint%s%s" % (n[:2], stamp)
            aid, token = new_account(brain, account, paints)
            if n == "apply":
                scenario_apply(brain, account, token, brain_url)
            elif n == "empty":
                scenario_empty(brain, account, token, brain_url)
            elif n == "refused":
                scenario_refused(brain, account, token, brain_url)
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check("exception: %r" % e, False)
    finally:
        brain.stop()
    print("paint picker e2e: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


if __name__ == "__main__":
    main()

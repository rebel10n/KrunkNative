#!/usr/bin/env python3
"""Spectate bot overlay e2e (module bots-spectate-overlay): NET_EXT_KN_BOT_PATH drawn by
client/src/nav_overlay.c and the spectator HUD's state line (client/src/ui/hud_spectate.c).

  python3 -I server/tests/bot_overlay_e2e.py BUILD_DIR      (run from the repository root)

A headless client_bin --sandbox (the in-process server runs the bots and the spectator list) on
Sandstorm enters as a spectator, adds two max-difficulty bots, follows one (right click = next
player) in third person (R: the route leaves the bot's feet in view) and turns show_navmeshes on,
then off. Checks:
  1. the server sent bot paths for the followed bot (the client logs the first one)
  2. with the overlay on: the HUD line #knSpecBot is shown and names the bot's state, and the
     frames hold bright magenta pixels (the route polyline / jump arcs: #ff00ff / #ff40ff; the
     nav mesh's red and blue lines only blend into dark purples, which do not count; the chat's
     left strip is excluded)
  3. with the overlay off: the HUD line is hidden and the frames hold no magenta (frame diff)
Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device
(KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it). Leaves nothing running: the
client is killed on a timeout; the work directory is removed on success.
"""
import glob
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kn_png import read_rgba  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-bot-overlay-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
WORLD = (430, 0, 1280, 720)  # the frame without the left strip (chat names and player rows)
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if detail else ''}", flush=True)


def probe(lines, kind, label):
    for line in lines:
        m = re.search(r"\[input_script\] " + kind + r" t=\d+ @" + label + r" (.*)$", line)
        if m:
            return m.group(1)
    return None


def frames(prefix):
    return sorted(glob.glob(os.path.join(WORK, prefix + "_*.png")))


def route_pixels(path, box):
    """bright magenta pixels (the route layers' #ff00ff / #ff40ff, also lit or fogged a little) in the
    box; -1 when the file cannot be read"""
    try:
        w, h, ch, rows = read_rgba(path)
    except (OSError, KeyError, ValueError):
        return -1
    x0, y0, x1, y1 = box
    hits = 0
    for y in range(max(y0, 0), min(y1, h)):
        row = rows[y]
        for x in range(max(x0, 0), min(x1, w)):
            r, g, b = row[x * ch], row[x * ch + 1], row[x * ch + 2]
            if r > 170 and b > 170 and g < 110 and abs(r - b) < 60:
                hits += 1
    return hits


def main():
    cfg = os.path.join(WORK, "cfg", "krunknative")
    os.makedirs(cfg)
    script = os.path.join(WORK, "overlay.script")
    with open(script, "w") as f:
        f.write("0 menu\n"
                "300 call toggleSpect 1\n"
                "600 play\n"
                "2500 con bot_add 2 difficulty=max randomCosmetics=false\n"  # no store skins: nothing else magenta
                "4500 mdown right\n"            # follow the next player: the first bot
                "4530 mup right\n"
                "5000 con show_navmeshes\n"
                "6000 down R\n"                # third person: the route leaves the bot's feet in view
                "6050 up R\n"
                "7500 players\n"
                "7500 cam @on\n"
                "7500 vis @on knSpecBot spectateUI\n"
                "7500 text @on [id=\"knSpecBot\"]\n"   # a # would start a script comment
                f"7500 frames {WORK}/on 3\n"
                "7700 con show_navmeshes off\n"
                "8300 vis @off knSpecBot\n"
                f"8300 frames {WORK}/off 3\n"
                "8700 quit\n")
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"), "KRUNKNATIVE_NO_BROWSER": "1",
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--sandbox", "--map", "sandstorm_v3"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=240)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        lines = f.read().splitlines()

    cam = probe(lines, "cam", "on") or ""
    check("spectating (cam probe)", "spect=1" in cam, cam[:160])
    players = [l for l in lines if "[input_script] players t=" in l]
    check("two bots in the session", players and re.search(r" n=([2-9]|\d\d)", players[0]) is not None, players[:1])
    first = [l for l in lines if "[nav] bot path:" in l]
    check("the server sent the followed bot's path (NET_EXT_KN_BOT_PATH)", first, first[:1])
    built = [l for l in lines if "[nav] overlay:" in l]
    check("the nav overlay was built", built, built[:1])

    vis_on = probe(lines, "vis", "on") or ""
    text_on = probe(lines, "text", "on") or ""
    state = re.search(r"text=BOT (Idle|Hunt|Attack|Hide|\S+)", text_on)
    check("overlay on: the HUD state line is shown", "knSpecBot=1" in vis_on and "spectateUI=1" in vis_on, vis_on)
    check("overlay on: the HUD line names the bot's state", state is not None and state.group(1) in ("Idle", "Hunt", "Attack", "Hide"),
          text_on[:200])
    on = frames("on")
    on_counts = [route_pixels(s, WORLD) for s in on]
    check("overlay on: the route is drawn (bright magenta pixels in the frames)", on and min(on_counts) > 60, f"route px per frame {on_counts}")

    vis_off = probe(lines, "vis", "off") or ""
    off = frames("off")
    off_counts = [route_pixels(s, WORLD) for s in off]
    check("overlay off: the HUD state line is hidden", "knSpecBot=0" in vis_off, vis_off)
    # a few stray purples (a lit edge, a skin) are not a route: the on frames carry hundreds
    check("overlay off: nothing drawn (frame diff: no magenta)", off and max(off_counts) <= 10, f"route px per frame {off_counts}")

    if all(results) and results:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs and frames kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if results and all(results) else 1)


if __name__ == "__main__":
    main()

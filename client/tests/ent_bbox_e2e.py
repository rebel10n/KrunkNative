#!/usr/bin/env python3
"""The developer console's ent_bbox (Krunker v7.1.4 mod/devConsole.ts:1392) in a headless client: the movement
boxes drawn over the world by client/src/ent_bbox.c (geometry: client/src/ent_bbox_geom.c, ent_bbox_test).

  python3 -I client/tests/ent_bbox_e2e.py BUILD_DIR      (run from the repository root)

A headless client_bin --sandbox on Sandstorm spawns, runs `ent_bbox` from the console, saves frames, runs
`ent_bbox off` and saves frames again. Checks:
  1. the console answers "ent_bbox: on (N player boxes, M object boxes)" with M > 0 (the map's colliders),
     then "ent_bbox: off"
  2. with the overlay on, the frames hold the solid objects' magenta lines (#ff00ff, depth test and fog off,
     so the colour is exact; the chat's left strip is excluded)
  3. with it off, none (frame diff)
  4. in third person (`thirdperson`, granted in the offline sandbox) `ent_bbox on` counts the own box, and its
     green lines (#00ff00) are drawn; in first person (1, the camera inside the box) there were none
Prints PASS/FAIL lines; exit code 0 only when everything passed, 77 without client_bin. Needs a headless GL
device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it). The client is killed on a timeout;
the work directory is removed on success.
"""
import glob
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.getcwd(), "server", "tests"))
from kn_png import read_rgba  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
# the full asset tree (a worktree has no models / fonts / css): the checkout's own, else the main one
ROOT = os.getcwd()
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or next(
    (d for d in (os.path.join(ROOT, "assets"), os.path.expanduser("~/src/KrunkNative/assets")) if os.path.isdir(os.path.join(d, "models"))),
    os.path.join(ROOT, "assets"))
WORLD = (430, 0, 1280, 720)  # the frame without the left strip (chat and player rows)
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if detail else ''}", flush=True)


def hue_count(path, box, want):
    """pixels of the layer colour `want` ("magenta" #ff00ff or "green" #00ff00) in the box (-1 when the file cannot
    be read). By hue, not level: the whole frame's brightness moves (the spawn fade), so a line reads #cf07cf in
    one frame and #f502f5 in the next"""
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
            if want == "magenta" and r > 80 and b > 80 and abs(r - b) * 8 < r and g * 6 < r:
                hits += 1
            elif want == "green" and g > 80 and r * 6 < g and b * 6 < g:
                hits += 1
    return hits


def magenta(path, box):
    return hue_count(path, box, "magenta")


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print(f"ent_bbox_e2e: no client_bin in {BUILD}: skipped")
        return 77
    if not os.path.isdir(os.path.join(ASSETS, "css")) or not os.path.isdir(os.path.join(ASSETS, "models")):
        print(f"ent_bbox_e2e: no game content in {ASSETS} (set KRUNKNATIVE_ASSETS_DIR): skipped")
        return 77
    work = tempfile.mkdtemp(prefix="kn-ent-bbox-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
    cfg = os.path.join(work, "cfg", "krunknative")
    os.makedirs(cfg)
    script = os.path.join(work, "ent_bbox.script")
    with open(script, "w") as f:
        f.write("0 play\n"                       # the clock starts once the local player is in
                "500 con ent_bbox\n"
                f"1000 frames {work}/on 3\n"
                # the 3 frames take ~150 ms each on llvmpipe and start late under load: off well after them
                "3000 con ent_bbox off\n"
                f"3600 frames {work}/off 3\n"
                "4600 con thirdperson\n"         # offline: granted at once (third_person.c)
                "5600 con ent_bbox on\n"
                f"6200 frames {work}/third 3\n"
                "7800 quit\n")
    log = os.path.join(work, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_DEBUG": "2", "XDG_CONFIG_HOME": os.path.join(work, "cfg"),
                "KRUNKNATIVE_NO_BROWSER": "1", "XDG_STATE_HOME": os.path.join(work, "state"), "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_ASSET_FETCH": "0", "KRUNKNATIVE_ASSETS_DIR": ASSETS})  # the content as it is: nothing fetched or written
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--sandbox", "--map", "sandstorm_v3"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=480)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        text = f.read()

    on_line = re.search(r"ent_bbox: on \((\d+) player boxes, (\d+) object boxes\)", text)
    check("1: the console answers ent_bbox: on with the map's object boxes", on_line and int(on_line.group(2)) > 0,
          on_line.group(0) if on_line else "no answer line")
    check("1: and ent_bbox: off", "ent_bbox: off" in text)

    on = sorted(glob.glob(os.path.join(work, "on_*.png")))
    off = sorted(glob.glob(os.path.join(work, "off_*.png")))
    on_counts = [magenta(s, WORLD) for s in on]
    off_counts = [magenta(s, WORLD) for s in off]
    check("2: overlay on: the solid objects' magenta lines are drawn", on and min(on_counts) > 200, f"px per frame {on_counts}")
    check("3: overlay off: no magenta (frame diff)", off and max(off_counts) <= 5, f"px per frame {off_counts}")

    answers = re.findall(r"ent_bbox: on \((\d+) player boxes, (\d+) object boxes\)", text)
    check("4: first person: no own box counted", answers and int(answers[0][0]) == 0, answers)
    check("4: third person: the own box counted", len(answers) >= 2 and int(answers[1][0]) == 1, answers)
    third = sorted(glob.glob(os.path.join(work, "third_*.png")))
    first_green = [hue_count(s, WORLD, "green") for s in on]
    third_green = [hue_count(s, WORLD, "green") for s in third]
    check("4: third person: the own box's green lines are drawn (none in first person)",
          third and min(third_green) > 100 and on and max(first_green) <= 5, f"px per frame first {first_green}, third {third_green}")

    if results and all(results):
        shutil.rmtree(work, ignore_errors=True)
    else:
        print(f"logs and frames kept in {work}")
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

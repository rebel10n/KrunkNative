#!/usr/bin/env python3
"""Hit marker vs death regression test (client/src/ui/hud_ui.c hud_ui_hit / client_hud_html_tick).

  python3 -I server/tests/hitmarker_death_e2e.py BUILD_DIR      (run from the repository root)

The hit marker (#knHitmarker) fades only while the player is alive (hud_hit_step); a marker shown
just before a death, or by a hit that lands after it (the kill shot's HIT behind the KILL, a
projectile), stayed on the death screen. A headless client_bin --sandbox (offline) with the marker
coloured magenta and slowed down (hitmFad), so the fade cannot hide a regression, takes scripted
NET_PACKET_HITs through the client's event handler (input script `nethit`):
  1. alive: a hit shows the marker (visible, magenta pixels at the centre)
  2. `kill` right after it: dead, no marker (not visible, no magenta pixel in the frames)
  3. a hit while dead: still no marker, also a second later
  4. respawned: a hit shows the marker again
  5. the end screen (`end`): a hit that lands during the end animation or on the results (the player
     is still active then) shows no marker: v7's showEndScreen -> hideUI hides the overlay canvas the
     marker is drawn on; here #inGameUI, which holds #knHitmarker, is down for the whole end screen
     (on the results only magenta the hit adds counts: the guest signup banner has its own)
Prints PASS/FAIL lines; exit code 0 only when everything passed. Needs a headless GL device
(KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
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
from kn_png import is_magenta, magenta_count, read_rgba  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-hitmarker-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
CENTRE = (440, 160, 840, 560)  # the marker's arms around the 1280x720 crosshair, with room to grow
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def vis(lines, label):
    for line in lines:
        m = re.search(r"\[input_script\] vis t=\d+ @" + label + r" knHitmarker=(\S+)", line)
        if m:
            return m.group(1)
    return None


def frames(prefix):
    return sorted(glob.glob(os.path.join(WORK, prefix + "_*.png")))


def magenta_new(path, base, box):
    """magenta pixels in the box of `path` that are not magenta in `base` (the same view before the
    hit): the results page has magenta of its own (the guest signup banner's purple card sits in the
    box), only what the hit adds counts; -1 when a file cannot be read"""
    try:
        w, h, ch, rows = read_rgba(path)
        bw, bh, bch, brows = read_rgba(base)
    except (OSError, KeyError, ValueError):
        return -1
    if (w, h) != (bw, bh):
        return -1
    x0, y0, x1, y1 = box
    hits = 0
    for y in range(max(y0, 0), min(y1, h)):
        row, brow = rows[y], brows[y]
        for x in range(max(x0, 0), min(x1, w)):
            if is_magenta(row[x * ch], row[x * ch + 1], row[x * ch + 2]) and \
                    not is_magenta(brow[x * bch], brow[x * bch + 1], brow[x * bch + 2]):
                hits += 1
    return hits


def main():
    cfg = os.path.join(WORK, "cfg", "krunknative")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "knSettings.json"), "w") as f:
        f.write('{"hitmCol":"#ff00ff","hitmKCol":"#ff00ff","hitmFad":0.0002}')
    script = os.path.join(WORK, "hit.script")
    with open(script, "w") as f:
        f.write(f"0 play\n1000 nethit 0\n1016 vis @alive knHitmarker\n1016 frames {WORK}/alive 1\n"
                f"1032 con kill\n1300 vis @dead knHitmarker\n1300 frames {WORK}/dead 3\n"
                f"2000 nethit 1\n2032 vis @deadhit knHitmarker\n2032 frames {WORK}/deadhit 3\n"
                f"3000 vis @dead2 knHitmarker\n3000 frames {WORK}/later 1\n"
                f"3100 respawn\n9000 state\n9000 nethit 0\n9016 vis @again knHitmarker\n9016 frames {WORK}/again 1\n"
                f"9500 con end\n13500 nethit 1\n13516 vis @endanim knHitmarker endUI\n13516 frames {WORK}/endanim 3\n"
                f"20000 vis @end endUI\n20000 state\n20050 frames {WORK}/endpre 1\n20100 nethit 1\n20116 vis @endhit knHitmarker\n"
                f"20116 frames {WORK}/endhit 3\n21000 vis @endlater knHitmarker\n21100 quit\n")
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"), "KRUNKNATIVE_NO_BROWSER": "1",
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--sandbox"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=180)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        lines = f.read().splitlines()

    alive = frames("alive")
    count = magenta_count(alive[0], CENTRE) if alive else -1
    check("alive: a hit shows the marker", vis(lines, "alive") == "1" and count > 30, f"visible {vis(lines, 'alive')}, magenta px {count}")
    check("the player died (`kill`)", any("/kill" in l for l in lines))
    for label, prefix in (("dead", "dead"), ("deadhit", "deadhit"), ("dead2", "later")):
        shots = frames(prefix)
        counts = [magenta_count(s, CENTRE) for s in shots] # the chat's names are magenta too
        what = {"dead": "right after the death", "deadhit": "a hit that lands while dead",
                "dead2": "a second later"}[label]
        check(f"dead: no marker ({what})", vis(lines, label) == "0" and shots and all(c == 0 for c in counts),
              f"visible {vis(lines, label)}, magenta px per frame {counts}")
    st = [l for l in lines if "[input_script] state t=9" in l]
    again = frames("again")
    count = magenta_count(again[0], CENTRE) if again else -1
    check("respawned: a hit shows the marker again", st and "menu=0" in st[0] and vis(lines, "again") == "1" and count > 30,
          f"visible {vis(lines, 'again')}, magenta px {count}, {st[:1]}")

    ended = [l for l in lines if "[input_script] state t=20" in l]
    check("the end screen is up (`end`)", any(re.search(r"@end endUI=1", l) for l in lines) and ended and "ended=1" in ended[0],
          f"{ended[:1]}")
    endpre = frames("endpre")
    for label, prefix, what in (("endanim", "endanim", "during the end animation"), ("endhit", "endhit", "on the results")):
        shots = frames(prefix)
        if label == "endhit": # the results page is static: compare with its frame before the hit
            counts = [magenta_new(s, endpre[0], CENTRE) if endpre else -1 for s in shots]
        else:
            counts = [magenta_count(s, CENTRE) for s in shots]
        later = vis(lines, "endlater") if label == "endhit" else "0"
        check(f"end screen: no marker (a hit that lands {what})",
              vis(lines, label) == "0" and later == "0" and shots and all(c == 0 for c in counts) and
              (label != "endanim" or any(re.search(r"@endanim knHitmarker=\S+ endUI=1", l) for l in lines)),
              f"visible {vis(lines, label)}, magenta px per frame {counts}")

    if all(results) and results:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs and frames kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if results and all(results) else 1)


if __name__ == "__main__":
    main()

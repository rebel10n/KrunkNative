#!/usr/bin/env python3
"""Krunker Strike's desktop reload path (client/src/ui/strike_hud.c, FRVR 1.37.2's tfe / qde / Xde).

  python3 -I server/tests/strike_reload_desktop_e2e.py BUILD_DIR      (run from the repository root)

One live offline Strike client (--flavor strike --offline), driven by an input script on the keyboard
only (no touch event: strike_touch_input() is 0, so the keyboard is the input):
  1. the desktop reload overlay (.frvr-in-game-ui__ammo-overlay--desktop: the reload.svg ring and
     "cur / max") is drawn with a full magazine, next to the mobile overlay ("Reloading", efe); the
     touch reload ring is hidden, and the keyboard hint is not up (the magazine is full).
  2. holding the alt shoot bind (BACKSLASH, 220) empties the magazine: the hint shows "[R] Reload",
     the overlay reads "0 / <max>".
  3. the Reload key (R): the overlay shows "- / <max>" while reloading (state reload > 0).
  4. a touch on the Strike page (`touch X Y`) makes the touch controls the input: the desktop overlay
     and the hint go, and the touch reload ring is shown again (its label has a box).
KRUNKNATIVE_E2E_KEEP=1 keeps the work directory (the live log) when everything passed.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). KRUNKNATIVE_ASSETS_DIR is passed through
(a worktree without the untracked asset dirs). Prints PASS/FAIL lines; exit 0 only when all passed.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-strike-reload-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


def env(cfg, extra=None):
    e = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    e.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
              "XDG_STATE_HOME": cfg + "-state", "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    e.update(extra or {})
    return e


TAPS = 80  # the AR magazine is 35; a headless tap fires about every second press (the fire timer), so 80 taps empty it
TAP_MS = 260


def script_text():
    lines = ["0 play",
             "500 text @desk0 .frvr-in-game-ui__ammo-overlay--desktop",
             "520 text @efe0 .frvr-in-game-ui__ammo-overlay span",
             "540 text @hint0 .frvr-in-game-ui__msg-holder .frvr-in-game-ui__message span",
             "600 state",
             ]
    t = 800
    for _ in range(TAPS):
        lines.append(f"{t} down BACKSLASH")
        lines.append(f"{t + 150} up BACKSLASH")
        t += TAP_MS
    lines += [f"{t + 100} state",
              f"{t + 150} text @hint1 .frvr-in-game-ui__msg-holder .frvr-in-game-ui__message span",
              f"{t + 200} text @desk1 .frvr-in-game-ui__ammo-overlay--desktop",
              f"{t + 300} down R",
              f"{t + 350} up R",
              f"{t + 400} state",
              f"{t + 450} text @desk2 .frvr-in-game-ui__ammo-overlay--desktop",
              f"{t + 460} rects @rbtn0 .control-button__label-text span",
              f"{t + 470} rects @deskr .frvr-in-game-ui__ammo-overlay--desktop",
              f"{t + 500} touch 320 180",
              f"{t + 700} text @desk3 .frvr-in-game-ui__ammo-overlay--desktop",
              f"{t + 750} text @hint3 .frvr-in-game-ui__msg-holder .frvr-in-game-ui__message span",
              f"{t + 800} rects @rbtn .control-button__label-text span",
              f"{t + 850} state",
              f"{t + 900} quit"]
    return "\n".join(lines) + "\n"


def text_of(text, label):
    m = re.search(r"\[input_script\] text t=\d+ @%s n=(\d+) checked=\d value=.*? text=(.*)" % re.escape(label), text)
    return (int(m.group(1)), m.group(2).strip()) if m else (-1, "")


def states(text):
    """(ammo, reload) of every `state` line, in script order: [0] full, [1] after the burst, [2] reloading"""
    return [(int(a), float(r)) for a, r in re.findall(r"\[input_script\] state t=\d+ .*? ammo=(-?\d+) .*? reload=([\d.]+)", text)]


def rect_of(text, label):
    m = re.search(r"\[input_script\] rects t=\d+ @%s n=(\d+) (\S+)" % re.escape(label), text)
    if not m:
        return (0, 0.0, 0.0)
    parts = m.group(2).split("@")
    if len(parts) != 2:
        return (int(m.group(1)), 0.0, 0.0)
    dims = [float(v) for v in parts[1].split(",")]
    return (int(m.group(1)), dims[2], dims[3])


def main():
    if not os.path.isfile(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin", file=sys.stderr)
        return 2
    script = os.path.join(WORK, "reload.txt")
    with open(script, "w") as f:
        f.write(script_text())
    cfg = os.path.join(WORK, "cfg")
    os.makedirs(cfg, exist_ok=True)
    log = os.path.join(WORK, "live.log")
    e = env(cfg, {"KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    with open(log, "w") as f:
        rc = subprocess.run([f"{BUILD}/client_bin", "--flavor", "strike", "--offline"], cwd=ROOT, env=e, stdout=f,
                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=400).returncode
    with open(log, errors="replace") as f:
        text = f.read()
    check("live client ran its script", rc == 0, f"rc {rc}")

    n, t = text_of(text, "desk0")
    check("keyboard: the desktop overlay is up with a full magazine", n == 1 and re.match(r"^\d+ / \d+$", t) is not None, f"n={n} text={t!r}")
    n, t = text_of(text, "efe0")
    check("keyboard: the mobile overlay (Reloading) is still there", n >= 1 and t == "Reloading", f"n={n} text={t!r}")
    n, t = text_of(text, "hint0")
    check("keyboard: no reload hint while the magazine is full", n == 0, f"n={n} text={t!r}")
    count, w, h = rect_of(text, "deskr")
    check("keyboard: the desktop overlay is drawn (a box at the reload key's place)", count == 1 and w > 0 and h > 0, f"n={count} w={w} h={h}")

    st = states(text)
    ammo = st[1][0] if len(st) > 1 else None
    check("keyboard: holding the alt shoot bind empties the magazine", ammo == 0, f"ammo={ammo}")
    n, t = text_of(text, "hint1")
    check("keyboard: the hint reads \"[R] Reload\" with an empty magazine", n == 1 and t == "[R] Reload", f"n={n} text={t!r}")
    n, t = text_of(text, "desk1")
    # the empty click starts the game's own reload: the ring then reads "- / max" until it finishes
    check("keyboard: the overlay reads the empty magazine (0, or - while it reloads)", n == 1 and re.match(r"^(0|-) / \d+$", t) is not None, f"n={n} text={t!r}")

    reload = st[2][1] if len(st) > 2 else None
    n, t = text_of(text, "desk2")
    check("keyboard: R starts the reload and the overlay shows \"- / max\"",
          reload is not None and reload > 0.0 and n == 1 and re.match(r"^- / \d+$", t) is not None, f"reload={reload} n={n} text={t!r}")

    n, t = text_of(text, "desk3")
    check("touch: the desktop overlay is gone once the touch controls are the input", n == 0, f"n={n} text={t!r}")
    n, t = text_of(text, "hint3")
    check("touch: no keyboard hint", n == 0, f"n={n} text={t!r}")
    count, w, h = rect_of(text, "rbtn0")
    check("keyboard: the touch reload ring is hidden (no box)", count == 1 and w == 0 and h == 0, f"n={count} w={w} h={h}")
    count, w, h = rect_of(text, "rbtn")
    check("touch: the touch reload ring is shown again", count == 1 and w > 0 and h > 0, f"n={count} w={w} h={h}")

    ok = all(results)
    print(f"\n{sum(results)}/{len(results)} passed" + ("" if ok else f"  (logs in {WORK})"))
    if ok and not os.environ.get("KRUNKNATIVE_E2E_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

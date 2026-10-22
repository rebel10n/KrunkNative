#!/usr/bin/env python3
"""The editor's view has its own lighting, whatever page drew before it.

    python3 -I tools/editor/lighting_check.py --bin <build>/client_bin [--root .] [--egl-device nvidia|mesa]

A game on a bright map (ACES tone mapping, sRGB output, a strong sun, built from Burg) is played,
the editor is opened on Atomic over it (`page editor map=official/23_atomic`), and the editor's
frame must have the same mean luminance as a fresh editor on Atomic. Before the fix the game's
output state (toneMapping / outputEncoding / toneMappingExposure) stayed in RenderLighting after
the game page was hidden: the editor came out at ~0.73 against 0.60 (whites blown, colours
washed out) until the next relaunch.

The game page gets its own lighting back when it returns from the editor (map_environment_restore
in app_mode.c leave_page); that is not measured here: the page returns to its menu, whose frame
is not comparable pixel for pixel.

Headless (KRUNKNATIVE_HEADLESS=1, no display); needs ImageMagick for the luminance (magick or convert).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

TOLERANCE = 0.01  # mean luminance, 0..1

BRIGHT = {"toneMapping": 4, "toneMappingExposure": 1.5, "outputEncoding": 1, "lightI": 2.5, "ambientI": 1.5}


def magick():
    for name in ("magick", "convert"):
        path = shutil.which(name)
        if path:
            return path
    return None


def luminance(tool, png):
    cmd = [tool, png, "-colorspace", "Gray", "-format", "%[fx:mean]", "info:"]
    return float(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout.strip())


def run(binary, root, args, env_extra, shot, out):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(out, "cfg-" + os.path.basename(shot)),
                "KRUNKNATIVE_SCREENSHOT": shot, "KRUNKNATIVE_FIXED_DELTA": "16"})
    env.update(env_extra)
    log = os.path.join(out, os.path.basename(shot) + ".log")
    with open(log, "w") as f:
        r = subprocess.run([binary] + args, cwd=root, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=300)
    return r.returncode, log


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", required=True, help="client_bin")
    ap.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    ap.add_argument("--egl-device", default="", help="KRUNKNATIVE_EGL_DEVICE (nvidia, mesa, an index)")
    ap.add_argument("--keep", help="keep the screenshots and logs in this directory")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    tool = magick()
    if not tool:
        print("lighting_check: ImageMagick (magick / convert) is needed for the luminance")
        return 2

    out = a.keep or tempfile.mkdtemp(prefix="kn-lighting-check-", dir=os.path.expanduser("~/.cache"))
    os.makedirs(out, exist_ok=True)
    burg = os.path.join(root, "assets", "maps", "official", "00_burg.json")
    with open(burg) as f:
        bright = json.load(f)
    bright.update(BRIGHT)
    bright_path = os.path.join(out, "bright_aces_srgb.json")
    with open(bright_path, "w") as f:
        json.dump(bright, f)
    script = os.path.join(out, "game-then-editor.script")
    with open(script, "w") as f:
        f.write("0 play\n300 page editor map=official/23_atomic\n")

    extra = {"KRUNKNATIVE_EGL_DEVICE": a.egl_device} if a.egl_device else {}
    binary = os.path.abspath(a.bin)
    failed = 0

    # 1. a fresh editor on Atomic: the baseline
    fresh_shot = os.path.join(out, "fresh.png")
    env = dict(extra, KRUNKNATIVE_EDITOR_SCRIPT="wait:0.5")
    rc, log = run(binary, root, ["--offline", "--editor", "assets/maps/official/23_atomic.json"], env, fresh_shot, out)
    if rc != 0 or not os.path.exists(fresh_shot):
        print("FAIL fresh editor: exit %d (log: %s)" % (rc, log))
        return 1
    fresh = luminance(tool, fresh_shot)

    # 2. the editor opened after a bright game
    bright_shot = os.path.join(out, "after-bright-game.png")
    rc, log = run(binary, root, ["--offline", bright_path], dict(extra, KRUNKNATIVE_INPUT_SCRIPT=script), bright_shot, out)
    if rc != 0 or not os.path.exists(bright_shot):
        print("FAIL editor after a bright game: exit %d (log: %s)" % (rc, log))
        return 1
    after = luminance(tool, bright_shot)
    if abs(after - fresh) <= TOLERANCE:
        print("editor after a bright game: luminance %.4f, fresh editor %.4f: ok" % (after, fresh))
    else:
        failed += 1
        print("FAIL editor after a bright game: luminance %.4f, fresh editor %.4f (tolerance %.3f); shots in %s"
              % (after, fresh, TOLERANCE, out))

    # 3. the game page after an editor round trip, against the same game without the editor
    print("lighting check: %s" % ("failed %d" % failed if failed else "passed"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

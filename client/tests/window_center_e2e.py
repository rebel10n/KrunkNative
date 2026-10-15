#!/usr/bin/env python3
"""The client window opens where it stays: created hidden, placed (centred on the monitor's work area,
or at its kept position), only then shown; never mapped first and moved after
(client_render_window_created, window_state_center).

  python3 -I client/tests/window_center_e2e.py BUILD_DIR   (repo root)

Each case runs a real (not headless) client_bin on a PRIVATE Xvfb of its own (server/tests/x11_watch.py:
-displayfd, no TCP, never the user's display) with no window manager, so a window maps exactly where the
program put it, and watches the root window's MapNotify / ConfigureNotify:
  - launch: a first start (no kept window) maps its 1280x720 window centred on the 1600x1000 screen;
  - relaunch: F4 / F5 / the Settings relaunch (KRUNKNATIVE_RESTORE_WINDOW) maps at the kept position;
  - editor: a page window (--editor, KRUNKNATIVE_WINDOW_SIZE=1200x800 as app_window.c opens it, with
    the opener's centre in KRUNKNATIVE_WINDOW_NEAR) maps centred;
and in each the window's first mapped position is the final one (no move after the map).
Exit 0 when every check passed, 77 when Xvfb or client_bin is missing.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = os.getcwd()
sys.path.insert(0, os.path.join(ROOT, "server/tests"))
from x11_watch import PrivateXvfb, X11Watch  # noqa: E402

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
SCREEN_W, SCREEN_H = 1600, 1000
# the asset tree: this checkout's, else the main checkout's (a worktree may lack the large content)
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or next(
    (d for d in (os.path.join(ROOT, "assets"), os.path.expanduser("~/src/KrunkNative/assets")) if os.path.isdir(os.path.join(d, "models"))),
    os.path.join(ROOT, "assets"))
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"), flush=True)


def run_case(work, label, args, env_extra, title_re):
    """One client_bin on its own Xvfb until its input script quits; the watcher's maps and moves."""
    xvfb = PrivateXvfb("%dx%dx24" % (SCREEN_W, SCREEN_H))
    watch = X11Watch(xvfb.display)
    script = os.path.join(work, label + ".script")
    with open(script, "w") as f:
        f.write("0 boot\n2500 quit\n")
    log = os.path.join(work, label + ".log")
    env = {k: v for k, v in os.environ.items()
           if k not in ("WAYLAND_DISPLAY", "KRUNKNATIVE_HEADLESS", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_WINDOW_SIZE")}
    env.update({"DISPLAY": xvfb.display, "XDG_CONFIG_HOME": os.path.join(work, label + "-cfg"),
                "XDG_STATE_HOME": os.path.join(work, label + "-state"), "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_ASSET_FETCH": "off",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": "off", "KRUNKNATIVE_ASSETS_DIR": ASSETS})
    env.update(env_extra)
    out = open(log, "w")
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin", *args], stdout=out, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, env=env, start_new_session=True, cwd=ROOT)
    try:
        p.wait(timeout=120)
    except subprocess.TimeoutExpired:
        check(f"{label}: client_bin finished its script", False, f"timeout, log {log}")
    finally:
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
        out.close()
        time.sleep(0.2)
        maps = [m for m in watch.maps() if re.search(title_re, m["name"])]
        moves = {m["window"]: watch.moves_after_map(m["window"]) for m in maps}
        watch.stop()
        xvfb.stop()
    with open(log, errors="replace") as f:
        text = f.read()
    return maps, moves, text


def expect_case(work, label, args, env_extra, want_xy, want_wh, how):
    maps, moves, text = run_case(work, label, args, env_extra, r"Krunker")
    check(f"{label}: the client window was mapped once", len(maps) == 1, [(m["name"], m["x"], m["y"]) for m in maps] or text[-600:])
    if not maps:
        return
    m = maps[0]
    check(f"{label}: its first mapped position is {how} {want_xy}", (m["x"], m["y"]) == want_xy and (m["w"], m["h"]) == want_wh,
          (m["x"], m["y"], m["w"], m["h"]))
    check(f"{label}: never moved after it was mapped", not moves[m["window"]], moves[m["window"]])
    line = re.search(r"\[window\] opened .*", text)
    check(f"{label}: the client logs where it placed the window", line and "at %d,%d" % want_xy in line.group(0), line.group(0) if line else None)


def main():
    if not shutil.which("Xvfb") or not os.path.isfile(f"{BUILD}/client_bin"):
        print("window_center_e2e: needs Xvfb and client_bin: skipped")
        return 77
    work = tempfile.mkdtemp(prefix="kn-window-center-e2e-")
    w, h = 1280, 720
    expect_case(work, "launch", ["--offline"], {}, ((SCREEN_W - w) // 2, (SCREEN_H - h) // 2), (w, h), "centred at")
    expect_case(work, "relaunch", ["--offline"], {"KRUNKNATIVE_RESTORE_WINDOW": "0,100,50,800,600,0,0"}, (100, 50), (800, 600),
                "the kept position")
    ew, eh = 1200, 800
    expect_case(work, "editor", ["--offline", "--editor", ""],
                {"KRUNKNATIVE_WINDOW_SIZE": "%dx%d" % (ew, eh), "KRUNKNATIVE_WINDOW_NEAR": "800,500"},
                ((SCREEN_W - ew) // 2, (SCREEN_H - eh) // 2), (ew, eh), "centred at")
    if all(results):
        shutil.rmtree(work, ignore_errors=True)
    else:
        print(f"logs kept in {work}")
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

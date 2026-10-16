#!/usr/bin/env python3
"""The developer console works on Krunker Strike's page as on the default page (client/src/ui/ui_console.c,
ui/strike/, ui/css/devconsole.css).

  python3 -I client/tests/console_strike_e2e.py BUILD_DIR      (run from the repository root)

The bug: on Krunker Strike the console showed up unstyled and broken: its stylesheet (css/devconsole.css,
relative to the page) and its font (fonts/lucon.ttf) were not in ui/strike/ at all, so the overlay had no box,
no height, the input row ran off the window, the close button drew as a box (no kn-symbols.ttf fallback). The
fix puts the console's own resources in ui/strike/css/ at the paths the console asks for, and the default
width / height of the box now fit the 640x360 phone viewport (the outer box was 2 px wider than the viewport
minus its 8 px margin).

For each flavor (--flavor strike, then the default one) a headless offline client (1280x720 window, fixed
60 fps step, input script) does:
  1. ` opens the developer console; its probe says it is open, built, shown, styled (sheet=1), the last
     element of <body>, topmost at all 5 points of its box, inside the viewport (the viewport is the phone
     page's 640x360 CSS px on Strike);
  2. the typed line `help` is in the input (conin), Enter runs it and clears the input;
  3. the output reads back: the log holds the command list (`N commands:`);
  4. the close button's glyph is drawn (no tofu), and the input row lies inside the window.
Strike's ui/strike/css/ also holds the same bytes as the default page's ui/css/ for the console's files.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import filecmp
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-console-strike-", dir=CACHE if os.path.isdir(CACHE) else None)
WINDOW = (1280, 720)
results = []

# each flavor's page viewport in CSS px: Strike's phone page is 640x360 (STRIKE_CSS_HEIGHT 360) in the
# 1280x720 window, the default page is 1280x720
RUNS = [
    ("strike", ["--flavor", "strike"], "640x360"),
    ("base", ["--flavor", "base"], "1280x720"),
]

# input scripts cut a line at '#' (a comment): the selectors use [id=...] for ids
SCRIPT = "\n".join([
    "0 menu",                                    # the main menu is up (the page is set up)
    "600 down GRAVE", "640 up GRAVE",            # ` opens the developer console
    "900 console @open",
    "1000 type help",
    "1100 conin @typed",                         # value=[help]
    "1200 down ENTER", "1240 up ENTER",          # runs it, clears the input
    "1600 conin @sent",                          # value=[]
    "1700 grep @help commands: [id=devConsoleOverlay] .logs",
    "1800 glyphs @close [id=devConsoleOverlay] .titlebar .x",
    "1900 rects @input [id=devConsoleOverlay] .row input",
    "2000 console @after",
    "2100 quit",
]) + "\n"

PROBE = re.compile(r"\[input_script\] console t=\d+ @(\S+) (.*)$", re.M)
CONIN = re.compile(r"\[input_script\] conin t=\d+ @(\S+) value=\[(.*)\] hints=(-?\d+)")
GREP = re.compile(r"\[input_script\] grep t=\d+ @(\S+) needle=(.*?) n=(\d+) hits=(\d+)")
GLYPHS = re.compile(r"\[input_script\] glyphs t=\d+ @(\S+) n=(\d+) missing=(-?\d+) first=(\S+)")
RECTS = re.compile(r"\[input_script\] rects t=\d+ @(\S+) n=(\d+)(.*)$", re.M)


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def run_client(name, flavor_args, timeout=300):
    script = os.path.join(WORK, f"{name}.script")
    with open(script, "w") as f:
        f.write(SCRIPT)
    cfg = os.path.join(WORK, f"{name}-cfg")
    os.makedirs(cfg, exist_ok=True)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667",
                "KRUNKNATIVE_WINDOW_SIZE": "%dx%d" % WINDOW})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", *flavor_args, "--offline", "--map", "kanji"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def probe_fields(text, label):
    fields = None
    for m in PROBE.finditer(text):
        if m.group(1) == label:
            fields = dict(kv.split("=", 1) for kv in m.group(2).split() if "=" in kv)
    return fields


def run_flavor(name, flavor_args, vp):
    text = run_client(name, flavor_args)
    p = probe_fields(text, "open")
    if not p:
        check(f"{name}: the console probe ran", False, "no probe line")
    else:
        keys = ("open", "built", "shown", "sheet", "last", "inside")
        ok = all(p.get(k) == "1" for k in keys) and p.get("top") == "5" and p.get("vp") == vp
        check(f"{name}: ` opens the console on the page, styled and on top", ok,
              " ".join(f"{k}={v}" for k, v in p.items()))
        if name == "strike":  # the box's outer size (rect=x,y,w,h): the Strike size, not the 744x280 of the desktop page
            rw, rh = (float(v) for v in p.get("rect", "0,0,0,0").split(",")[2:])
            check(f"{name}: the console box is the Strike size (414x214 outer), not the desktop one (624x294 on this page)",
                  rw <= 420 and rh <= 220, f"rect w={rw} h={rh}")
    inputs = {m.group(1): m.group(2) for m in CONIN.finditer(text)}
    check(f"{name}: the typed line reaches the input", inputs.get("typed") == "help", repr(inputs.get("typed")))
    check(f"{name}: Enter runs it and clears the input", inputs.get("sent") == "", repr(inputs.get("sent")))
    grep = {m.group(1): m for m in GREP.finditer(text)}
    hits = int(grep["help"].group(4)) if "help" in grep else -1
    check(f"{name}: the output reads back (the command list is in the log)", hits >= 1,
          grep["help"].group(0) if "help" in grep else "no grep line")
    glyphs = {m.group(1): m for m in GLYPHS.finditer(text)}
    g = glyphs.get("close")
    check(f"{name}: the close button's glyph is drawn", g is not None and g.group(2) == "1" and g.group(3) == "0",
          g.group(0) if g else "no glyphs line")
    rects = {m.group(1): m for m in RECTS.finditer(text)}
    r = rects.get("input")
    box = None
    if r and int(r.group(2)) == 1:
        nums = re.search(r"-@([-\d.]+),([-\d.]+),([-\d.]+),([-\d.]+)", r.group(3))
        box = tuple(float(v) for v in nums.groups()) if nums else None
    inside = box is not None and box[2] > 0 and box[3] > 0 and box[0] >= 0 and box[1] >= 0 \
        and box[0] + box[2] <= WINDOW[0] + 0.5 and box[1] + box[3] <= WINDOW[1] + 0.5
    check(f"{name}: the input row lies inside the window", inside, r.group(0) if r else "no rects line")
    return text


def check_resources():
    """ui/strike/css/ holds the same console files as ui/css/ (the paths css/devconsole.css and fonts/ ask for)"""
    for rel in ("css/devconsole.css", "css/fonts/lucon.ttf", "css/fonts/kn-symbols.ttf"):
        base = os.path.join(ROOT, "ui", rel)
        strike = os.path.join(ROOT, "ui", "strike", rel)
        check(f"ui/strike/{rel} is the base page's file", os.path.isfile(strike) and filecmp.cmp(base, strike, shallow=False),
              strike)


def main():
    if not os.path.exists(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin")
        return 1
    check_resources()
    for name, flavor_args, vp in RUNS:
        run_flavor(name, flavor_args, vp)
    shutil.rmtree(WORK, ignore_errors=True)
    failed = results.count(False)
    print(f"{len(results) - failed}/{len(results)} passed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

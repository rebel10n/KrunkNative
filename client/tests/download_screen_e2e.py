#!/usr/bin/env python3
"""The native client never offers "Download our client" (the Download window, showWindow(25),
client/src/ui/download_ui.c): not at startup, not from the menu, not from the hub.

  python3 -I client/tests/download_screen_e2e.py BUILD_DIR      (from the repository root)

A fresh profile (an empty XDG_CONFIG_HOME) and a headless offline client_bin on Kanji, driven by an
input script:
  1. startup: the menu is up, no window is open and the Download window's package list (.knDlFoot,
     download_ui.c) is not in the page;
  2. More Krunker (showMoreKrunker, the nav's "more" button): its four tiles are there, and there is
     no "Download our client" block (.moreKrunkerClient) or "Download" text in the popup;
  3. the hub (page hub): the Android app badge (#androidDB) is not shown; it is a link to the Download
     window natively (social_ui.c), and the desktop client is the client already;
  4. positive control: showWindow(25) puts the package list (.knDlFoot) in the page, so the probe can see
     the window at all.
The header's client.png (menu_ui.c) is gated the same way natively and is not probed here.
Exit code 0 when every check passed; logs are kept in ~/.cache/kn-dlscreen-e2e-* on failure.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-dlscreen-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
failures = []


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def run_native(script, timeout=240):
    path = os.path.join(WORK, "native.script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, "native.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"), "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(False, f"client_bin finished within {timeout} s")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read()


def vis(log, label):
    m = re.search(r"\[input_script\] vis t=\d+ " + re.escape(label) + r" (.*)", log)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def nodes(log, label):
    m = re.search(r"\[input_script\] nodes t=\d+ " + re.escape(label) + r" elements=\d+ match=(\d+)", log)
    return int(m.group(1)) if m else -1


def grep(log, label):
    m = re.search(r"\[input_script\] grep t=\d+ " + re.escape(label) + r" needle=\S+ n=(\d+) hits=(\d+)", log)
    return int(m.group(2)) if m else -1


# '#' starts a comment in input scripts: ids are [id=...]
script = [
    "0 menu",
    "300 vis @start menuWindow",
    "300 nodes @start0 .knDlFoot",
    "400 call showMoreKrunker",
    "700 nodes @more .moreKrunkerOption",
    "700 nodes @promo .moreKrunkerClient",
    "700 grep @dl Download [id=genericPop]",
    "700 vis @more genericPop",
    "900 call showWindow 25",
    "1100 nodes @open .knDlFoot",
    "1200 call clearPops",
    "1300 page hub",
    "2500 vis @hub androidDB",
    "2600 quit",
]
log = run_native("\n".join(script))

start = vis(log, "@start")
check(nodes(log, "@start0") == 0, f"native, fresh profile: no Download window's package list at startup ({nodes(log, '@start0')})")
check(start.get("menuWindow") in ("0", "-"), f"native, fresh profile: no window is open at startup ({start})")
check(nodes(log, "@more") == 4, f"native: More Krunker shows its four tiles ({nodes(log, '@more')})")
check(nodes(log, "@promo") == 0, f"native: More Krunker has no 'Download our client' block ({nodes(log, '@promo')})")
check(grep(log, "@dl") == 0, f"native: More Krunker says Download nowhere ({grep(log, '@dl')})")
more = vis(log, "@more")
check(more.get("genericPop") == "1", f"native: More Krunker is the popup on screen ({more})")
check(nodes(log, "@open") == 1,
      f"positive control: showWindow(25) puts the Download window's package list in the page ({nodes(log, '@open')})")
hub = vis(log, "@hub")
check(hub.get("androidDB") in ("0", "-"), f"native: the hub's Android app badge is not shown ({hub})")
check("[input_script] page" in log and "[hub]" in log, "the hub page was entered (its log line is there)")

shutil.rmtree(WORK, ignore_errors=True) if not failures else print(f"logs kept in {WORK}")
if failures:
    print(f"{len(failures)} check(s) failed")
    sys.exit(1)
print("download screen: all checks passed")

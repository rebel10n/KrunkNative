#!/usr/bin/env python3
"""The native client offers "Download our client" (the Download window, showWindow(25),
client/src/ui/download_ui.c): the More Krunker tile and the hub's Android app badge. It is how mobile
players get the client, so it stays on the desktop build and in the phone layout (KRUNKNATIVE_MOBILE=1).

  python3 -I client/tests/download_screen_e2e.py BUILD_DIR      (from the repository root)

For each layout (desktop, mobile): a fresh profile (an empty XDG_CONFIG_HOME) and a headless offline
client_bin on Kanji, driven by an input script:
  1. startup: the menu is up, no window is open and the Download window's package list (.knDlFoot)
     is not in the page (the window opens on a click, not at startup);
  2. More Krunker (showMoreKrunker, the nav's "more" button): its four tiles and the "Download our
     client" block (.moreKrunkerClient) are there, and the popup says Download;
  3. clicking "Download our client" opens the Download window (its package list is in the page);
  4. the hub (page hub): the Android app badge (#androidDB) is shown; it links to the Download window
     (/?download, social_ui.c);
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


def run_native(tag, script, mobile, timeout=240):
    path = os.path.join(WORK, tag + ".script")
    with open(path, "w") as f:
        f.write(script.strip() + "\n")
    log = os.path.join(WORK, tag + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "XDG_CONFIG_HOME": os.path.join(WORK, tag + "-cfg"), "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    if mobile:
        env["KRUNKNATIVE_MOBILE"] = "1"
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(False, f"{tag}: client_bin finished within {timeout} s")
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
SCRIPT = [
    "0 menu",
    "300 vis @start menuWindow",
    "300 nodes @start0 .knDlFoot",
    "400 call showMoreKrunker",
    "700 nodes @more .moreKrunkerOption",
    "700 nodes @promo .moreKrunkerClient",
    "700 grep @dl Download [id=genericPop]",
    "700 vis @more genericPop",
    "900 clicksel .moreKrunkerClientBtn",
    "1100 nodes @clicked .knDlFoot",
    "1200 call clearPops",
    "1500 page hub",
    "2600 vis @hub androidDB",
    "2700 quit",
]


def check_layout(tag, mobile):
    log = run_native(tag, "\n".join(SCRIPT), mobile)
    print(f"-- {tag} layout ({'phone, KRUNKNATIVE_MOBILE=1' if mobile else 'desktop'})")
    check(nodes(log, "@start0") == 0, f"{tag}: no Download window's package list at startup ({nodes(log, '@start0')})")
    check(vis(log, "@start").get("menuWindow") in ("0", "-"), f"{tag}: no window is open at startup")
    check(nodes(log, "@more") == 4, f"{tag}: More Krunker shows its four tiles ({nodes(log, '@more')})")
    check(nodes(log, "@promo") == 1, f"{tag}: More Krunker has the 'Download our client' block ({nodes(log, '@promo')})")
    check(grep(log, "@dl") >= 1, f"{tag}: More Krunker says Download ({grep(log, '@dl')})")
    check(vis(log, "@more").get("genericPop") == "1", f"{tag}: More Krunker is the popup on screen")
    check(nodes(log, "@clicked") == 1, f"{tag}: 'Download our client' opens the Download window ({nodes(log, '@clicked')})")
    check(vis(log, "@hub").get("androidDB") == "1", f"{tag}: the hub's Android app badge is shown ({vis(log, '@hub')})")


check_layout("desktop", mobile=False)
check_layout("mobile", mobile=True)

if failures:
    print(f"{len(failures)} check(s) failed; logs kept in {WORK}")
    sys.exit(1)
shutil.rmtree(WORK, ignore_errors=True)
print("download screen: all checks passed")

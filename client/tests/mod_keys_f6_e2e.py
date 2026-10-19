#!/usr/bin/env python3
"""F6 opens the map editor from the menu and the game page, and does nothing while a text field has
focus (client/src/ui/ui_mod_keys.c, dispatched by ui_mods.c; the v3.1.1 mods.ts switch KrunkNative keeps).

  python3 -I client/tests/mod_keys_f6_e2e.py BUILD_DIR      (run from the repository root)

Headless client_bin runs (offline Kanji, input scripts, private XDG_CONFIG_HOME):
  1. menu page: F6 routes to editor.html (the log shows "Creating editor window" and
     "[app] editor.html -> editor window").
  2. game page (after play): F6 routes to editor.html the same way.
  3. game page with the chat field focused (Enter): F6 does nothing, no editor route.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-f6-", dir=CACHE if os.path.isdir(CACHE) else None)
CFG = os.path.join(WORK, "cfg")
ROUTE = "[app] editor.html -> editor window"
OPENED = "Creating editor window"
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(name, script, timeout=240):
    path = os.path.join(WORK, f"{name}.script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": CFG, "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        return f.read()


def main():
    os.makedirs(CFG, exist_ok=True)

    # 1. the menu page: F6 opens the editor
    text = run("menu", "0 menu\n300 down F6\n400 up F6\n1500 quit\n")
    check("menu: F6 routes to the editor window", OPENED in text and ROUTE in text, f"log {WORK}/menu.log")

    # 2. the game page: F6 opens the editor
    text = run("game", "0 play\n500 down F6\n600 up F6\n1500 quit\n")
    check("game: F6 routes to the editor window", OPENED in text and ROUTE in text, f"log {WORK}/game.log")

    # 3. a text field has focus (the chat field, Enter): F6 does nothing
    text = run("typing", "0 play\n500 down ENTER\n550 up ENTER\n700 down F6\n750 up F6\n1500 quit\n")
    check("text field focused: F6 does nothing", OPENED not in text and ROUTE not in text, f"log {WORK}/typing.log")

    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

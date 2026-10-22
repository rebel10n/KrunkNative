#!/usr/bin/env python3
"""F6 opens the map editor from the menu and the game page, and does nothing while a text field has
focus (client/src/ui/ui_mod_keys.c, dispatched by ui_mods.c; the v3.1.1 mods.ts switch KrunkNative keeps).

  python3 -I client/tests/mod_keys_f6_e2e.py BUILD_DIR      (run from the repository root)

Headless client_bin runs (offline Kanji, input scripts, private XDG_CONFIG_HOME):
  1. menu page: F6 routes to editor.html (the log shows "Creating editor window" and
     "[app] editor.html -> editor window").
  2. game page (after play): F6 routes to editor.html the same way.
  3. game page with the chat field focused (Enter): F6 does nothing, no editor route.
  4. Clockwork played, the editor's last session is Atomic (its autosave): F6 opens Clockwork
     ("[editor] opened the staged map: Clockwork"), never the autosave.
  5. a custom map played (a Clockwork copy named "F6 Probe"): F6 opens that map the same way.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-f6-", dir=CACHE if os.path.isdir(CACHE) else None)
CFG = os.path.join(WORK, "cfg")
ROUTE = "[app] editor.html -> editor window"
OPENED = "Creating editor window"
KANJI = "assets/maps/official/10_kanji.json"
CLOCKWORK = "assets/maps/official/28_clockwork.json"
ATOMIC = "assets/maps/official/23_atomic.json"
STAGED = "[editor] opened the staged map: "
AUTOSAVE = "[editor] restored the last session: "
EDITOR_UP = "[editor] entered ("  # the editor window's page is up (logged after the map is chosen)
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def run(name, script, timeout=240, map_path=KANJI, until=None):
    """Run client_bin on the script. With `until`, the game stays up (the script has no quit) until
    that text shows in the log: the editor is its own process and only logs after it boots, and it
    exits with its game. Then the game gets SIGTERM (a clean exit that flushes its log)."""
    path = os.path.join(WORK, f"{name}.script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": CFG, "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", map_path], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            if until is None:
                p.wait(timeout=timeout)
            else:
                deadline = time.monotonic() + timeout
                while p.poll() is None and time.monotonic() < deadline:
                    with open(log, errors="replace") as f:
                        if until in f.read():
                            break
                    time.sleep(0.25)
                if p.poll() is None:
                    p.send_signal(signal.SIGTERM)
                p.wait(timeout=60)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
        finally:
            try:
                os.killpg(p.pid, signal.SIGKILL) # the editor window this run opened, if still up
            except ProcessLookupError:
                pass
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

    # 4. the editor's last session is Atomic (its autosave): F6 from Clockwork opens Clockwork
    seed = os.path.join(CFG, "krunknative", "knEditorMap.json")
    os.makedirs(os.path.dirname(seed), exist_ok=True)
    shutil.copy(ATOMIC, seed)
    text = run("clockwork", "0 play\n500 down F6\n600 up F6\n", map_path=CLOCKWORK, until=EDITOR_UP)
    check("Clockwork: F6 stages Clockwork", "[controls] F6: the editor opens Clockwork" in text, f"log {WORK}/clockwork.log")
    check("Clockwork: the editor opens Clockwork, not its last session (Atomic)",
          STAGED + "Clockwork" in text and AUTOSAVE not in text, f"log {WORK}/clockwork.log")

    # 5. a custom map (a Clockwork copy named "F6 Probe"): F6 opens that map
    with open(CLOCKWORK) as f:
        probe = json.load(f)
    probe["name"] = "F6 Probe"
    custom = os.path.join(WORK, "f6_probe.json")
    with open(custom, "w") as f:
        json.dump(probe, f)
    text = run("custom", "0 play\n500 down F6\n600 up F6\n", map_path=custom, until=EDITOR_UP)
    check("custom map: the editor opens that map", STAGED + "F6 Probe" in text and AUTOSAVE not in text, f"log {WORK}/custom.log")

    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Developer console switch and the clear commands, end to end (client/src/ui/ui_mods.c, ui_console.c).

  python3 -I client/tests/console_settings_e2e.py BUILD_DIR      (run from the repository root)

Headless client_bin runs (offline Kanji, input scripts, private XDG_CONFIG_HOME):
  1. kro_setngss_devConsoleEnabled = true, open Settings (advanced, General) twice, restart, open it
     again: the value stays true and the "Enable Developer Console" switch shows checked. Regression:
     the window templates carry the row unchecked, and the first poll read that as a click, so every
     Settings open stored false.
  2. a real click on that switch turns it off (stored "false") and the next start keeps it off;
     a second click turns it back on.
  3. console `clear` / `cls` empty the console log; chat `/clear` empties #chatList locally (no
     chat line sent) and later lines still show.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
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
WORK = tempfile.mkdtemp(prefix="kn-con-settings-", dir=CACHE if os.path.isdir(CACHE) else None)
CFG = os.path.join(WORK, "cfg")
STORE = os.path.join(CFG, "krunknative")
KEY = os.path.join(STORE, "kro_setngss_devConsoleEnabled.json")
SWITCH = "[data-dev-console-setting] .switch"
CHECKED = "[data-dev-console-setting] input:checked"
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def stored():
    try:
        with open(KEY) as f:
            return f.read().strip()
    except OSError:
        return None


def run(name, script, timeout=180):
    path = os.path.join(WORK, f"{name}.script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": CFG, "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "assets/maps/kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        text = f.read()
    return {m.group(1): int(m.group(2)) for m in re.finditer(r"\[input_script\] nodes t=\d+ (@\S+) elements=\d+ match=(\d+)", text)}


# the menu's Settings window (windows[0], advanced view so the Experimental rows render)
def open_settings(t, label):
    return f"{t} call showWindow 1\n{t + 600} setscroll menuWindow 100000\n{t + 800} nodes {label} {CHECKED}\n"


def main():
    os.makedirs(STORE, exist_ok=True)
    with open(os.path.join(STORE, "krk_advanced.json"), "w") as f:
        f.write("1")
    with open(KEY, "w") as f:
        f.write("true")

    # 1. opening Settings (twice) and a restart keep it on
    n = run("open", "0 menu\n" + open_settings(500, "@first") + "1600 call closWind\n" + open_settings(2000, "@second") + "3200 quit\n")
    check("switch shows checked on the first Settings open", n.get("@first") == 1, str(n))
    check("switch shows checked on the second Settings open", n.get("@second") == 1, str(n))
    check("opening Settings keeps devConsoleEnabled true", stored() == "true", f"stored {stored()!r}")
    n = run("restart", "0 menu\n" + open_settings(500, "@after") + "1600 quit\n")
    check("after a restart the switch is still checked", n.get("@after") == 1, str(n))
    check("after a restart devConsoleEnabled is still true", stored() == "true", f"stored {stored()!r}")

    # 2. a click is still a click (off, persisted across a start, then on again)
    n = run("click-off", "0 menu\n" + open_settings(500, "@before") + f"1500 clicksel {SWITCH}\n1800 nodes @off {CHECKED}\n2000 quit\n")
    check("clicking the switch unchecks it", n.get("@before") == 1 and n.get("@off") == 0, str(n))
    check("clicking the switch stores false", stored() == "false", f"stored {stored()!r}")
    n = run("click-on", "0 menu\n" + open_settings(500, "@still-off") + f"1500 clicksel {SWITCH}\n1800 nodes @on {CHECKED}\n2000 quit\n")
    check("off survives a restart and Settings open", n.get("@still-off") == 0, str(n))
    check("clicking again checks it and stores true", n.get("@on") == 1 and stored() == "true", f"{n} stored {stored()!r}")

    # 3. console clear / cls, chat /clear (the echoed `say` shows only with Show Player Messages on)
    with open(os.path.join(STORE, "knSettings.json"), "w") as f:
        f.write('{"showMessages": true}')
    chat = "[id^=chatMsg_]"
    n = run("clear", f"""0 play
1000 con echo one
1100 nodes @log1 .logs div
1200 con clear
1300 nodes @clear .logs div
1400 con echo two
1500 nodes @log2 .logs div
1600 con cls
1700 nodes @cls .logs div
3000 con say hello
3500 nodes @chat1 {chat}
3600 down ENTER
3650 up ENTER
3700 type /clear
3800 down ENTER
3850 up ENTER
4000 nodes @chat2 {chat}
4100 con say again
4500 nodes @chat3 {chat}
4600 quit
""")
    check("console log has lines before clear", n.get("@log1", 0) > 0, str(n))
    check("console `clear` empties the log", n.get("@clear") == 0, str(n))
    check("console `cls` empties the log", n.get("@log2", 0) > 0 and n.get("@cls") == 0, str(n))
    check("chat has lines before /clear", n.get("@chat1", 0) > 0, str(n))
    check("chat `/clear` empties the chat log", n.get("@chat2") == 0, str(n))
    check("chat lines show again after /clear", n.get("@chat3", 0) > 0, str(n))

    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Raw mouse motion has one switch: Settings > General > Experimental > Raw Mouse Input (rawMouse).

  python3 -I client/tests/raw_mouse_e2e.py BUILD_DIR      (run from the repository root)

Headless client_bin runs (offline Kanji, input scripts, private XDG_CONFIG_HOME). The client logs
"[client] raw mouse motion on|off" each time it sets GLFW_RAW_MOUSE_MOTION (ui_client_settings.c):
  1. a fresh profile starts with raw mouse on, and the switch shows checked.
  2. a click turns it off (stored false, the log says off), it stays off across a start, and a
     second click turns it back on.
  3. the Client tab's old Raw Input (knClient rawInput true) becomes rawMouse true once; the
     stored rawInput is gone, so a later Controls click off sticks across a start.
  4. a stored rawInput false becomes rawMouse false.
  5. a rawMouse already stored is kept (rawInput true does not override it).
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-raw-mouse-", dir=CACHE if os.path.isdir(CACHE) else None)
CFG = os.path.join(WORK, "cfg")
STORE = os.path.join(CFG, "krunknative")
ROW = '[title="Use raw mouse input when available"]'
SWITCH = ROW + " .switch"
CHECKED = ROW + " input:checked"
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def stored(key):
    """The raw JSON of a stored setting (knClient / knSettings are one file each), or None."""
    try:
        with open(os.path.join(STORE, key + ".json")) as f:
            return f.read()
    except OSError:
        return None


def client_tab():
    try:
        with open(os.path.join(STORE, "knClient.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def game_settings():
    try:
        with open(os.path.join(STORE, "knSettings.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def raw_states(text):
    """Every raw motion state the client logged, in order."""
    return re.findall(r"\[client\] raw mouse motion (on|off)", text)


def raw_state(text):
    """The last raw motion state the client logged, or None."""
    states = raw_states(text)
    return states[-1] if states else None


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
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        text = f.read()
    nodes = {m.group(1): int(m.group(2)) for m in re.finditer(r"\[input_script\] nodes t=\d+ (@\S+) elements=\d+ match=(\d+)", text)}
    return nodes, text


def reset_profile(client=None, settings=None):
    shutil.rmtree(CFG, ignore_errors=True)
    os.makedirs(STORE)
    with open(os.path.join(STORE, "krk_advanced.json"), "w") as f:
        f.write("1")
    if client is not None:
        with open(os.path.join(STORE, "knClient.json"), "w") as f:
            json.dump(client, f)
    if settings is not None:
        with open(os.path.join(STORE, "knSettings.json"), "w") as f:
            json.dump(settings, f)


# the menu's Settings window, advanced view (General holds Experimental)
def open_settings(t, label):
    return f"{t} call showWindow 1\n{t + 600} setscroll menuWindow 100000\n{t + 800} nodes {label} {CHECKED}\n"


def click_switch(t):
    return f"{t} clicksel {SWITCH}\n"


def main():
    # 1. a fresh profile: raw mouse on, the switch shows checked
    reset_profile()
    n, text = run("fresh", "0 menu\n" + open_settings(500, "@shown") + "1600 quit\n")
    check("fresh profile: the Raw Mouse Input switch shows checked", n.get("@shown") == 1, str(n))
    check("fresh profile: raw mouse motion is on from the start", raw_states(text) and set(raw_states(text)) == {"on"},
          f"log says {raw_states(text)}")

    # 2. a click turns it off; the off survives a start; a second click turns it on
    n, text = run("click-off", "0 menu\n" + open_settings(500, "@before") + click_switch(1500) +
                  "1800 nodes @off " + CHECKED + "\n2000 quit\n")
    check("clicking the switch unchecks it", n.get("@before") == 1 and n.get("@off") == 0, str(n))
    check("clicking the switch turns raw mouse motion off", raw_state(text) == "off", f"log says {raw_state(text)!r}")
    check("clicking the switch stores rawMouse false", game_settings().get("rawMouse") is False, str(game_settings()))
    n, text = run("restart-off", "0 menu\n" + open_settings(500, "@after") + "1600 quit\n")
    check("off survives a restart (switch unchecked)", n.get("@after") == 0, str(n))
    check("off survives a restart (raw mouse motion off)", raw_state(text) == "off", f"log says {raw_state(text)!r}")
    n, text = run("click-on", "0 menu\n" + open_settings(500, "@x") + click_switch(1500) +
                  "1800 nodes @on " + CHECKED + "\n2000 quit\n")
    check("clicking again turns raw mouse motion on", n.get("@on") == 1 and raw_state(text) == "on",
          f"{n} log says {raw_state(text)!r}")

    # 3. a stored Raw Input true becomes rawMouse true once; the stored rawInput is dropped
    reset_profile(client={"rawInput": True})
    n, text = run("migrate-true", "0 menu\n" + open_settings(500, "@shown") + "1600 quit\n")
    check("stored Raw Input on: rawMouse becomes true", game_settings().get("rawMouse") is True, str(game_settings()))
    check("stored Raw Input on: raw mouse motion stays on", raw_state(text) == "on", f"log says {raw_state(text)!r}")
    check("the migrated rawInput is dropped from the Client store", "rawInput" not in client_tab(), str(client_tab()))
    n, text = run("migrate-off", "0 menu\n" + open_settings(500, "@x") + click_switch(1500) + "2000 quit\n")
    check("after the migration a Controls click off sticks", game_settings().get("rawMouse") is False, str(game_settings()))
    n, text = run("migrate-restart", "0 menu\n" + open_settings(500, "@after") + "1600 quit\n")
    check("the migration does not run again: off survives a restart",
          raw_state(text) == "off" and n.get("@after") == 0, f"log says {raw_state(text)!r} {n}")

    # 4. a stored Raw Input false becomes rawMouse false (the default is on)
    reset_profile(client={"rawInput": False})
    n, text = run("migrate-false", "0 menu\n" + open_settings(500, "@shown") + "1600 quit\n")
    check("stored Raw Input off: rawMouse becomes false", game_settings().get("rawMouse") is False, str(game_settings()))
    check("stored Raw Input off: raw mouse motion is off", raw_state(text) == "off", f"log says {raw_state(text)!r}")

    # 5. a rawMouse already stored is kept
    reset_profile(client={"rawInput": True}, settings={"rawMouse": False})
    n, text = run("migrate-kept", "0 menu\n" + open_settings(500, "@shown") + "1600 quit\n")
    check("a stored rawMouse is kept over a stored Raw Input",
          game_settings().get("rawMouse") is False and raw_state(text) == "off", f"{game_settings()} log says {raw_state(text)!r}")

    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""The developer console's `class` command and its tab completion (client/src/ui/ui_console.c, server
shared/src/chat_commands.c cmd_class, shared/src/player.c player_switch_class).

  python3 -I client/tests/class_console_e2e.py BUILD_DIR      (run from the repository root)

An offline Kanji client (headless client_bin, input script, fixed 60 fps step) opens the developer console
with the ` key and types into it (input script `type`, keys TAB / ENTER):
  1. `cla` + Tab completes the command name to "class ";
  2. `Spr` + Tab completes the class name, which contains spaces, to a quoted name: `class "Spray N Pray" `;
  3. Enter runs it: the local player is Spray N Pray (class 3) with its health (170, the class table's); a quoted
     name reached the command as one argument (position and the server's state: server/tests/sandbox_test.c test_class);
  4. `class 99` and `class Nope` are refused and change nothing;
  5. `class Triggerman Pistol` switches the class and the secondary weapon (the Pistol is weapon 2);
  6. `class Triggerman Pis` + Tab completes the secondary weapon name, unquoted (no space in it).
A second run with the developer console switched off (kro_setngss_devConsoleEnabled false): `class` is a dev
command, so a line that still reaches the console (a bind, `exec`, here input script `con`) neither runs it nor
sends it: the player stays the Triggerman and the sandbox logs no /class.
The probes that read the state: `conin @label` (the console input line) and `class @label` (the player).
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
WORK = tempfile.mkdtemp(prefix="kn-class-console-", dir=CACHE if os.path.isdir(CACHE) else None)
FRAME_MS = 16.667
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


# Chat lines reach the server at most 3 per second (net_handle_chat: a 4th closes the connection), so the
# commands are spaced more than a second apart.
SCRIPT = "\n".join([
    "0 play",                                       # close the menu and enter the game (waits for the map)
    "1200 down GRAVE", "1240 up GRAVE",           # open the developer console
    "1300 type cla", "1340 down TAB", "1380 up TAB",
    "1420 conin @cmd",                              # class " (the command name completed)
    "1460 type Spr", "1500 down TAB", "1540 up TAB",
    "1580 conin @tab",                              # class "Spray N Pray"
    "1620 down ENTER", "1660 up ENTER",
    "2100 class @spray",                            # class 3, health 170
    "2640 type class 99", "2700 down ENTER", "2740 up ENTER",
    "3300 class @refused",                          # unchanged
    "3740 type class Nope", "3800 down ENTER", "3840 up ENTER",
    "4400 class @nope",                             # unchanged
    "4840 type class Triggerman Pistol", "4900 down ENTER", "4940 up ENTER",
    "5500 class @trig",                             # class 0, secondary 2, health 100
    "5580 type class Triggerman Pis", "5620 down TAB", "5660 up TAB",
    "5700 conin @sec",                              # class Triggerman Pistol
    "5900 quit",
]) + "\n"

# the developer console off: `class` (a dev command) is neither run nor sent, directly or through exec
SCRIPT_DEV_OFF = "\n".join([
    "0 play",
    "1200 con class \"Spray N Pray\"",
    "1700 class @direct",
    "2400 con exec class 3",
    "2900 class @exec",
    "3000 quit",
]) + "\n"

PROBE = re.compile(r"\[input_script\] class t=\d+ @(\S+) index=(-?\d+) name=(.*?) secondary=(-?\d+) health=(-?\d+) max=(-?\d+) loadout=(\S+)")
CONIN = re.compile(r"\[input_script\] conin t=\d+ @(\S+) value=\[(.*)\] hints=(-?\d+)")


def run(name="class", source=SCRIPT, dev_console=True, timeout=240):
    script = os.path.join(WORK, name + ".script")
    with open(script, "w") as f:
        f.write(source)
    log = os.path.join(WORK, name + ".log")
    store = os.path.join(WORK, name + "-cfg", "krunknative")
    os.makedirs(store, exist_ok=True)
    with open(os.path.join(store, "kro_setngss_devConsoleEnabled.json"), "w") as f:
        f.write("true" if dev_console else "false")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "XDG_CONFIG_HOME": os.path.join(WORK, name + "-cfg"), "XDG_STATE_HOME": os.path.join(WORK, name + "-state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_FIXED_DELTA": str(FRAME_MS), "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check("client_bin finished its script", False, f"timeout, log {log}")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    with open(log, errors="replace") as f:
        return f.read(), log


def probes(text):
    classes = {m.group(1): m.groups() for m in PROBE.finditer(text)}
    inputs = {m.group(1): m.group(2) for m in CONIN.finditer(text)}
    return classes, inputs


try:
    text, log = run()
    classes, inputs = probes(text)
    got = inputs.get("cmd")
    check("1. `cla` + Tab completes the command name", got == "class ", repr(got))
    got = inputs.get("tab")
    check("2. `Spr` + Tab completes the class name quoted", got == 'class "Spray N Pray" ', repr(got))

    c = classes.get("spray")
    check("3. Enter switches to Spray N Pray (class 3) with its health 170",
          c is not None and c[1] == "3" and c[2] == "Spray N Pray" and c[4] == "170" and c[5] == "170",
          repr(c) if c else f"no class probe; log {log}")
    base = classes.get("spray")
    c = classes.get("refused")
    check("4a. `class 99` is refused and changes nothing",
          c is not None and base is not None and c[1:6] == base[1:6], repr(c))
    c = classes.get("nope")
    check("4b. `class Nope` is refused and changes nothing",
          c is not None and base is not None and c[1:6] == base[1:6], repr(c))
    c = classes.get("trig")
    check("5. `class Triggerman Pistol`: class 0, the Pistol (2) as the secondary, full health 100",
          c is not None and c[1] == "0" and c[2] == "Triggerman" and c[3] == "2" and c[4] == "100" and c[5] == "100", repr(c))
    got = inputs.get("sec")
    check("6. `class Triggerman Pis` + Tab completes the secondary weapon unquoted", got == "class Triggerman Pistol ", repr(got))

    text, log = run("devoff", SCRIPT_DEV_OFF, dev_console=False)
    classes, _ = probes(text)
    for label, how in (("direct", "typed"), ("exec", "through exec")):
        c = classes.get(label)
        check(f"7. dev console off: `class` {how} does nothing (still the Triggerman)", c is not None and c[1] == "0", repr(c) if c else f"no probe; log {log}")
    check("7. dev console off: no /class reached the sandbox", "/class" not in text, log)
finally:
    if all(results) and not os.environ.get("KN_TEST_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")

print("class_console_e2e:", "PASS" if all(results) else "FAIL")
sys.exit(0 if all(results) else 1)

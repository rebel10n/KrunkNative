#!/usr/bin/env python3
"""The backtick toggle never frees the mouse without showing the console (client/src/ui/ui_console.c,
client/src/ui/ui_mods.c, client/src/ui/menu_ui.c).

  python3 -I client/tests/console_toggle_race_e2e.py BUILD_DIR     (run from the repository root)

The bug: "sometimes pressing backtick to open the console doesn't show the console overlay; the
mouse will get unlocked but there won't be a visible console." The toggle flipped devConsole.visible
(which the pointer lock read to free the mouse, and which opened the game page's menu for
exitPointerLock) the instant the key fired, but the overlay was only displayed once it was live in
the ready document shown now. When it was not live yet (a page still loading, the overlay just
replaced by a page re-render, a page just switched), the mouse was freed but no console appeared.

The invariant checked here, after every toggle and in every state (menu, in game, dead, respawn,
a map switch, rapid taps, the editor page and back): the console is "open" (devConsole.visible)
only when it is also "shown" (its overlay on screen), and whenever the toggle freed the mouse for
the console the overlay is shown. One key press is one toggle (an even number of taps leaves it
closed and the pointer relocked, an odd number leaves it open and shown). Driven headless with the
input script `console` / `vis :cursor` / `state` probes.

Note: the native headless game page is always ready (only a `page` switch resets readiness, and
that pauses the script clock), so the not-ready window itself is a web / in-client-webview
phenomenon; this test locks the coupling and guards against regressions on every scriptable path.
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
WORK = tempfile.mkdtemp(prefix="kn-console-race-", dir=CACHE if os.path.isdir(CACHE) else None)
CFG = os.path.join(WORK, "cfg")
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def run_client(name, script, timeout=300):
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": CFG,
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "--map", "kanji"], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
        finally:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
    return read(log)


def probes(text):
    """input_script `console @label ...` -> {label: {key: value}} (the last line per label)"""
    out = {}
    for m in re.finditer(r"\[input_script\] console t=\d+ @(\S+) (.*)$", text, re.M):
        out[m.group(1)] = dict(kv.split("=", 1) for kv in m.group(2).split() if "=" in kv)
    return out


def cursors(text):
    """`vis @label :cursor` -> {label: "1"|"0"} (1 = the mouse cursor shows, i.e. the pointer is free)"""
    out = {}
    for m in re.finditer(r"\[input_script\] vis t=\d+ @(\S+) :cursor=(\d)", text):
        out[m.group(1)] = m.group(2)
    return out


def main():
    if not os.path.exists(f"{BUILD}/client_bin"):
        print(f"no {BUILD}/client_bin")
        return 1
    os.makedirs(os.path.join(CFG, "krunknative"), exist_ok=True)

    # (label, events before the probe, ms to wait after them) -- a toggle, then a wait, then the probe.
    # "open?" says whether the console should be open at the probe (odd number of taps so far on the
    # relevant page). The invariant (open == shown, and a console-freed cursor is shown) is checked at
    # every label regardless.
    tap = ["down GRAVE", "up GRAVE"]
    steps = [
        ("menu_open", tap, 500, 1),           # the menu: ` opens it
        ("menu_closed", tap, 500, 0),          # ` again closes it
        ("game_pre", ["play"], 1500, 0),       # in game, closed
        ("game_open", tap, 500, 1),            # ` opens it in game (the menu opens for exitPointerLock)
        ("game_closed", tap, 500, 0),          # ` closes it, the pointer relocks
        ("rapid_even", tap + tap, 500, 0),     # two fast taps: still closed, still locked
        ("rapid_odd", tap + tap + tap, 600, 1),  # three fast taps: open and shown
        ("after_close", tap, 500, 0),          # back to closed
        ("dead", ["con kill", *tap], 1500, 1),  # ` right after death: open and shown
        ("respawn", ["respawn", *tap], 4000, 0),  # ` across respawn: a second tap closes it again
        ("map_switch", ["con map sandstorm", *tap], 1500, 1),  # ` during a map change: open and shown
        ("map_close", tap, 600, 0),
    ]
    lines, t = ["0 menu"], 200
    for label, events, wait, _open in steps:
        for e in events:
            lines.append(f"{t} {e}")
            t += 40
        t += wait
        lines += [f"{t} console @{label}", f"{t} vis @{label} :cursor", f"{t} state"]
        t += 150
    # the editor page round trip with the console left open: it must be shown on every page, never a
    # freed mouse with no console (con_live goes false while a page loads)
    lines += [f"{t} down GRAVE", f"{t + 40} up GRAVE"]          # open on the game page
    t += 300
    lines += [f"{t} console @before_editor", f"{t} vis @before_editor :cursor", f"{t} page editor"]
    t += 5000
    lines += [f"{t} console @on_editor", f"{t} vis @on_editor :cursor", f"{t} page game"]
    t += 6000
    lines += [f"{t} console @back_game", f"{t} vis @back_game :cursor", f"{t} state", f"{t + 200} quit"]

    text = run_client("race", "\n".join(lines) + "\n")
    p = probes(text)
    cur = cursors(text)

    labels = [s[0] for s in steps] + ["before_editor", "on_editor", "back_game"]
    want_open = {s[0]: s[3] for s in steps}
    want_open.update({"before_editor": 1, "on_editor": 1, "back_game": 1})

    for label in labels:
        pr = p.get(label)
        check(f"{label}: a probe ran", pr is not None, str(pr))
        if not pr:
            continue
        o, sh = pr.get("open"), pr.get("shown")
        # THE INVARIANT: open only when shown. Never a console the player cannot see.
        check(f"{label}: open implies shown (no freed mouse with no console)", o != "1" or sh == "1",
              f"open={o} shown={sh} built={pr.get('built')} page={pr.get('page')} top={pr.get('top')}")
        # and the toggle landed where expected (one key press = one toggle)
        check(f"{label}: ends {'open' if want_open[label] else 'closed'}", o == ("1" if want_open[label] else "0"),
              f"open={o} shown={sh}")
        # whenever the cursor is free for the console, the overlay is on screen
        if cur.get(label) == "1" and o == "1":
            check(f"{label}: the freed cursor shows the console", sh == "1", f"shown={sh}")

    # the pointer relocks when the console is closed in game (the cursor is hidden/disabled again)
    for label in ("game_closed", "rapid_even", "after_close", "map_close"):
        check(f"{label}: closed in game relocks the pointer", cur.get(label) == "0", f":cursor={cur.get(label)}")

    check("editor page: the console is on the editor page", p.get("on_editor", {}).get("page") == "editor.html",
          str(p.get("on_editor")))
    check("back on the game page", p.get("back_game", {}).get("page") == "index.html", str(p.get("back_game")))

    passed = sum(results)
    print(f"{passed}/{len(results)} passed")
    if passed == len(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())

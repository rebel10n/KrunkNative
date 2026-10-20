#!/usr/bin/env python3
"""Developer console autofill for `give` (client/src/ui/ui_console.c give_candidates, shared/src/chat_commands.c cmd_give).

  python3 -I client/tests/console_give_autofill_e2e.py BUILD_DIR      (run from the repository root)

The bug: the give line's hints disagreed with each other. `/give *` offered no "*" (the first word took
only items); `/give * ` offered the player names where the item list belongs (and `/give * s` nothing);
`/give tool g` offered players (a multi-word item had no list); `/give krt` offered nothing although the
lookup takes "krtag"; `/give tool gun ` offered nothing for the receiver. The item list after a selector
is now the same list the no-selector form gives, and the receiver list follows a complete item.

An offline Kanji client (the sandbox: one player, Guest_1, who may give to anyone) runs a headless input
script. The console is opened with the ` key, then each line is typed in place (CTRL+A, then the text; a
space is a SPACE key, as the script's `type` trims a trailing space). After each line the visible hint rows
are read (`text @label .hints>.hintItem:nth-child(n)`, the signature row is row 1). Checks:
  1. each line's rows equal the expected list (exact names, in list order);
  2. the same item filter gives the same rows with and without a selector (`/give s` = `/give * s`), and
     the same text twice gives the same rows (`/give ` = `/give * ` = the second `/give * `);
  3. Tab on a single match completes it, quoted, with a trailing space (`/give * tool g` -> `/give * "Tool Gun" `).
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
WORK = tempfile.mkdtemp(prefix="kn-console-give-", dir=CACHE if os.path.isdir(CACHE) else None)
PLAYER = "Guest_1"   # the sandbox's own player: the only receiver offline
ALL = "*"
results = []

# (key, typed line, expected rows): rows are the candidate names in list order (the signature row excluded);
# None: not checked here (the list is compared with another line's below)
LINES = [
    ("plain", "/give ", ["Sniper Rifle", "Assault Rifle", "Pistol", "Submachine Gun", "Revolver", "Shotgun",
                         "Machine Gun", "Semi Auto", "Rocket Launcher", "Akimbo Uzi", "Desert Eagle"]),
    ("star", "/give *", [ALL]),
    ("star-space", "/give * ", "same-as:plain"),
    ("plain-s", "/give s", None),
    ("star-s", "/give * s", "same-as:plain-s"),
    ("star-item-recv", "/give * sniper ", [PLAYER]),
    ("star-item-star", "/give * sniper *", [ALL]),
    ("item-recv", "/give sniper ", [PLAYER]),
    ("item-star", "/give sniper *", [ALL]),
    ("prefix", "/give sn", ["Sniper Rifle"]),
    ("multi", "/give tool g", ["Tool Gun"]),
    ("star-multi", "/give * tool g", ["Tool Gun"]),
    ("alias-recv", "/give tool ", [PLAYER]),
    ("normalised", "/give krt", ["KR Tag"]),
    ("multi-recv", "/give tool gun ", [PLAYER]),
    ("star-again", "/give * ", "same-as:plain"),
]
TABS = [
    ("tab-multi", "/give * tool g", '/give * "Tool Gun" '),
    ("tab-prefix", "/give sn", '/give "Sniper Rifle" '),
]


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def typing(text):
    """The script lines that type `text` in the focused input: a space is a SPACE key (see the note above)."""
    lines = []
    parts = text.split(" ")
    for j, part in enumerate(parts):
        if j:
            lines.append("down SPACE\nup SPACE")
        if part:
            lines.append(f"type {part}")
    return lines


def build_script():
    out = ["0 play", "200 down GRAVE", "220 up GRAVE"]
    t = 400
    for key, text, _ in LINES:
        out.append(f"{t} down CTRL+A"); out.append(f"{t + 10} up CTRL+A"); t += 40
        for line in typing(text):
            for part in line.split("\n"):
                out.append(f"{t} {part}"); t += 12
        t += 60
        for n in range(1, 13):  # row 1 is the signature, then up to 11 candidates
            out.append(f"{t} text @row_{key}_{n} .hints>.hintItem:nth-child({n})"); t += 6
        t += 20
    for key, text, _ in TABS:
        out.append(f"{t} down CTRL+A"); out.append(f"{t + 10} up CTRL+A"); t += 40
        for line in typing(text):
            for part in line.split("\n"):
                out.append(f"{t} {part}"); t += 12
        t += 40
        out.append(f"{t} down TAB"); out.append(f"{t + 10} up TAB"); t += 60
        out.append(f"{t} text @tab_{key} .row input"); t += 30
    out.append(f"{t} quit")
    return "\n".join(out) + "\n"


def run(timeout=300):
    script = os.path.join(WORK, "give.script")
    with open(script, "w") as f:
        f.write(build_script())
    cfg = os.path.join(WORK, "cfg")
    state = os.path.join(WORK, "state")
    os.makedirs(cfg, exist_ok=True)
    os.makedirs(state, exist_ok=True)
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": state,
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": script,
                "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/client_bin", "--offline", "assets/maps/official/10_kanji.json"], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check("client_bin finished its script", False, f"timeout, log {log}")
    with open(log, errors="replace") as f:
        return f.read()


ROW = re.compile(r"\[input_script\] text t=\d+ @row_(\S+?)_(\d+) n=(\d+) checked=\d value=.*? text=(.*)$")
TAB = re.compile(r"\[input_script\] text t=\d+ @tab_(\S+) n=(\d+) checked=\d value=(.*) text=")


def read_rows(text):
    """{line key: (signature row, candidate rows)} and {tab key: the input's value after Tab}."""
    rows, tabs = {}, {}
    for line in text.splitlines():
        m = ROW.search(line)
        if m:
            key, n, count, shown = m.group(1), int(m.group(2)), int(m.group(3)), m.group(4).strip()
            if count:
                rows.setdefault(key, {})[n] = shown
            continue
        m = TAB.search(line)
        if m:
            tabs[m.group(1)] = m.group(3)
    out = {}
    for key, by_n in rows.items():
        # row 1 is the command's signature; the candidates are rows 2.. up to the first missing one
        names = []
        for n in range(2, 13):
            if n not in by_n:
                break
            names.append(by_n[n])
        out[key] = (by_n.get(1, ""), names)
    return out, tabs


def main():
    text = run()
    check("client_bin ran the script", "[input_script] " in text and "events from" in text, "")
    rows, tabs = read_rows(text)
    expected = {key: (typed, exp) for key, typed, exp in LINES}
    got = {}
    for key, typed, exp in LINES:
        if key not in rows:
            check(f"{typed!r}: the hint rows were read", False, "no rows (the console did not open?)")
            continue
        sig, names = rows[key]
        got[key] = names
        if typed.startswith("/give ") and not sig.startswith("give weapon|item"):
            check(f"{typed!r}: the signature row is the command's", False, f"got {sig!r}")
        if isinstance(exp, list):
            check(f"{typed!r} lists {exp}", names == exp, f"got {names}")
        elif isinstance(exp, str) and exp.startswith("same-as:"):
            ref = exp.split(":", 1)[1]
            check(f"{typed!r} lists the same as {expected[ref][0]!r}", names == got.get(ref) and names,
                  f"got {names}, {expected[ref][0]!r} gave {got.get(ref)}")
    if "star-s" in got:
        check("'/give * s' gives items only (no receiver)", PLAYER not in got["star-s"], f"got {got['star-s']}")
    for key, typed, shown_want in TABS:
        shown = tabs.get(key)
        check(f"Tab completes {typed!r} to {shown_want!r}", shown == shown_want, f"value {shown!r}")


try:
    main()
except Exception as e:  # noqa: BLE001
    check("exception", False, repr(e))
finally:
    if results and all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}", flush=True)
print(f"{sum(results)}/{len(results)} passed", flush=True)
sys.exit(0 if results and all(results) else 1)

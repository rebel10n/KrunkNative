#!/usr/bin/env python3
"""The bhop mode is one setting: Settings > Game > Gameplay > Bhop Mode (client/src/ui/ui_bhop.c).

  python3 -I client/tests/bhop_setting_e2e.py BUILD_DIR      (run from the repository root)

Headless client_bin runs (offline Kanji, input scripts, private XDG_CONFIG_HOME). The mode is
knSettings "bhopMode" (a number; absent = Disabled):
  1. a fresh profile: the Bhop Mode row shows in the Game tab with the five modes, Disabled
     selected; walking forward (W) in Disabled makes no jump.
  2. choosing Always Jump in the row stores 3 at once, the row shows it, and walking forward jumps
     (the movement reads the setting); the value is in knSettings.json.
  3. a restart keeps 3: the row shows it, the movement still jumps; the bhop key (B, the mods bind)
     moves the mode to Hold Jump ("Bhop - Hold Jump" in the log), and the row shows 5 after the
     in-game menu opens again; knSettings.json holds 5.
  4. a restart keeps 5: the row shows Hold Jump, and walking forward without Space makes no jump.
  5. a v3.1.1 localStorage "bhop" (2 = Hold Jump/Crouch) with no knSettings bhopMode migrates once:
     the row shows Hold Jump/Crouch, knSettings holds 2 and the old key is gone.
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
WORK = tempfile.mkdtemp(prefix="kn-bhop-setting-", dir=CACHE if os.path.isdir(CACHE) else None)
CFG = os.path.join(WORK, "cfg")
STORE = os.path.join(CFG, "krunknative")
# no '#' in selectors (the script reads it as a comment outside quotes)
ROW = "[data-bhop-mode-setting]"
SELECT = ROW + " select"
OPTIONS = ROW + " option"
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def stored_file(key):
    try:
        with open(os.path.join(STORE, key + ".json")) as f:
            return f.read()
    except OSError:
        return None


def settings():
    try:
        with open(os.path.join(STORE, "knSettings.json")) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def stored_mode():
    return settings().get("bhopMode")


def rows(path):
    """The movement log (KRUNKNATIVE_INPUT_LOG): one dict per frame after `play`."""
    out = []
    try:
        with open(path) as f:
            head = f.readline().strip().split(",")
            for line in f:
                cells = line.strip().split(",")
                if len(cells) == len(head):
                    out.append({k: float(v) for k, v in zip(head, cells)})
    except OSError:
        pass
    return out


def jumps(frames):
    """Frames in the air with an upward speed: a jump's take-off."""
    return sum(1 for r in frames if r["vy"] > 0.01 and not r["on_ground"])


def moved(frames):
    return sum(1 for r in frames if abs(r["vx"]) + abs(r["vz"]) > 0.01)


def text_value(text, label):
    """The value the page's <select> holds, from the script's `text` line (None if not logged)."""
    m = re.search(rf"\[input_script\] text t=\d+ {re.escape(label)} n=(\d+) checked=\d+ value=(\S*)", text)
    return (int(m.group(1)), m.group(2)) if m else None


def grep_hits(text, label):
    m = re.search(rf"\[input_script\] grep t=\d+ {re.escape(label)} needle=\S+ n=\d+ hits=(\d+)", text)
    return int(m.group(1)) if m else None


def run(name, script, timeout=240):
    path = os.path.join(WORK, f"{name}.script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, f"{name}.log")
    csv = os.path.join(WORK, "last-input.csv")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": CFG, "XDG_STATE_HOME": os.path.join(WORK, "state"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_INPUT_LOG": csv, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
    if os.path.exists(csv):
        os.remove(csv)
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
    nodes = {m.group(1): int(m.group(2)) for m in re.finditer(r"\[input_script\] nodes t=\d+ (@\S+) elements=\d+ match=(\d+)", text)}
    return nodes, text, rows(csv)


def reset_profile(files):
    """A private profile: krk_advanced 0 (the basic Settings view), and the given key -> raw text files."""
    shutil.rmtree(CFG, ignore_errors=True)
    os.makedirs(STORE)
    with open(os.path.join(STORE, "krk_advanced.json"), "w") as f:
        f.write("0")
    for key, text in files.items():
        with open(os.path.join(STORE, key + ".json"), "w") as f:
            f.write(text)


def open_game_tab(t):
    """Settings (basic view) on the Game tab (index 2), the Bhop Mode row inserted at its top."""
    return (f"{t} call showWindow 1\n{t + 200} call windows.0.changeTab 2\n"
            f"{t + 600} nodes @row {SELECT}\n{t + 650} nodes @opts {OPTIONS}\n{t + 700} text @sel {SELECT}\n")


def play_walk(t):
    """Close the menu, enter the game and walk forward for about two seconds, then quit."""
    return f"{t} call closWind\n{t + 300} play\n{t + 900} down W\n{t + 2900} up W\n{t + 3100} quit\n"


def main():
    # 1. a fresh profile: the row with five modes (the labels), Disabled, and no jump while walking
    reset_profile({})
    labels = {"@g_dis": ("Disabled", 1), "@g_slide": ("Slidehop", 1), "@g_crouch": ("Crouch", 1),
              "@g_always": ("Always", 1), "@g_hold": ("Hold", 2)}
    grep_lines = "".join(f"{1210 + 10 * i} grep {label} {needle} {OPTIONS}\n" for i, (label, (needle, _)) in enumerate(labels.items()))
    n, text, frames = run("fresh", "0 menu\n" + open_game_tab(500) + grep_lines + play_walk(1500))
    check("the Game tab shows the Bhop Mode row (one select)", n.get("@row") == 1, str(n))
    check("the row has the five modes (options)", n.get("@opts") == 5, str(n))
    for label, (needle, want) in labels.items():
        check(f"the row has the option {needle}", grep_hits(text, label) == want, f"hits {grep_hits(text, label)}, want {want}")
    check("a fresh profile shows Disabled", text_value(text, "@sel") == (1, "0"), str(text_value(text, "@sel")))
    check("walking forward in Disabled moves the player", moved(frames) > 0, f"moved frames {moved(frames)}")
    check("walking forward in Disabled makes no jump", jumps(frames) == 0, f"jumps {jumps(frames)}")
    check("a fresh profile stores no bhopMode", stored_mode() is None, str(settings()))

    # 2. choosing Always Jump stores 3; the movement jumps while walking; the file holds it
    reset_profile({})
    n, text, frames = run("set", "0 menu\n" + open_game_tab(500) +
                          '1400 call setSetting "bhopMode" 3\n1700 text @sel2 ' + SELECT + "\n" + play_walk(1800))
    check("choosing Always Jump shows in the row", text_value(text, "@sel2") == (1, "3"), str(text_value(text, "@sel2")))
    check("choosing it stores bhopMode 3", stored_mode() == 3, str(settings()))
    check("walking forward in Always Jump jumps (the movement reads the setting)", jumps(frames) > 0,
          f"jumps {jumps(frames)} moved {moved(frames)}")

    # 3. a restart keeps 3 (the row and the movement); the bhop key (B) moves it to Hold Jump
    n, text, frames = run("restart3", "0 menu\n" + open_game_tab(500) + play_walk(1500))
    check("a restart shows Always Jump in the row", text_value(text, "@sel") == (1, "3"), str(text_value(text, "@sel")))
    check("a restart: walking forward still jumps", jumps(frames) > 0, f"jumps {jumps(frames)}")

    n, text, frames = run("bhop-key", "0 menu\n" + open_game_tab(500) +
                          "1400 call closWind\n1800 play\n2200 down B\n2250 up B\n2600 down ESC\n2650 up ESC\n" +
                          "2900 call showWindow 1\n3100 call windows.0.changeTab 2\n3300 text @sel3 " + SELECT +
                          "\n3400 quit\n")
    check("the bhop key logs the toggle (Bhop - Hold Jump)", "Bhop - Hold Jump" in text)
    check("the bhop key moves the Settings row to Hold Jump", text_value(text, "@sel3") == (1, "5"),
          str(text_value(text, "@sel3")))
    check("the bhop key stores bhopMode 5", stored_mode() == 5, str(settings()))

    # 4. a restart keeps 5: Hold Jump in the row; no Space, so walking does not jump
    n, text, frames = run("restart5", "0 menu\n" + open_game_tab(500) + play_walk(1500))
    check("a restart shows Hold Jump in the row", text_value(text, "@sel") == (1, "5"), str(text_value(text, "@sel")))
    check("a restart in Hold Jump: walking without Space does not jump", jumps(frames) == 0, f"jumps {jumps(frames)}")

    # 5. the v3.1.1 localStorage "bhop" key migrates once
    reset_profile({"bhop": "2"})
    n, text, frames = run("migrate", "0 menu\n" + open_game_tab(500) + "1400 quit\n")
    check("a stored legacy bhop 2 shows Hold Jump/Crouch", text_value(text, "@sel") == (1, "2"), str(text_value(text, "@sel")))
    check("the legacy bhop 2 is stored as bhopMode 2", stored_mode() == 2, str(settings()))
    check("the legacy bhop key is removed", stored_file("bhop") is None)

    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

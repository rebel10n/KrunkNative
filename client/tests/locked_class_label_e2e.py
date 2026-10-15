#!/usr/bin/env python3
"""The locked-class label in the Change Loadout class tab shows its full text (client/src/ui/ui_loadout.c
gen_tab_class: `.lockedClass > .lockedClassText`; v10's CSS sets `text-wrap: auto` on it inside a nowrap
.classCard, which the engine honours through client/src/html/engine css_values.c).

  python3 -I client/tests/locked_class_label_e2e.py BUILD_DIR      (run from the repository root)

An official krunker.io lobby (state.official: the flag krunker_net.c sets on join) locks the Nettspend's
card with "Not on official servers". One headless render of the class tab (client_bin --render-scenario, no
window): KRUNKNATIVE_SCENARIO_DUMP prints the label's box tree after the capture, the label box's width
is the space its text may fill, and each text box under it must fit inside it. A clipped, unwrapped line
(the pre-fix engine: a 152px text box in an 80px label) fails.

KRUNKNATIVE_UI_DIR=<copy of ui/ without the .lockedClassText text-wrap line> renders the pre-fix page
for comparison (the check then fails). A second render without state.official (a KrunkNative lobby) must show
no lock on the Nettspend's card. Exit 0 when the label fits and the lock is official-only, 1 otherwise.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.getcwd()
# the worktree lacks the untracked assets/ (fonts): KRUNKNATIVE_ASSETS_DIR points at the checkout's
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or os.path.join(ROOT, "assets")
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
CLIENT = os.path.join(BUILD, "client_bin")
BOX = re.compile(r"local\(([-\d.]+),([-\d.]+) ([\d.]+)x([\d.]+)\)")
WORK = tempfile.mkdtemp(prefix="kn-class-label-", dir=os.path.expanduser("~/.cache") if os.path.isdir(os.path.expanduser("~/.cache")) else None)
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def scenario(name, official):
    path = os.path.join(WORK, name + ".json")
    state = {
        "menu": True, "connected": True, "ready": True, "inGame": False, "name": "Guest", "classIndex": 0,
        "region": "...", "mapName": "Kanji", "mapFile": "maps/311_kanji.json", "gameName": "ffa on Kanji",
        "anim_time": 0.8, "window": 3, "windowActions": [["windows.2.switchTab", 0]],
    }
    if official:
        state["official"] = True
    with open(path, "w") as f:
        json.dump({"size": [1280, 720], "background": "#000000", "state": state, "frames": 3}, f)
    return path


def render(name, official, dump):
    """The label's box tree after the capture: [(label_w, [text_box_w...], text_lines)]."""
    scen = scenario(name, official)
    out = os.path.join(WORK, name + ".png")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
        "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
        "KRUNKNATIVE_SCENARIO_DUMP": dump, "KRUNKNATIVE_ASSETS_DIR": ASSETS,
    })
    proc = subprocess.run([CLIENT, "--render-scenario", scen, out], env=env, cwd=ROOT, capture_output=True, text=True, timeout=300)
    lines = proc.stderr.splitlines()
    if proc.returncode != 0 or not os.path.exists(out):
        return None, lines
    # the first line naming the label is its element box; the lines indented under it are its boxes
    label, texts = None, []
    for i, line in enumerate(lines):
        if "lockedClassText" not in line:
            continue
        m = BOX.search(line)
        if not m:
            continue
        label = float(m.group(3))
        depth = len(line) - len(line.lstrip(" "))
        for child in lines[i + 1:]:
            if not child.strip() or len(child) - len(child.lstrip(" ")) <= depth:
                break
            tm = BOX.search(child)
            if child.strip().startswith("text") and tm:
                texts.append((float(tm.group(3)), child.strip()))
        break
    return (label, texts), lines


def main():
    if not os.path.exists(CLIENT):
        print(f"no {CLIENT}: build client_bin first")
        return 1
    if not os.path.isdir(ASSETS):
        print("no assets directory (KRUNKNATIVE_ASSETS_DIR): run from the repository root")
        return 1
    cases = (("official", True, ".classCard:last-child .lockedClassText", "Not on official servers"),)
    for name, official, dump, want in cases:
        got, lines = render(name, official, dump)
        if got is None:
            check(f"{name}: the class tab renders", False, "client_bin --render-scenario failed")
            print("\n".join(lines[-12:]))
            continue
        label, texts = got
        check(f"{name}: the Nettspend's label box is dumped", label is not None and label > 0, f"label width {label}")
        check(f"{name}: its text is in the label", any(want in t for _, t in texts), "; ".join(t for _, t in texts) or "no text box")
        worst = max((w for w, _ in texts), default=0.0)
        shown = "n/a" if label is None else f"{label:.1f}px"
        check(f"{name}: the text fits its label ({shown} label, widest text box {worst:.1f}px)",
              label is not None and bool(texts) and worst <= label + 0.5)
    # a KrunkNative lobby (no state.official): the Nettspend's card has no lock (game_class_allowed)
    got, lines = render("krunknative", False, ".classCard:last-child .lockedClassText")
    check("krunknative: the class tab renders", got is not None, "" if got is not None else "client_bin --render-scenario failed")
    if got is not None:
        check("krunknative: the Nettspend's card is not locked", got[0] is None, f"label width {got[0]}")
    shutil.rmtree(WORK, ignore_errors=True)
    failed = len(results) - sum(results)
    print(f"locked_class_label_e2e: {len(results) - failed}/{len(results)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

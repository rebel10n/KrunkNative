#!/usr/bin/env python3
"""The capped Leaderboard list of a guest's end card scrolls under the mouse wheel (client/src/ui/hud_v10.c,
hud_v10_end_signup_place: #endLayoutHolder, .tabBody with overflow-y auto, gets a max-height when the signup banner
would not fit; commit 19886bcd).

  python3 -I client/tests/leaderboard_scroll_e2e.py BUILD_DIR      (run from the repository root)

Headless --render-scenario (state from tools/uidiff/scenarios/ingame-endscreen-late.json: a guest's Defeat, 16 players
on the Leaderboard tab, three vote cards, so the list is capped), driven by an input script that goes through the
engine's wheel path (html_ui_scroll -> events_scroll at the pointer; input_script.c `cursorid` + `wheel`):
  1. the list is capped: its box ends above the Double XP button, and it is a scroll container (scrollTop reads);
  2. before the wheel the list is at its top and its first visible row is row 1 (Local User);
  3. three wheel clicks over the list (down, 53 px each) move its scrollTop to 159 and its first visible row to row 2;
  4. three wheel clicks up bring it back to the top, row 1 first again.
Exit 0 when every check passes, 1 otherwise.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.getcwd()
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or os.path.join(ROOT, "assets")
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
CLIENT = os.path.join(BUILD, "client_bin")
SCENARIO = os.path.join(ROOT, "tools", "uidiff", "scenarios", "ingame-endscreen-late.json")
PX_PER_CLICK = 53.0  # events.c EV_SCROLL_PIXELS_PER_CLICK, in page px per wheel click at the page's scale
LONG_BOARD = ["Local User", "Vortex", "NovaBlade", "KrunkLord", "Sniperino", "Ghostwire", "PixelPanda", "BoltKnight",
              "Mavericks", "Quarry", "Hexagon", "Tundra", "RazorFox", "Lumen", "Ember", "Juniper"]
VOTE_MAPS = [{"mode": 0, "map": 0, "votes": 0, "name": "Lagoon"}, {"mode": 0, "map": 1, "votes": 0, "name": "Tropicana"},
             {"mode": 1, "map": 2, "votes": 0, "name": "Freight"}]
WORK = tempfile.mkdtemp(prefix="kn-lb-scroll-", dir=os.path.expanduser("~/.cache") if os.path.isdir(os.path.expanduser("~/.cache")) else None)
results = []

# The input script: the probes sit at the times the scenario reaches them (the card is up by ~100 ms); the wheel
# events go between the probes. `cursorid` puts the pointer at the middle of the list. A '#' starts a comment in the
# script, so the selectors are classes or ids read with rect / style / scroll.
SCRIPT = """\
100 rects @rows0 .endTableN
100 rect @box endLayoutHolder endUI doubleXPButton
100 style @cap endLayoutHolder:max-height
110 scroll @s0 endLayoutHolder
120 cursorid endLayoutHolder 0.5 0.5
130 wheel -1
140 wheel -1
150 wheel -1
160 scroll @s1 endLayoutHolder
170 rects @rows1 .endTableN
180 wheel 1
190 wheel 1
200 wheel 1
210 scroll @s2 endLayoutHolder
220 rects @rows2 .endTableN
"""


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def scenario_path():
    with open(SCENARIO, encoding="utf-8") as f:
        doc = json.load(f)
    st = doc["state"]
    st["endscreen"] = {"place": 9, "result": 0, "text": "#9 DEFEAT", "nextRound": "00:17", "results": 1}
    st["endTab"] = "leaderboard"
    st["scoreboard"] = [{"name": n, "score": 1200 - 60 * i, "kills": 14 - i % 9, "deaths": 9 + i % 5, **({"me": True} if i == 0 else {})}
                        for i, n in enumerate(LONG_BOARD)]
    st["matchVotes"] = VOTE_MAPS
    path = os.path.join(WORK, "long-board.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f)
    return path


def run_scenario():
    script = os.path.join(WORK, "wheel.script")
    with open(script, "w", encoding="utf-8") as f:
        f.write(SCRIPT)
    out = os.path.join(WORK, "list.png")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
        "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
        "KRUNKNATIVE_ASSETS_DIR": ASSETS, "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667",
    })
    proc = subprocess.run([CLIENT, "--render-scenario", scenario_path(), out], env=env, cwd=ROOT, capture_output=True,
                          text=True, timeout=300)
    return proc.returncode, (proc.stderr + proc.stdout).splitlines()


KV = re.compile(r"(\S+?)=(\S+)")


def rect_of(lines, label, id_):
    """the box (window px: x, y, w, h) of a `rect @label <id>` probe, or None"""
    for line in lines:
        if re.match(r"\[input_script\] rect t=\S+ @%s " % re.escape(label), line):
            for key, val in KV.findall(line.split(" @%s " % label, 1)[1]):
                if key == id_ and val != "-":
                    return tuple(float(v) for v in val.split(",")[:4])
    return None


def scroll_top(lines, label, id_):
    """the scrollTop of a `scroll @label <id>` probe (None: not a scroll container or no such id)"""
    for line in lines:
        m = re.match(r"\[input_script\] scroll t=\S+ @%s %s=(\S+)$" % (re.escape(label), re.escape(id_)), line)
        if m:
            return None if m.group(1) == "-" else float(m.group(1))
    return None


def row_boxes(lines, label):
    """[(x, y, w, h)] of the rows of a `rects @label .endTableN` probe, in document order"""
    for line in lines:
        m = re.match(r"\[input_script\] rects t=\S+ @%s n=(\d+)(.*)$" % re.escape(label), line)
        if m:
            rows = []
            for tok in m.group(2).split():
                _, _, box = tok.partition("@")
                rows.append(tuple(float(v) for v in box.split(",")))
            return rows
    return []


def first_visible(rows, box):
    """the index of the first row whose box lies inside the list's box (None: no row shows)"""
    if box is None:
        return None
    top, bottom = box[1], box[1] + box[3]
    for i, (_, y, _, h) in enumerate(rows):
        if y >= top - 0.5 and y + h <= bottom + 0.5:
            return i
    return None


def main():
    if not os.path.exists(CLIENT):
        print(f"client_bin not found: {CLIENT}")
        return 1
    code, lines = run_scenario()
    if code != 0 or not any(line.startswith("[scenario] 1280x720") for line in lines):
        print("\n".join(lines[-20:]))
        check("the end card scenario renders", False, f"exit {code}")
        return 1
    check("the end card scenario renders", True)

    box = rect_of(lines, "box", "endLayoutHolder")
    button = rect_of(lines, "box", "doubleXPButton")
    capped = any(re.match(r"\[input_script\] style t=\S+ @cap endLayoutHolder:max-height=(\S+)$", line) and
                 "=-" not in line for line in lines)
    check("the list is capped: its box ends above the Double XP button",
          box is not None and button is not None and capped and box[1] + box[3] <= button[1] + 0.5,
          f"list {box}, button {button}" if box and button else "no box")

    s0, s1, s2 = (scroll_top(lines, k, "endLayoutHolder") for k in ("s0", "s1", "s2"))
    check("the list is a scroll container at its top before the wheel", s0 == 0.0, f"scrollTop {s0}")

    before, after = row_boxes(lines, "rows0"), row_boxes(lines, "rows1")
    first0, first1 = first_visible(before, box), first_visible(after, box)
    names = LONG_BOARD
    check("before the wheel the first visible row is row 1 (Local User)",
          first0 == 0 and len(before) == len(names), f"first visible row index {first0} of {len(before)}")

    check("three wheel clicks over the list scroll it down 3 x 53 px",
          s1 is not None and abs(s1 - 3 * PX_PER_CLICK) < 0.5, f"scrollTop {s1}")
    check("the wheel moves the list's first visible row (row 1 -> row 2, Vortex)",
          first1 is not None and first1 > first0 and first1 == 1 and names[first1] == "Vortex",
          f"first visible row index {first0} -> {first1}" + (f" ({names[first1]})" if first1 is not None else ""))

    rows_back = row_boxes(lines, "rows2")
    first2 = first_visible(rows_back, box)
    check("three wheel clicks up bring the list back to its top (row 1 first again)",
          s2 == 0.0 and first2 == 0, f"scrollTop {s2}, first visible row index {first2}")

    print(f"{sum(results)}/{len(results)} checks passed  (work dir {WORK})")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

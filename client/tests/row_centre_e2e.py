#!/usr/bin/env python3
"""The action buttons of a list row sit on the row's vertical middle: the Player List's rows (hud_ui.c:
the mute / K.P.D / Votekick / staff buttons of .pListActions, ui/css/kn-v10-menu.css) and the Account
Manager's rows (alt_manager_ui.c: Use / Remove of .knAltRow, ui/css/kn-alt.css).

  python3 -I client/tests/row_centre_e2e.py BUILD_DIR   (repository root)

Each scenario renders headless with client_bin --render-scenario; KRUNKNATIVE_SCENARIO_DUMP prints the
captured frame's box tree of the list (ui_scenario.c, engine_dump_boxes), in viewport px (vp()). A Player
List row's middle is the middle of its visible band: the row box minus the 3 CSS px separator under it
(main.css `th, tr`), so the band is [top, bottom - 3 CSS px). An alt row has no separator: its middle is its
box's middle. Every button's centre must be within 1 px of its row's middle.

  - ingame-playerlist.json: the Player List of three account players (chat / voice / K.P.D / Votekick)
  - account-manager-rows.json: two alt accounts (Use / Remove)

Exit 0 when every button is within 1 px, 77 when client_bin is missing, 1 otherwise.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.getcwd()
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
TOLERANCE_PX = 1.0
SEPARATOR_CSS_PX = 3.0  # main.css `th, tr { border-bottom: 3px solid ... }`

# (scenario, dump selector, is a table row, row class, button classes, minimum buttons measured)
SCENARIOS = [
    ("tools/uidiff/scenarios/ingame-playerlist.json", ".pListTable", True, "tr", ("punishButton", "muteUserBtn"), 12),
    ("tools/uidiff/scenarios/account-manager-rows.json", ".knAltList", False, "knAltRow", ("knAltBtn",), 4),
]
# a dump line: "<indent><kind> <tag or (anon)>  local(x,y wxh) vp(x,y wxh)"; a text run has no vp()
BOX = re.compile(r"^(?P<ind> *)(?P<kind>\w+) (?:\(anon\)|<(?P<tag>[^>]*)>)\s+local\([^)]*\)\s+vp\("
                 r"(?P<x>[-\d.]+),(?P<y>[-\d.]+) (?P<w>[\d.]+)x(?P<h>[\d.]+)\)")
LOCAL = re.compile(r"local\([^)]*? [\d.]+x(?P<h>[\d.]+)\)")


def dump_boxes(build, scenario, selector, work, label):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(work, label + "-cfg"),
                "KRUNKNATIVE_SCENARIO_DUMP": selector})
    p = subprocess.run([os.path.join(build, "client_bin"), "--render-scenario", os.path.join(ROOT, scenario),
                        os.path.join(work, label + ".png")], env=env, cwd=ROOT, capture_output=True, text=True, timeout=300)
    if p.returncode != 0 or "no match" in p.stderr:
        raise RuntimeError("%s: render failed (rc %d): %s" % (label, p.returncode, (p.stderr or p.stdout)[-800:]))
    return p.stderr.splitlines()


def rows_with_buttons(lines, is_table_row, row_class, button_classes):
    """[(row box, [button box, ...]), ...] from the dump: a row's buttons are the boxes nested under it."""
    rows = []
    row = None
    for line in lines:
        m = BOX.match(line)
        if not m:
            continue
        ind = len(m.group("ind"))
        kind, tag = m.group("kind"), m.group("tag") or ""
        if row is not None and ind <= row["ind"]:
            row = None
        is_row = (kind == "row" and tag == "tr") if is_table_row else (kind == "grid" and ("." + row_class) in tag)
        if is_row:
            vp_h = float(m.group("h"))
            local = LOCAL.search(line)
            local_h = float(local.group("h")) if local else vp_h
            row = {"y": float(m.group("y")), "h": vp_h, "ind": ind, "scale": vp_h / local_h if local_h else 1.0,
                   "tag": tag, "buttons": []}
            rows.append(row)
            continue
        if row is not None and any(("." + c) in tag for c in button_classes):
            row["buttons"].append({"tag": tag, "y": float(m.group("y")), "h": float(m.group("h"))})
    return rows


def row_middle(row, is_table_row):
    """The middle of the row's visible band: a table row's separator (3 CSS px, scaled to vp) is not part of it."""
    sep = SEPARATOR_CSS_PX * row["scale"] if is_table_row else 0.0
    return row["y"] + (row["h"] - sep) / 2.0


def check_scenario(build, scenario, selector, is_table_row, row_class, button_classes, min_buttons, work):
    label = os.path.splitext(os.path.basename(scenario))[0]
    rows = rows_with_buttons(dump_boxes(build, scenario, selector, work, label), is_table_row, row_class, button_classes)
    failures = []
    offsets = []
    for i, row in enumerate(rows):
        if not row["buttons"]:
            continue
        mid = row_middle(row, is_table_row)
        for b in row["buttons"]:
            off = (b["y"] + b["h"] / 2.0) - mid
            offsets.append(abs(off))
            print("  %-22s row %d  %-28s centre %+.2f px from the row middle (row %.1f..%.1f)"
                  % (label, i, b["tag"], off, row["y"], row["y"] + row["h"]))
            if abs(off) > TOLERANCE_PX:
                failures.append("%s row %d %s: %+.2f px from the row middle" % (label, i, b["tag"], off))
    if len(offsets) < min_buttons:
        failures.append("%s: only %d buttons measured (want >= %d)" % (label, len(offsets), min_buttons))
    return len(offsets), (max(offsets) if offsets else float("nan")), failures


def main():
    if not os.path.isfile(os.path.join(BUILD, "client_bin")):
        print("row_centre_e2e: needs client_bin: skipped")
        return 77
    work = tempfile.mkdtemp(prefix="kn-row-centre-e2e-")
    failures = []
    for scenario, selector, is_table_row, row_class, button_classes, min_buttons in SCENARIOS:
        n, worst, bad = check_scenario(BUILD, scenario, selector, is_table_row, row_class, button_classes, min_buttons, work)
        print("%s: %d buttons, worst %.2f px (tolerance %.1f px)" % (os.path.basename(scenario), n, worst, TOLERANCE_PX))
        failures += bad
    for f in failures:
        print("FAIL " + f)
    if not failures:
        shutil.rmtree(work, ignore_errors=True)
        print("PASS every action button is within %.1f px of its row's middle" % TOLERANCE_PX)
        return 0
    print("logs kept in %s" % work)
    return 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""The elements of a list row sit where they belong vertically.

The Player List's rows (hud_ui.c) are laid out by v10's own CSS (main.css: the inline-block .pListName
cell, the floated .pListActions cell, the buttons' negative margins), which puts the Focus Player eye, the
ping, the name, the chat / voice mutes and the K.P.D / Votekick / staff buttons on one line a few px above
the row's geometric middle. The target is Krunker v10's capture, element by element: each element's ink
(its glyphs, in its own column of the row's band) must have the same vertical centre and extent as in the
reference capture, within a px of sub-pixel rounding. The Account Manager's rows (alt_manager_ui.c: Use /
Remove of .knAltRow, ui/css/kn-alt.css) have no v10 counterpart: their buttons sit on the row's middle.

  python3 -I client/tests/row_centre_e2e.py BUILD_DIR   (repository root)

Each scenario renders headless with client_bin --render-scenario; KRUNKNATIVE_SCENARIO_DUMP prints the
captured frame's box tree of the list (ui_scenario.c, engine_dump_boxes), in viewport px (vp()). A Player
List row's band is its box minus the 3 CSS px separator under it (main.css `th, tr`); the reference's rows
sit ref_offset higher (tools/uidiff/regions/<scenario>.json, player-list-rows: v10's sandbox adds a block
under the list that moves its window). An alt row has no separator: its middle is its box's middle.

  - ingame-playerlist.json: the Player List of three account players (ping / name / chat / voice / K.P.D /
    Votekick) against doc/ref/krunker-v10/ingame/ingame-playerlist.png
  - ingame-playerlist-spectate.json: the same while spectating (Focus Player eye, Clear Focus) against
    ingame-playerlist-spectate.png
  - account-manager-rows.json: two alt accounts (Use / Remove), centred on their row

Exit 0 when every element is in place, 77 when client_bin is missing, 1 otherwise.
"""
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = os.getcwd()
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
TOLERANCE_PX = 1.0         # an alt row's button centre from its row's middle
PARITY_CENTRE_PX = 1.5     # an element's ink centre from the reference's: sub-pixel rows round either way
PARITY_EXTENT_PX = 2.0     # its ink's top / bottom from the reference's
SEPARATOR_CSS_PX = 3.0     # main.css `th, tr { border-bottom: 3px solid ... }`
INK_THRESHOLD = 30.0       # a channel this far from the band's background counts as ink

# the Player List scenarios: (scenario, dump selector); every element of a row is checked against v10
PARITY_SCENARIOS = [
    ("tools/uidiff/scenarios/ingame-playerlist.json", ".pListTable", 19),
    ("tools/uidiff/scenarios/ingame-playerlist-spectate.json", ".pListTable", 23),
]
# (scenario, dump selector, row class, button classes, minimum buttons measured): buttons on the row's middle
CENTRE_SCENARIOS = [
    ("tools/uidiff/scenarios/account-manager-rows.json", ".knAltList", "knAltRow", ("knAltBtn",), 4),
]
# a dump line: "<indent><kind> <tag or (anon)>  local(x,y wxh) vp(x,y wxh)"; a text run has no vp()
BOX = re.compile(r"^(?P<ind> *)(?P<kind>\w+) (?:\(anon\)|<(?P<tag>[^>]*)>)\s+local\([^)]*\)\s+vp\("
                 r"(?P<x>[-\d.]+),(?P<y>[-\d.]+) (?P<w>[\d.]+)x(?P<h>[\d.]+)\)")
LOCAL = re.compile(r"local\([^)]*? [\d.]+x(?P<h>[\d.]+)\)")
# a Player List row's elements (hud_ui.c): the Focus Player eye / Clear Focus / staff (punishButton), the
# ping, the name link, the mutes; the local row's name is the cell's own text
ELEMENT_TAGS = ("span.punishButton", "span.muteUserBtn", "span.pListPing", "a", "td.pListName")


def dump_boxes(build, scenario, selector, work, label):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(work, label + "-cfg"),
                "KRUNKNATIVE_SCENARIO_DUMP": selector})
    png = os.path.join(work, label + ".png")
    p = subprocess.run([os.path.join(build, "client_bin"), "--render-scenario", os.path.join(ROOT, scenario), png],
                       env=env, cwd=ROOT, capture_output=True, text=True, timeout=300)
    if p.returncode != 0 or "no match" in p.stderr:
        raise RuntimeError("%s: render failed (rc %d): %s" % (label, p.returncode, (p.stderr or p.stdout)[-800:]))
    return p.stderr.splitlines(), png


def is_element(tag):
    return tag == "a" or tag.startswith("a.") or any(tag == t or tag.startswith(t + " ") or tag.startswith(t + ".")
                                                     for t in ELEMENT_TAGS if t != "a")


def rows_with_boxes(lines, is_table_row, row_class, wanted):
    """[(row box, [box, ...]), ...] from the dump: a row's boxes are the wanted ones nested under it."""
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
                   "tag": tag, "boxes": []}
            rows.append(row)
            continue
        if row is not None and wanted(tag):
            row["boxes"].append({"tag": tag, "x": float(m.group("x")), "y": float(m.group("y")),
                                 "w": float(m.group("w")), "h": float(m.group("h"))})
    return rows


def row_band(row, is_table_row):
    """The row's visible band [top, bottom): a table row's separator (3 CSS px, scaled to vp) is not part of it."""
    sep = SEPARATOR_CSS_PX * row["scale"] if is_table_row else 0.0
    return row["y"], row["y"] + row["h"] - sep


def ink(px, x0, x1, y0, y1):
    """(centre, top, bottom) of the ink in the column x0..x1 of the band y0..y1: the pixels whose colour is
    more than INK_THRESHOLD (in any channel) from the band's background (its per-channel median), weighted
    by how far. Colour, not luminance: a hacker's red name is as dark as the row."""
    xs = range(max(0, int(round(x0))), int(round(x1)))
    ys = range(max(0, int(round(y0))), int(round(y1)))
    bg = tuple(statistics.median(px[x, y][c] for y in ys for x in xs) for c in range(3))
    total = mass = 0.0
    top = bottom = None
    for y in ys:
        w = sum(max(0.0, max(abs(px[x, y][c] - bg[c]) for c in range(3)) - INK_THRESHOLD) for x in xs)
        if w > 0.0:
            top = y if top is None else top
            bottom = y
        total += w * y
        mass += w
    if mass <= 0.0:
        return None
    return total / mass + 0.5, top, bottom + 1


def ref_offset_of(scenario):
    regions = os.path.join(ROOT, "tools", "uidiff", "regions", os.path.basename(scenario))
    with open(regions) as fh:
        for r in json.load(fh)["regions"]:
            if r["name"] == "player-list-rows":
                return tuple(r.get("ref_offset", [0, 0]))
    return (0, 0)


def check_parity(build, scenario, selector, min_elements, work):
    """Every element of every Player List row: its ink is where v10's capture has it."""
    label = os.path.splitext(os.path.basename(scenario))[0]
    lines, png = dump_boxes(build, scenario, selector, work, label)
    with open(os.path.join(ROOT, scenario)) as fh:
        ref_png = os.path.join(ROOT, json.load(fh)["ref"])
    dx, dy = ref_offset_of(scenario)
    ours = Image.open(png).convert("RGB").load()
    ref = Image.open(ref_png).convert("RGB").load()
    rows = rows_with_boxes(lines, True, "tr", is_element)
    failures = []
    count = 0
    worst = 0.0
    for i, row in enumerate(rows):
        top, bottom = row_band(row, True)
        mid = (top + bottom) / 2.0
        boxes = row["boxes"]
        if any(b["tag"] == "a" or b["tag"].startswith("a.") for b in boxes):  # a player's row: its name is the link
            boxes = [b for b in boxes if not b["tag"].startswith("td.pListName")]
        for b in boxes:
            x0, x1 = b["x"] - 1.0, b["x"] + b["w"] + 1.0
            mine = ink(ours, x0, x1, top, bottom)
            theirs = ink(ref, x0 + dx, x1 + dx, top + dy, bottom + dy)
            if mine is None or theirs is None:
                failures.append("%s row %d %s: no ink (ours %s, v10 %s)" % (label, i, b["tag"], mine, theirs))
                continue
            # the reference's rows sit dy higher, so its ink does too
            d_centre = mine[0] - (theirs[0] - dy)
            d_top, d_bottom = mine[1] - (theirs[1] - dy), mine[2] - (theirs[2] - dy)
            count += 1
            worst = max(worst, abs(d_centre))
            print("  %-28s row %d  %-32s ink %3d..%-3d centre %+.2f px from the row middle, %+.2f px from v10 (top %+d, bottom %+d)"
                  % (label, i, b["tag"], mine[1], mine[2], mine[0] - mid, d_centre, d_top, d_bottom))
            if abs(d_centre) > PARITY_CENTRE_PX or abs(d_top) > PARITY_EXTENT_PX or abs(d_bottom) > PARITY_EXTENT_PX:
                failures.append("%s row %d %s: ink centre %+.2f px from v10's (top %+d, bottom %+d)"
                                % (label, i, b["tag"], d_centre, d_top, d_bottom))
    if count < min_elements:
        failures.append("%s: only %d elements measured (want >= %d)" % (label, count, min_elements))
    print("%s: %d elements, worst %.2f px from v10 (tolerance %.1f px)" % (os.path.basename(scenario), count, worst, PARITY_CENTRE_PX))
    return failures


def check_centre(build, scenario, selector, row_class, button_classes, min_buttons, work):
    """Every button of every alt row sits on the row's middle."""
    label = os.path.splitext(os.path.basename(scenario))[0]
    lines, _png = dump_boxes(build, scenario, selector, work, label)
    rows = rows_with_boxes(lines, False, row_class, lambda tag: any(("." + c) in tag for c in button_classes))
    failures = []
    offsets = []
    for i, row in enumerate(rows):
        if not row["boxes"]:
            continue
        top, bottom = row_band(row, False)
        mid = (top + bottom) / 2.0
        for b in row["boxes"]:
            off = (b["y"] + b["h"] / 2.0) - mid
            offsets.append(abs(off))
            print("  %-28s row %d  %-32s centre %+.2f px from the row middle (row %.1f..%.1f)"
                  % (label, i, b["tag"], off, row["y"], row["y"] + row["h"]))
            if abs(off) > TOLERANCE_PX:
                failures.append("%s row %d %s: %+.2f px from the row middle" % (label, i, b["tag"], off))
    if len(offsets) < min_buttons:
        failures.append("%s: only %d buttons measured (want >= %d)" % (label, len(offsets), min_buttons))
    print("%s: %d buttons, worst %.2f px (tolerance %.1f px)"
          % (os.path.basename(scenario), len(offsets), max(offsets) if offsets else float("nan"), TOLERANCE_PX))
    return failures


def main():
    if not os.path.isfile(os.path.join(BUILD, "client_bin")):
        print("row_centre_e2e: needs client_bin: skipped")
        return 77
    work = tempfile.mkdtemp(prefix="kn-row-centre-e2e-")
    failures = []
    for scenario, selector, min_elements in PARITY_SCENARIOS:
        failures += check_parity(BUILD, scenario, selector, min_elements, work)
    for scenario, selector, row_class, button_classes, min_buttons in CENTRE_SCENARIOS:
        failures += check_centre(BUILD, scenario, selector, row_class, button_classes, min_buttons, work)
    for f in failures:
        print("FAIL " + f)
    if not failures:
        shutil.rmtree(work, ignore_errors=True)
        print("PASS every Player List element is where v10 puts it (within %.1f px); every alt button is within %.1f px of its row's middle"
              % (PARITY_CENTRE_PX, TOLERANCE_PX))
        return 0
    print("logs kept in %s" % work)
    return 1


if __name__ == "__main__":
    sys.exit(main())

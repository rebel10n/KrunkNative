#!/usr/bin/env python3
"""Every element of a list row sits on its row's vertical middle.

KrunkNative deviates from Krunker v10 here, on purpose (user, 2026-10-22: "we should deviate from krunker
for the vertical alignment of the player list because it just makes sense. i want everything perfectly
sitting vertical in each row."). The Player List's rows (hud_ui.c, styled by ui/css/kn-v10-menu.css) put
the focus eye, the ping, the name, the chat / voice mutes, the K.P.D / Votekick / Kill / TP2 / TPME / TPAll /
wrench buttons and Clear Focus on their row's middle: each element's box (its border box, a text run's line
box) has its centre within TOLERANCE_PX of the middle of its row's band. The band is the row's box minus the
3 CSS px separator under it (main.css `th, tr`), so the separator is not part of the row.

The Account Manager's alt rows (alt_manager_ui.c: Use / Remove of .knAltRow, ui/css/kn-alt.css) have no v10
counterpart: their buttons get the same check (an alt row has no separator, so its band is its box).

  python3 -I client/tests/row_centre_e2e.py BUILD_DIR   (repository root)

Each scenario renders headless with client_bin --render-scenario; KRUNKNATIVE_SCENARIO_DUMP prints the
captured frame's box tree of the selected element (ui_scenario.c, engine_dump_boxes) in viewport px (vp()).
A text run has no vp(): its local rect is converted with its row's scale.

  - ingame-playerlist.json: the Player List of three account players (local row, ping / name / chat / voice /
    K.P.D / Votekick)
  - ingame-playerlist-spectate.json: the same while spectating (Focus Player eye, Clear Focus)
  - client/tests/data/row_centre/staff.json: a DEV account while spectating, with a host star, voice icons and a
    K.P.D suspect row, so Kill / TPAll / TP2 / TPME and the punish popup (wrench) are in the same render
  - account-manager-rows.json: two alt accounts (Use / Remove)

Every element kind the list draws must appear in at least one of these renders (REQUIRED_ELEMENTS): a render
that drops one would otherwise pass vacuously.

Exit 0 when every element is in place, 77 when client_bin is missing, 1 otherwise.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.getcwd()
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
TOLERANCE_PX = 1.0       # an element's box centre from its row's band middle (viewport px)
SEPARATOR_CSS_PX = 3.0   # main.css `th, tr { border-bottom: 3px solid ... }`

# the Player List renders: every element of every row is checked against its row's middle
PLAYER_SCENARIOS = [
    "tools/uidiff/scenarios/ingame-playerlist.json",
    "tools/uidiff/scenarios/ingame-playerlist-spectate.json",
    "client/tests/data/row_centre/staff.json",
]
# every element kind a Player List row draws, as the dump's labels spell them (see key())
REQUIRED_ELEMENTS = [
    "span.pListPing material-icons",    # ping bars
    "span.pListVoice material-icons",   # speaking icon
    "i.material-icons",                 # host star
    "span.punishButton",                # focus eye
    "span.takeAction material-icons",   # the glyph inside the eye / wrench
    "a",                                # player name link
    "text",                             # the local row's name (a text run in its cell)
    "span.muteUserBtn material-icons",  # chat / voice mute
    "img",                              # K.D (police) icon
    "span.punishButton kdf",
    "span.punishButton vote",           # Votekick
    "span.punishButton kill",
    "span.punishButton tp2",
    "span.punishButton tpme",
    "span.punishButton tpall",
    "span.punishButton kick",           # Clear Focus, and the punish popup (wrench)
]
# (scenario, dump selector, row class, button classes, minimum buttons measured): buttons on the row's middle
ALT_SCENARIOS = [
    ("tools/uidiff/scenarios/account-manager-rows.json", ".knAltList", "knAltRow", ("knAltBtn",), 4),
]

# a dump line: "<indent><kind> <label>  local(x,y wxh)[ vp(x,y wxh)][ SC]"; log lines do not match
LINE = re.compile(r"^(?P<ind> *)(?P<kind>\w+) (?P<label>.+?)\s+local\((?P<lx>[-\d.]+),(?P<ly>[-\d.]+) "
                  r"(?P<lw>[\d.]+)x(?P<lh>[\d.]+)\)(?: vp\((?P<vx>[-\d.]+),(?P<vy>[-\d.]+) "
                  r"(?P<vw>[\d.]+)x(?P<vh>[\d.]+)\))?")
# the elements a Player List row draws (hud_ui.c): the eye / Kill / TP2 / TPME / TPAll / Clear Focus / wrench /
# K.D / mute / Votekick buttons and their glyphs, the ping and voice icons, the host star, the name link, the img
ELEMENT = re.compile(r"^(span\.(pListPing|pListVoice|muteUserBtn|punishButton|takeAction)(\s|$)|a$|i\.material-icons$|img$)")


def key(label):
    """A dump label as REQUIRED_ELEMENTS spells it: `<span.punishButton kick>` -> `span.punishButton kick`;
    a name link is `a` whether or not it is red (`<a.red>`)."""
    s = label.strip("<>")
    return "a" if re.match(r"a(\.|$)", s) else s


def dump_boxes(build, scenario, selector, work, label):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(work, label + "-cfg"),
                "KRUNKNATIVE_SCENARIO_DUMP": selector})
    png = os.path.join(work, label + ".png")
    scen = os.path.join(ROOT, scenario)
    p = subprocess.run([os.path.join(build, "client_bin"), "--render-scenario", scen, png],
                       env=env, cwd=ROOT, capture_output=True, text=True, timeout=300)
    if p.returncode != 0 or "no match" in p.stderr:
        raise RuntimeError("%s: render failed (rc %d): %s" % (label, p.returncode, (p.stderr or p.stdout)[-800:]))
    return p.stderr.splitlines()


def parse_boxes(lines):
    """Every box line of the dump, in order, with the label of its enclosing box as `parent`."""
    boxes = []
    stack = []  # (indent, label) of the open ancestors
    for line in lines:
        m = LINE.match(line)
        if not m:
            continue
        ind = len(m.group("ind"))
        while stack and stack[-1][0] >= ind:
            stack.pop()
        label = m.group("label")
        parent = stack[-1][1] if stack else ""
        stack.append((ind, label))
        vp = None
        if m.group("vx") is not None:
            vp = (float(m.group("vx")), float(m.group("vy")), float(m.group("vw")), float(m.group("vh")))
        local = (float(m.group("lx")), float(m.group("ly")), float(m.group("lw")), float(m.group("lh")))
        boxes.append({"ind": ind, "kind": m.group("kind"), "label": label, "key": key(label),
                      "parent": parent, "local": local, "vp": vp})
    return boxes


def rows_of(boxes, is_row):
    """[row, ...] where a row is a box that is_row(); its kids are the boxes nested under it, in order."""
    rows = []
    row = None
    for b in boxes:
        if row is not None and b["ind"] <= row["ind"]:
            row = None
        if is_row(b):
            row = dict(b)
            row["scale"] = row["vp"][3] / row["local"][3] if row["vp"] else 1.0
            row["kids"] = []
            rows.append(row)
        elif row is not None:
            row["kids"].append(b)
    return rows


def vp_rect(b, row):
    """A box's viewport rect: its own vp(), or a text run's local rect scaled from its row."""
    if b["vp"]:
        return b["vp"]
    lx, ly, lw, lh = b["local"]
    rx, ry = row["local"][0], row["local"][1]
    s = row["scale"]
    return (row["vp"][0] + (lx - rx) * s, row["vp"][1] + (ly - ry) * s, lw * s, lh * s)


def band_middle(row):
    """A Player List row's band: its box minus the separator under it (3 CSS px, scaled to the viewport)."""
    return row["vp"][1] + (row["vp"][3] - SEPARATOR_CSS_PX * row["scale"]) / 2.0


def check_players(build, scenario, work, failures, seen):
    label = os.path.splitext(os.path.basename(scenario))[0]
    boxes = parse_boxes(dump_boxes(build, scenario, ".pListTable", work, label))
    rows = rows_of(boxes, lambda b: b["kind"] == "row" and b["label"] == "<tr>")
    if not rows:
        failures.append("%s: no Player List rows in the dump" % label)
        return
    count = 0
    worst = 0.0
    for i, row in enumerate(rows):
        mid = band_middle(row)
        for b in row["kids"]:
            is_name_text = b["kind"] == "text" and b["parent"].startswith("<td.pListName")
            if not (is_name_text or ELEMENT.match(b["key"])):
                continue
            name = "text" if is_name_text else b["key"]
            x, y, w, h = vp_rect(b, row)
            off = (y + h / 2.0) - mid
            count += 1
            seen.add(name)
            worst = max(worst, abs(off))
            print("  %-28s row %d  %-34s centre %+.2f px from the row middle (box %.1f..%.1f)"
                  % (label, i, name, off, y, y + h))
            if abs(off) > TOLERANCE_PX:
                failures.append("%s row %d %s: %+.2f px from the row middle" % (label, i, name, off))
    if count == 0:
        failures.append("%s: no elements measured" % label)
    print("%s: %d elements in %d rows, worst %.2f px (tolerance %.1f px)"
          % (os.path.basename(scenario), count, len(rows), worst, TOLERANCE_PX))


def check_alt(build, scenario, selector, row_class, button_classes, min_buttons, work):
    """Every button of every alt row sits on the row's middle (an alt row has no separator)."""
    label = os.path.splitext(os.path.basename(scenario))[0]
    boxes = parse_boxes(dump_boxes(build, scenario, selector, work, label))
    rows = rows_of(boxes, lambda b: b["kind"] == "grid" and ("." + row_class) in b["label"])
    failures = []
    offsets = []
    for i, row in enumerate(rows):
        mid = row["vp"][1] + row["vp"][3] / 2.0
        for b in row["kids"]:
            if not b["vp"] or not any(("." + c) in b["label"] for c in button_classes):
                continue
            off = (b["vp"][1] + b["vp"][3] / 2.0) - mid
            offsets.append(abs(off))
            print("  %-28s row %d  %-34s centre %+.2f px from the row middle" % (label, i, b["key"], off))
            if abs(off) > TOLERANCE_PX:
                failures.append("%s row %d %s: %+.2f px from the row middle" % (label, i, b["key"], off))
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
    seen = set()
    for scenario in PLAYER_SCENARIOS:
        check_players(BUILD, scenario, work, failures, seen)
    for need in REQUIRED_ELEMENTS:
        if need not in seen:
            failures.append("no %r element in any Player List render (the check would pass without it)" % need)
    for scenario, selector, row_class, button_classes, min_buttons in ALT_SCENARIOS:
        failures += check_alt(BUILD, scenario, selector, row_class, button_classes, min_buttons, work)
    for f in failures:
        print("FAIL " + f)
    if not failures:
        shutil.rmtree(work, ignore_errors=True)
        print("PASS every Player List element sits on its row's middle (within %.1f px); every alt button is within %.1f px of its row's middle"
              % (TOLERANCE_PX, TOLERANCE_PX))
        return 0
    print("logs kept in %s" % work)
    return 1


if __name__ == "__main__":
    sys.exit(main())

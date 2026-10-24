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

An official game's class list (state.officialClasses: the GAME_CONFIG "classes" krunker.io sends once the game
loads, which never names the Nettspend) must keep the Nettspend's card in the class tab, locked: it is listed
whenever the server's list is applied, not removed when the list arrives (menu_class_listed).

The class level req (ui_loadout.c class_unlocked, v10.0.2 _34270) is checked on the whole class tab through
KRUNKNATIVE_SCENARIO_HTML (the captured DOM): a guest in an official krunker.io game (state.official) sees each
class with minRec locked ("Register to unlock", "Req lvl N") and the classes without one open; a guest in a
KrunkNative lobby sees every class open, no lock card at all (Box Office has no level req).
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from html.parser import HTMLParser

ROOT = os.getcwd()
# the worktree lacks the untracked assets/ (fonts): KRUNKNATIVE_ASSETS_DIR points at the checkout's
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or os.path.join(ROOT, "assets")
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
CLIENT = os.path.join(BUILD, "client_bin")
BOX = re.compile(r"local\(([-\d.]+),([-\d.]+) ([\d.]+)x([\d.]+)\)")
WORK = tempfile.mkdtemp(prefix="kn-class-label-", dir=os.path.expanduser("~/.cache") if os.path.isdir(os.path.expanduser("~/.cache")) else None)
results = []
# the class list an official krunker.io game sends (GAME_CONFIG "classes"): Krunker's 16 classes less the two
# hidden ones (Deagler 10, Survivor 14), as resetClasses builds it (v10 _64265). KrunkNative's Nettspend (the last
# class, CLASS_ID_NETTSPEND past v10's 16) is never in it, so it is the list the Nettspend's card must survive (menu_class_listed).
OFFICIAL_CLASSES = [i for i in range(16) if i not in (10, 14)]


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def scenario(name, official, classes=None):
    path = os.path.join(WORK, name + ".json")
    state = {
        "menu": True, "connected": True, "ready": True, "inGame": False, "name": "Guest", "classIndex": 0,
        "region": "...", "mapName": "Kanji", "mapFile": "maps/311_kanji.json", "gameName": "ffa on Kanji",
        "anim_time": 0.8, "window": 3, "windowActions": [["windows.2.switchTab", 0]],
    }
    if official:
        state["official"] = True
    if classes is not None:
        state["officialClasses"] = classes
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


class CardParser(HTMLParser):
    """The class tab's cards: every <div class="classCard"> with its onclick, whether it carries a
    lockedClass overlay, and its text (nested divs counted, so the card ends at its own close tag)."""

    def __init__(self):
        super().__init__()
        self.cards, self.cur, self.depth = [], None, 0

    def handle_starttag(self, tag, attrs):
        if tag != "div":
            return
        a = dict(attrs)
        classes = (a.get("class") or "").split()
        if self.cur is None:
            if "classCard" in classes:
                self.cur = {"onclick": a.get("onclick") or "", "locked": False, "text": ""}
                self.depth = 1
            return
        self.depth += 1
        if "lockedClass" in classes:
            self.cur["locked"] = True

    def handle_endtag(self, tag):
        if tag != "div" or self.cur is None:
            return
        self.depth -= 1
        if self.depth == 0:
            self.cards.append(self.cur)
            self.cur = None

    def handle_data(self, data):
        if self.cur is not None:
            self.cur["text"] += data


def class_cards(name, official, classes=None):
    """The class tab's cards (CardParser) of one render, or None when the render failed."""
    scen = scenario(name, official, classes)
    out = os.path.join(WORK, name + ".png")
    html = os.path.join(WORK, name + ".html")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({
        "KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
        "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
        "KRUNKNATIVE_SCENARIO_HTML": html, "KRUNKNATIVE_ASSETS_DIR": ASSETS,
    })
    proc = subprocess.run([CLIENT, "--render-scenario", scen, out], env=env, cwd=ROOT, capture_output=True, text=True, timeout=300)
    if proc.returncode != 0 or not os.path.exists(html):
        return None
    parser = CardParser()
    with open(html, encoding="utf-8", errors="replace") as f:
        parser.feed(f.read())
    return parser.cards


def check_official_list():
    """The official game's class list (GAME_CONFIG "classes", applied once the game loads) never names the
    Nettspend. Its card must stay in the class tab, locked, as it is before that list arrives: the card
    vanished when the list did (menu_class_listed filtered by the server's list)."""
    listed = class_cards("official-list", True, OFFICIAL_CLASSES)
    check("official server's class list: the class tab renders", listed is not None and len(listed) > 1,
          "" if listed is not None else "client_bin --render-scenario failed")
    if listed is None:
        return
    nett = listed[-1] if listed else None
    names = " | ".join(" ".join(c["text"].split())[:16] for c in listed)
    check("official server's class list: the Nettspend's card is still in the class tab",
          len(listed) == len(OFFICIAL_CLASSES) + 1 and nett is not None and "Nettspend" in nett["text"],
          f"{len(listed)} cards: {names}")
    check("official server's class list: the Nettspend is locked for not being on official servers",
          nett is not None and nett["locked"] and "Not on official servers" in nett["text"])


def check_class_gate():
    """The level req on the whole class tab: official guest locked by minRec, KrunkNative guest all open."""
    official = class_cards("gate-official", True)
    check("official guest: the class tab renders", official is not None and len(official) > 1,
          "" if official is not None else "client_bin --render-scenario failed")
    if official:
        reg = [c for c in official if c["locked"] and "Register to unlock" in c["text"]]
        req = [c for c in official if c["locked"] and re.search(r"Req lvl \d+", c["text"])]
        open_cards = [c for c in official if not c["locked"]]
        nett = official[-1]
        check("official guest: the level-locked classes show the lock and their req",
              len(reg) > 0 and len(req) == len(reg), f"{len(reg)} locked, {len(req)} with a Req lvl line")
        check("official guest: the classes without a minRec stay open", len(open_cards) > 0,
              f"{len(open_cards)} open of {len(official)}")
        check("official guest: the Nettspend is still locked for not being on official servers",
              nett["locked"] and "Not on official servers" in nett["text"])
    krunknative = class_cards("gate-krunknative", False)
    check("krunknative guest: the class tab renders", krunknative is not None and len(krunknative) > 1,
          "" if krunknative is not None else "client_bin --render-scenario failed")
    if krunknative:
        locked = [c for c in krunknative if c["locked"]]
        check("krunknative guest: every class is open with no lock card (Box Office has no level req)",
              not locked and all("selectClass(" in c["onclick"] for c in krunknative),
              f"{len(locked)} of {len(krunknative)} cards locked")


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
    check_class_gate()
    check_official_list()
    shutil.rmtree(WORK, ignore_errors=True)
    failed = len(results) - sum(results)
    print(f"locked_class_label_e2e: {len(results) - failed}/{len(results)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

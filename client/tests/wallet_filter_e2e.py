#!/usr/bin/env python3
"""The Wallet's inventory filter (deob-10.0.7.js:150777, the wallet's options at :169833; v10_wallet.c).

Search, season, sort, the five checkboxes (Only Show Missing, Hide Non-Tradable, Show Only Paintable, Only
Painted, Group by Wear), the rarity / weapon / cosmetic / wear sets, and Reset. Group by Wear is the saved
store s_groupInventoryByWear: on, copies of one item and wear tier draw as one stack (:169426-169450,
:169861-169904); a click opens it, and the last copy carries the collapse control. Reset clears the sets and
the search and keeps that switch.

The account (store data from assets/skins.json):
  Topo 7819 (Assault Rifle, rarity 2, season 9, paintable): copies 1000101 (wear 0.05), 1000102 (0.07) and
    1000103 (0.50, painted); a Box Office copy id is at least 1000000 (V10_COPY_ID_BASE, bo_inventory.c)
  Flip-Flop 7857 (melee, rarity 4, season 9, paintable) x2, Purple Beanie 1411 (hat, rarity 0, season 3) x1,
  Celeste 1869 (Assault Rifle, rarity 4, season 3, not tradable) x1, Autumn Hunt 1 (weapon 1, rarity 1, season 1) x1
Each case opens the Wallet in a --render-scenario run, dispatches the page's own calls (knV10WalletF(...), as
the filter's oninput / onchange / onclick send them) and reads the grid back from the written document.

  python3 -I client/tests/wallet_filter_e2e.py BUILD_DIR [SRC]

Headless (KRUNKNATIVE_HEADLESS=1). Exit 77 without client_bin.
"""
import html.parser
import json
import os
import re
import signal
import subprocess
import sys
import tempfile

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
SRC = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.getcwd())
BIN = os.path.join(BUILD, "client_bin")
if not os.access(BIN, os.X_OK):
    print("wallet_filter_e2e: no client_bin: skipped")
    sys.exit(77)

TOPO, FLIP, BEANIE, CELESTE, AUTUMN = 7819, 7857, 1411, 1869, 1
OWNED = (TOPO, FLIP, BEANIE, CELESTE, AUTUMN)
SKINS = [{"ind": TOPO, "cnt": 1, "id": 1000101, "m": {"wear": 0.05}},
         {"ind": TOPO, "cnt": 1, "id": 1000102, "m": {"wear": 0.07}},
         {"ind": TOPO, "cnt": 1, "id": 1000103, "m": {"wear": 0.5, "paint": "#00ff00"}},
         {"ind": FLIP, "cnt": 2}, {"ind": BEANIE, "cnt": 1}, {"ind": CELESTE, "cnt": 1}, {"ind": AUTUMN, "cnt": 1}]
FAILS = []


def check(what, ok, detail=""):
    print(("  ok   " if ok else "  FAIL ") + what + ("" if ok or not detail else "  [%s]" % str(detail)[:400]), flush=True)
    if not ok:
        FAILS.append(what)


class Grid(html.parser.HTMLParser):
    """The cards of #knV10WalletGrid in order (their onclick), the filter's inputs, and the collapse buttons."""

    VOID = ("img", "input", "br", "hr", "meta", "link", "source", "wbr")

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.depth, self.grid_at, self.cards, self.inputs, self.collapse, self.selects = 0, None, [], [], 0, {}
        self.in_group = 0
        self.header, self._in_h3, self._sel, self.search = "", False, None, None

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        cls = (a.get("class") or "").split()
        if a.get("id") == "knV10WalletGrid":
            self.grid_at = self.depth
        if self.grid_at is not None and "market-card" in cls:
            self.cards.append(a.get("onclick", ""))
        if self.grid_at is not None and "stack-collapse-fab" in cls:
            self.collapse += 1
        if self.grid_at is not None and "wallet-item" in cls and "in-group" in cls:
            self.in_group += 1
        if tag == "input" and a.get("type") == "checkbox" and "onchange" in a:
            self.inputs.append((a["onchange"], "checked" in a))
        if tag == "input" and a.get("id") == "knV10WalletSearch":
            self.search = a.get("value", "")
        if tag == "select":
            self._sel = "season" if "season-select" in cls else "sort" if "sort-select" in cls else None
        if tag == "option" and self._sel and "selected" in a:
            self.selects[self._sel] = a.get("value")
        if tag == "h3":
            self._in_h3 = True
        if tag not in self.VOID:
            self.depth += 1

    def handle_endtag(self, tag):
        if tag in self.VOID:
            return
        self.depth -= 1
        if self.grid_at is not None and self.depth == self.grid_at:
            self.grid_at = None
        if tag == "h3":
            self._in_h3 = False
        if tag == "select":
            self._sel = None

    def handle_data(self, data):
        if self._in_h3 and "Inventory (" in data:
            self.header = data


def items(grid):
    """The grid's cards as store indices; a collapsed wear stack as 'S<index>:<wear>'."""
    out = []
    for c in grid.cards:
        m = re.match(r"knV10WalletItem\((\d+)\)", c)
        s = re.match(r"knV10WalletStack\('([0-9:]+)'\)", c)
        out.append(int(m.group(1)) if m else "S" + s.group(1) if s else c)
    return out


def run(tag, actions, storage=None):
    st = {"menu": True, "connected": True, "ready": True, "inGame": False, "name": "Trader", "classIndex": 0,
          "region": "...", "mapName": "Kanji", "mapFile": "maps/311_kanji.json", "gameName": "ffa on Kanji",
          "anim_time": 0.8, "account": {"name": "Trader", "score": 2000000, "funds": 5000, "skins": SKINS},
          "wallet": {"open": True}, "windowActions": [["knV10WalletF", "head"]] + actions}
    if storage:
        st["storage"] = storage
    work = tempfile.mkdtemp(prefix="kn-wallet-filter-%s." % tag, dir=os.path.expanduser("~/.cache"))
    path, html_out = os.path.join(work, "scenario.json"), os.path.join(work, "page.html")
    with open(path, "w") as f:
        json.dump({"size": [1600, 900], "background": "#000000", "state": st, "frames": 2}, f)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update(KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_EGL_DEVICE=os.environ.get("KRUNKNATIVE_EGL_DEVICE", "0"),
               LIBGL_ALWAYS_SOFTWARE="1", KRUNKNATIVE_SCENARIO_HTML=html_out, KRUNKNATIVE_AUDIO_BACKEND="null",
               KRUNKNATIVE_NO_BROWSER="1", KRUNKNATIVE_UPDATES_URL="off", XDG_CONFIG_HOME=os.path.join(work, "cfg"))
    with open(os.path.join(work, "client.log"), "w") as out:
        p = subprocess.Popen([BIN, "--render-scenario", path, os.path.join(work, "frame.png")], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, cwd=SRC, start_new_session=True)
        try:
            rc = p.wait(timeout=180)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            rc = -9
    g = Grid()
    if rc == 0 and os.path.isfile(html_out):
        with open(html_out, encoding="utf-8", errors="replace") as f:
            g.feed(f.read())
    else:
        check("%s: client_bin exits 0 and writes the document" % tag, False, "rc=%s, %s" % (rc, work))
    g.work = work
    return g


# close the Wallet (the view stack's backdrop) and open it again: open_modal reads s_groupInventoryByWear back
# from storage (group_wear_load), so the reopened switch is what was saved
REOPEN = [["knViewBackdrop"], ["knV10Wallet"], ["knV10WalletF", "head"]]


def grouped_order(got, want):
    """want's order, except the Topo rows: they tie on name and index, so qsort may put them either way."""
    def split(xs):
        rest = [x for x in xs if x != TOPO and not str(x).startswith("S")]
        return rest, sorted(str(x) for x in xs if x == TOPO or str(x).startswith("S"))
    return split(got) == split(want) and got[:len(split(want)[0])] == split(want)[0]


def box(g, call):
    for onchange, on in g.inputs:
        if onchange == call:
            return on
    return None


def main():
    g = run("default", [])
    check("default: every unit is a tile, by name (Autumn Hunt, Celeste, Flip-Flop, Purple Beanie, Topo x3)",
          items(g) == [AUTUMN, CELESTE, FLIP, BEANIE, TOPO, TOPO, TOPO], items(g))
    check("default: no checkbox is on, Group by Wear off", [on for _, on in g.inputs[:5]] == [False] * 5, g.inputs[:5])

    g = run("search", [["knV10WalletF", "search", "tOp"]])
    check("search: \"tOp\" keeps the three Topo copies (case-insensitive)", items(g) == [TOPO] * 3, items(g))
    check("search: the box keeps the text", g.search == "tOp", g.search)

    g = run("season", [["knV10WalletF", "season", 3]])
    check("season 3: Celeste and Purple Beanie", items(g) == [CELESTE, BEANIE], items(g))
    check("season 3: the select shows Season 3", g.selects.get("season") == "3", g.selects)

    g = run("sort", [["knV10WalletF", "sort", "rarity"]])
    check("sort by rarity: highest first, ties by index (Celeste, Flip-Flop, Topo x3, Autumn Hunt, Purple Beanie)",
          items(g) == [CELESTE, FLIP, TOPO, TOPO, TOPO, AUTUMN, BEANIE], items(g))
    check("sort by rarity: the select shows it", g.selects.get("sort") == "rarity", g.selects)

    g = run("missing", [["knV10WalletF", "chk", 0]])
    check("Only Show Missing: no owned item, other items listed", items(g) and not set(items(g)) & set(OWNED), items(g)[:10])
    check("Only Show Missing: its box is on", box(g, "knV10WalletF('chk',0)") is True, g.inputs[:5])

    g = run("tradable", [["knV10WalletF", "chk", 1]])
    check("Hide Non-Tradable: Celeste is gone", items(g) == [AUTUMN, FLIP, BEANIE, TOPO, TOPO, TOPO], items(g))

    g = run("paintable", [["knV10WalletF", "chk", 2]])
    check("Show Only Paintable: Flip-Flop and Topo x3", items(g) == [FLIP, TOPO, TOPO, TOPO], items(g))

    g = run("painted", [["knV10WalletF", "chk", 3]])
    check("Only Painted: the painted Topo copy alone", items(g) == [TOPO], items(g))

    g = run("rarity", [["knV10WalletF", "only", 0, 4]])
    check("rarity only 4: Celeste and Flip-Flop", items(g) == [CELESTE, FLIP], items(g))
    g = run("rarity-off", [["knV10WalletF", "set", 0, 2]])
    check("rarity 2 off: the Topo copies are gone", TOPO not in items(g) and len(items(g)) == 4, items(g))

    g = run("weapon", [["knV10WalletF", "set", 1, 0]])  # weapon 1's skins (index 0) off
    check("weapon 1 off: Autumn Hunt is gone, the Assault Rifle skins stay", AUTUMN not in items(g) and
          items(g).count(TOPO) == 3 and CELESTE in items(g), items(g))

    g = run("cosmetic", [["knV10WalletF", "set", 2, 1]])  # Hats off
    check("Hats off: Purple Beanie is gone, the melee stays", BEANIE not in items(g) and FLIP in items(g) and len(items(g)) == 6, items(g))

    g = run("wear", [["knV10WalletF", "only", 3, 0]])
    check("wear only Flawless: the two Flawless Topo copies", items(g) == [TOPO, TOPO], items(g))

    g = run("group", [["knV10WalletF", "chk", 4]])
    check("Group by Wear: the two Flawless copies are one stack, the Trashed copy its own tile",
          grouped_order(items(g), [AUTUMN, CELESTE, FLIP, BEANIE, "S%d:0" % TOPO, TOPO]), items(g))
    check("Group by Wear: its box is on", box(g, "knV10WalletF('chk',4)") is True, g.inputs[:5])
    g = run("group-reopen", [["knV10WalletF", "chk", 4]] + REOPEN)
    check("Group by Wear: saved (s_groupInventoryByWear), the reopened Wallet is grouped",
          box(g, "knV10WalletF('chk',4)") is True and "S%d:0" % TOPO in items(g), "%r %r" % (items(g), g.inputs[:5]))
    g = run("group-off-reopen", [["knV10WalletF", "chk", 4]] + REOPEN, {"s_groupInventoryByWear": "true"})
    check("Group by Wear off: saved too, the reopened Wallet is not grouped",
          box(g, "knV10WalletF('chk',4)") is False and items(g).count(TOPO) == 3, "%r %r" % (items(g), g.inputs[:5]))

    g = run("group-open", [["knV10WalletF", "chk", 4], ["knV10WalletStack", "%d:0" % TOPO]])
    check("Group by Wear, stack opened: its two copies, the last with the collapse control",
          items(g) == [AUTUMN, CELESTE, FLIP, BEANIE, TOPO, TOPO, TOPO] and g.collapse == 1 and g.in_group == 2,
          "%r collapse=%d in_group=%d" % (items(g), g.collapse, g.in_group))
    g = run("group-closed", [["knV10WalletF", "chk", 4], ["knV10WalletStack", "%d:0" % TOPO], ["knV10WalletStack", "%d:0" % TOPO]])
    check("Group by Wear, stack closed again: one stack", "S%d:0" % TOPO in items(g) and g.collapse == 0, items(g))

    g = run("group-saved", [], {"s_groupInventoryByWear": "true"})
    check("a saved s_groupInventoryByWear opens grouped", "S%d:0" % TOPO in items(g) and box(g, "knV10WalletF('chk',4)") is True,
          "%r %r" % (items(g), g.inputs[:5]))

    g = run("reset", [["knV10WalletF", "chk", 4], ["knV10WalletF", "search", "topo"], ["knV10WalletF", "season", 9],
                      ["knV10WalletF", "only", 0, 2], ["knV10WalletF", "only", 3, 0], ["knV10WalletF", "chk", 1],
                      ["knV10WalletF", "sort", "rarity"], ["knV10WalletF", "reset"]])
    check("Reset: the search is empty", g.search == "", g.search)
    check("Reset: season, sort and the other boxes are back", g.selects == {"season": "0", "sort": "name"} and
          [on for _, on in g.inputs[:4]] == [False] * 4, "%r %r" % (g.selects, g.inputs[:5]))
    check("Reset: Group by Wear stays on", box(g, "knV10WalletF('chk',4)") is True, g.inputs[:5])
    check("Reset: every unit again, still grouped",
          grouped_order(items(g), [AUTUMN, CELESTE, FLIP, BEANIE, "S%d:0" % TOPO, TOPO]), items(g))
    check("Reset: the rarity set is full again (seven boxes on)",
          [on for c, on in g.inputs if c.startswith("knV10WalletF('set',0,")] == [True] * 7,
          [x for x in g.inputs if "'set',0," in x[0]])
    g = run("reset-reopen", [["knV10WalletF", "chk", 4], ["knV10WalletF", "reset"]] + REOPEN)
    check("Reset: the saved switch is untouched, the reopened Wallet is grouped",
          box(g, "knV10WalletF('chk',4)") is True and "S%d:0" % TOPO in items(g), "%r %r" % (items(g), g.inputs[:5]))

    if FAILS:
        print("wallet_filter_e2e: %d failure(s)" % len(FAILS))
        return 1
    print("wallet_filter_e2e: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

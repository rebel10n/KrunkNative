#!/usr/bin/env python3
"""A copy's card carries createTip's data-* attributes (deob-10.0.7.js:221824, read at :223708-223712).

ui_loadout_tooltip_attrs writes data-nametag, data-wear (the tier name), data-wear-float (%.4f), data-serial,
data-paint, data-paint-shader and data-seed on a copy's card. Three places draw such a card: the loadout picker
(Select Skin, window 15: owned_card) and both lists of the live trade (#tradeInv / the offer list, trade_ui.c).

The account owns two copies of Topo (store 7819, an Assault Rifle skin) and a plain stack of another skin:
  copy 42: m {name "Al's <tag>", wear 0.123456, serial 7, paint "#ff00aa", paintShader 3, seed 1234567.8}
  copy 77: m {wear 0.75} (no seed: data-seed is skin_copy_seed's hash of the id)
A stack has no copy and no m, so no data-* tip attribute at all.

  python3 -I client/tests/loadout_tip_attrs_e2e.py BUILD_DIR [SRC]

Headless client_bin --render-scenario (KRUNKNATIVE_HEADLESS=1). Exit 77 without client_bin.
"""
import html.parser
import json
import os
import signal
import subprocess
import sys
import tempfile

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
SRC = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.getcwd())
BIN = os.path.join(BUILD, "client_bin")
if not os.access(BIN, os.X_OK):
    print("loadout_tip_attrs_e2e: no client_bin: skipped")
    sys.exit(77)

TOPO = 7819
STACK = 7857
FAILS = []
TIP_ATTRS = ("data-nametag", "data-wear", "data-wear-float", "data-serial", "data-paint", "data-paint-shader", "data-seed")
COPY_A = {"name": "Al's <tag>", "wear": 0.123456, "serial": 7, "paint": "#ff00aa", "paintShader": 3, "seed": 1234567.8}
COPY_B = {"wear": 0.75}


def check(what, ok, detail=""):
    print(("  ok   " if ok else "  FAIL ") + what + ("" if ok or not detail else "  [%s]" % str(detail)[:400]), flush=True)
    if not ok:
        FAILS.append(what)


def copy_seed(copy_id):
    """skin_copy_seed without m.seed (shared.h): the id's integer hash mod 1e6."""
    x = copy_id & 0xFFFFFFFF
    x ^= x >> 16
    x = (x * 0x7FEB352D) & 0xFFFFFFFF
    x ^= x >> 15
    x = (x * 0x846CA68B) & 0xFFFFFFFF
    x ^= x >> 16
    return x % 1000000


WANT_A = {"data-nametag": "Al's <tag>", "data-wear": "Tested", "data-wear-float": "0.1235", "data-serial": "7",
          "data-paint": "#ff00aa", "data-paint-shader": "3", "data-seed": "234567"}
WANT_B = {"data-wear": "Wrecked", "data-wear-float": "0.7500", "data-seed": str(copy_seed(77))}


class Cards(html.parser.HTMLParser):
    """Every element whose class list has one of the wanted classes, with its attributes."""

    def __init__(self, classes):
        super().__init__(convert_charrefs=True)
        self.classes, self.found = classes, []

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if set((a.get("class") or "").split()) & self.classes:
            self.found.append(a)


def cards(doc, *classes):
    p = Cards(set(classes))
    p.feed(doc)
    return p.found


def env_for(work, html_out):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update(KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_EGL_DEVICE=os.environ.get("KRUNKNATIVE_EGL_DEVICE", "0"),
               LIBGL_ALWAYS_SOFTWARE="1", KRUNKNATIVE_SCENARIO_HTML=html_out,
               KRUNKNATIVE_AUDIO_BACKEND="null", KRUNKNATIVE_NO_BROWSER="1",
               KRUNKNATIVE_UPDATES_URL="off", XDG_CONFIG_HOME=os.path.join(work, "cfg"))
    return env


def render(tag, state):
    work = tempfile.mkdtemp(prefix="kn-tip-attrs-%s." % tag, dir=os.path.expanduser("~/.cache"))
    path, html_out = os.path.join(work, "scenario.json"), os.path.join(work, "page.html")
    with open(path, "w") as f:
        json.dump({"size": [1280, 720], "background": "#000000", "state": state, "frames": 3}, f)
    log = os.path.join(work, "client.log")
    with open(log, "w") as out:
        p = subprocess.Popen([BIN, "--render-scenario", path, os.path.join(work, "frame.png")], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env_for(work, html_out), cwd=SRC,
                             start_new_session=True)
        try:
            rc = p.wait(timeout=180)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            rc = -9
    check("%s: client_bin exits 0" % tag, rc == 0, "rc=%s, %s" % (rc, work))
    if rc != 0 or not os.path.isfile(html_out):
        return ""
    with open(html_out, encoding="utf-8", errors="replace") as f:
        return f.read()


def base_state():
    return {"menu": True, "connected": True, "ready": True, "inGame": False, "name": "Trader", "classIndex": 0,
            "region": "...", "mapName": "Kanji", "mapFile": "maps/311_kanji.json", "gameName": "ffa on Kanji",
            "anim_time": 0.8}


def expect(tag, card, want):
    if card is None:
        check("%s: the card is drawn" % tag, False)
        return
    got = {k: card.get(k) for k in TIP_ATTRS if k in card}
    check("%s: the tip attributes are the copy's" % tag, got == want, "got %r want %r" % (got, want))


def picker():
    st = base_state()
    st["window"] = 15  # Select Skin, the primary (Assault Rifle) of class 0
    st["account"] = {"name": "Trader", "score": 2000000, "funds": 5000,
                     "skins": [{"ind": TOPO, "cnt": 1, "id": 42, "m": COPY_A},
                               {"ind": TOPO, "cnt": 1, "id": 77, "m": json.dumps(COPY_B)},  # m as a JSON string too
                               {"ind": TOPO, "cnt": 2}]}
    doc = render("picker", st)
    if not doc:
        return
    topo = [c for c in cards(doc, "skinCard") if c.get("data-index") == str(TOPO)]
    check("picker: the three Topo cards are drawn (two copies, one stack)", len(topo) == 3, len(topo))
    by_seed = {c.get("data-seed"): c for c in topo}
    expect("picker copy 42", by_seed.get("234567"), WANT_A)
    expect("picker copy 77", by_seed.get(WANT_B["data-seed"]), WANT_B)
    plain = [c for c in topo if not any(k in c for k in TIP_ATTRS)]
    check("picker: the stack's card has no tip attribute", len(plain) == 1, [sorted(c) for c in topo])
    check("picker: every card still calls createTip(this)", all(c.get("onmouseover") == "createTip(this)" for c in topo))


def trade():
    st = base_state()
    st["window"] = 37  # showWindow(37) = windows[36]
    st["account"] = {"name": "Trader", "score": 2000000, "funds": 5000, "skins": [{"ind": TOPO, "cnt": 1}]}
    mine = [{"i": TOPO, "c": 1, "id": 42, "m": COPY_A}, {"i": STACK, "c": 3}]
    theirs = [{"i": TOPO, "c": 1, "id": 77, "m": json.dumps(COPY_B)}]
    st["trade"] = {"trader": "Bob", "invs": [mine, theirs], "offers": [[[TOPO, 1, 42], [STACK, 1]], [[TOPO, 1, 77]]]}
    doc = render("trade", st)
    if not doc:
        return
    holders = {c.get("id"): c for c in cards(doc, "tItemHolder")}
    check("trade: both inventories and both offer lists are drawn",
          all(k in holders for k in ("trd_inv_0_%d_42" % TOPO, "trd_inv_1_%d_77" % TOPO, "trd_sel_0_%d_42" % TOPO,
                                     "trd_sel_1_%d_77" % TOPO, "trd_inv_0_%d" % STACK)), sorted(k for k in holders if k))
    expect("trade: my inventory's copy 42", holders.get("trd_inv_0_%d_42" % TOPO), WANT_A)
    expect("trade: their inventory's copy 77", holders.get("trd_inv_1_%d_77" % TOPO), WANT_B)
    expect("trade: my offer's copy 42", holders.get("trd_sel_0_%d_42" % TOPO), WANT_A)
    expect("trade: their offer's copy 77", holders.get("trd_sel_1_%d_77" % TOPO), WANT_B)
    for key in ("trd_inv_0_%d" % STACK, "trd_sel_0_%d" % STACK):
        c = holders.get(key)
        check("trade: the stack %s has no tip attribute" % key, c is not None and not any(k in c for k in TIP_ATTRS),
              sorted(c) if c else "missing")


def main():
    picker()
    trade()
    if FAILS:
        print("loadout_tip_attrs_e2e: %d failure(s)" % len(FAILS))
        return 1
    print("loadout_tip_attrs_e2e: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

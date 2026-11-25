#!/usr/bin/env python3
"""A Wallet tile opens the item popup (viewItem), not the Catalog's item page.

v10's inventory tile calls viewItem for its item (deob-10.0.7.js:169669-169765, the menu's #itemViewPop).
v10_wallet.c's knV10WalletItem dispatches viewItem (social_ui.c h_view_item); it used to open the Catalog's
item page (.items-detail-name, v10_market_wallet_item).

The account owns one Topo (store 7819). The Wallet is open, and a real DOM click lands on its only tile
(#knV10WalletGrid .market-card, onclick knV10WalletItem(7819)). The written document must show #itemViewPop
(display block) named Topo, and no Catalog item page.

The tile's lock (.lock-toggle) is v10.0.7's svelte-1wcm5z8 rule (deob-10.0.7.js:147437): a 28 x 28 circle placed
absolutely at top 44, right 8 of the tile, under Quick Sell. KRUNKNATIVE_SCENARIO_DUMP='.wallet-item' prints the
tile's boxes; the lock must sit there and the tile must stay the card's 128 x 128. Without the rule (KN's
svelte.css carried the v10.0.2 block, which has no .lock-toggle) the button fell under the card as a 40 x 33
UA button and the tile grew to 161 px.

  python3 -I client/tests/wallet_item_e2e.py BUILD_DIR [SRC]

Headless client_bin --render-scenario (KRUNKNATIVE_HEADLESS=1). Exit 77 without client_bin.
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
    print("wallet_item_e2e: no client_bin: skipped")
    sys.exit(77)

TOPO = 7819
FAILS = []


def check(what, ok, detail=""):
    print(("  ok   " if ok else "  FAIL ") + what + ("" if ok or not detail else "  [%s]" % str(detail)[:400]), flush=True)
    if not ok:
        FAILS.append(what)


class Doc(html.parser.HTMLParser):
    """Each element's attributes by id, the text under each class, the classes seen."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.by_id, self.text, self.stack, self.classes = {}, {}, [], set()

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if a.get("id"):
            self.by_id[a["id"]] = a
        cls = (a.get("class") or "").split()
        self.classes.update(cls)
        if tag not in ("img", "input", "br", "hr", "meta", "link", "source", "wbr"):
            self.stack.append(cls)

    def handle_endtag(self, tag):
        if tag not in ("img", "input", "br", "hr", "meta", "link", "source", "wbr") and self.stack:
            self.stack.pop()

    def handle_data(self, data):
        for cls in self.stack:
            for c in cls:
                self.text[c] = self.text.get(c, "") + data


BOX = re.compile(r"^( *)(\S+) <([^>]*)>  local\(([-\d.]+),([-\d.]+) ([\d.]+)x([\d.]+)\)")


def render(tag, state, dump=None):
    work = tempfile.mkdtemp(prefix="kn-wallet-item-%s." % tag, dir=os.path.expanduser("~/.cache"))
    path, html_out = os.path.join(work, "scenario.json"), os.path.join(work, "page.html")
    with open(path, "w") as f:
        json.dump({"size": [1280, 720], "background": "#000000", "state": state, "frames": 3}, f)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update(KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_EGL_DEVICE=os.environ.get("KRUNKNATIVE_EGL_DEVICE", "0"),
               LIBGL_ALWAYS_SOFTWARE="1", KRUNKNATIVE_SCENARIO_HTML=html_out, KRUNKNATIVE_AUDIO_BACKEND="null",
               KRUNKNATIVE_NO_BROWSER="1", KRUNKNATIVE_UPDATES_URL="off", XDG_CONFIG_HOME=os.path.join(work, "cfg"))
    if dump:
        env["KRUNKNATIVE_SCENARIO_DUMP"] = dump
    log = os.path.join(work, "client.log")
    with open(log, "w") as out:
        p = subprocess.Popen([BIN, "--render-scenario", path, os.path.join(work, "frame.png")], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, cwd=SRC, start_new_session=True)
        try:
            rc = p.wait(timeout=180)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            rc = -9
    check("%s: client_bin exits 0" % tag, rc == 0, "rc=%s, %s" % (rc, work))
    doc = ""
    if rc == 0 and os.path.isfile(html_out):
        with open(html_out, encoding="utf-8", errors="replace") as f:
            doc = f.read()
    with open(log, errors="replace") as f:
        return doc, f.read(), work


def boxes(log):
    """The dumped box tree's element boxes: [(depth, kind, tag.classes, x, y, w, h)]."""
    out = []
    for line in log.splitlines():
        m = BOX.match(line)
        if m:
            out.append((len(m.group(1)), m.group(2), m.group(3)) + tuple(float(m.group(i)) for i in range(4, 8)))
    return out


def state(clicks):
    return {"menu": True, "connected": True, "ready": True, "inGame": False, "name": "Trader", "classIndex": 0,
            "region": "...", "mapName": "Kanji", "mapFile": "maps/311_kanji.json", "gameName": "ffa on Kanji",
            "anim_time": 0.8, "account": {"name": "Trader", "score": 2000000, "funds": 5000, "skins": [{"ind": TOPO, "cnt": 1}]},
            "wallet": {"open": True}, "windowClicks": clicks}


def shown(attrs):
    style = (attrs or {}).get("style", "").replace(" ", "").lower()
    return "display:block" in style


def main():
    # before: the Wallet's tile is there and no popup is up
    doc, log, work = render("before", state([]), dump=".wallet-item")
    d = Doc()
    d.feed(doc)
    check("before: the Wallet grid draws the Topo tile", "knV10WalletGrid" in d.by_id and
          'onclick="knV10WalletItem(%d)"' % TOPO in doc, work)
    check("before: the item popup is not up", not shown(d.by_id.get("itemViewPop")), d.by_id.get("itemViewPop"))

    # the tile's lock: v10.0.7's .lock-toggle.svelte-1wcm5z8 (absolute, top 44, right 8, 28 x 28)
    bx = boxes(log)
    tile = next((b for b in bx if b[0] == 0 and b[2].startswith("div.wallet-item")), None)
    lock = next((b for b in bx if b[2].startswith("button.lock-toggle")), None)
    check("lock: the tile and its lock are dumped", tile is not None and lock is not None, work)
    if tile and lock:
        tx, ty, tw, th = tile[3:]
        lx, ly, lw, lh = lock[3:]
        check("lock: the tile is the card's 128 x 128 (the lock takes no row)", (tw, th) == (128.0, 128.0), tile)
        check("lock: 28 x 28", (lw, lh) == (28.0, 28.0), lock)
        check("lock: 44 px under the tile's top", abs(ly - (ty + 44)) < 0.05, (ly, ty))
        check("lock: 8 px from the tile's right edge", abs((lx + lw) - (tx + tw - 8)) < 0.05, (lx, lw, tx, tw))

    # the click on the tile
    doc, log, work = render("click", state(["#knV10WalletGrid .market-card"]))
    d = Doc()
    d.feed(doc)
    check("click: the scenario's click found the tile", "windowClicks: no match" not in log, work)
    pop = d.by_id.get("itemViewPop")
    check("click: #itemViewPop is shown", shown(pop), pop)
    check("click: the popup names the tile's item", "Topo" in d.text.get("itemViewName", ""), d.text.get("itemViewName"))
    check("click: the Catalog's item page did not open", "items-detail-name" not in d.classes, work)
    if FAILS:
        print("wallet_item_e2e: %d failure(s)" % len(FAILS))
        return 1
    print("wallet_item_e2e: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

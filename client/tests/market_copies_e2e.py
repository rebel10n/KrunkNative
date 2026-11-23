#!/usr/bin/env python3
"""Krunker v10.0.7's item copies in the Market & Trading popup (client/src/ui/v10_market.c), in the rendered document of
client_bin --render-scenario (the "market" state, v10_market_scenario):

  1. My Inventory: a copy's card (svelte-r5taai, deob-10.0.7.js:110688-110693, drawn :110757-110790) shows its name tag
     ("Lucky", .itemNametag) above the name and below it .itemWear: the tier, "(0.2000)" (.wearFloat) and "#4"
     (.itemSerial); a copy with a serial and no wear shows the serial alone; a stack shows neither.
  2. Browse: a Box Office listing row's raw copy columns (wear, serial, nametag: kn_brain's LISTING_ROWS, 0398 / 0401)
     draw the same lines on the listing's card (the card's listing.m, :131335-131350).
  3. Listing Details: the listing row's .listing-nametag "Shiny", .listing-wear "Flawless (0.0500)" and .listing-serial
     "#3" after "<funds> KR " (svelte-kcz1lc, :130983-130991, :136679-136700).
  4. The item page for one copy (openSales' instance, :223620-223650): .items-detail-instance-overlay with the Name Tag,
     Mint, Wear (in the tier's colour) and Paint rows (:109885-109935).
  5. The item page of a wearable item (view stackable false, wearable true): the Mints and Wear tabs (:109727-109733);
     the Mints table's rows (# , Owner, Float in the tier's colour, Mint).

  python3 -I client/tests/market_copies_e2e.py BUILD_DIR [SOURCE_DIR]   (run from the repository root)
77 = skipped (no client_bin).
"""
import json
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "cmake-build-debug")
SRC = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else ROOT
BIN = os.path.join(BUILD, "client_bin")
SCRATCH = os.path.expanduser("~/.cache/kn-market-copies-e2e-%d" % os.getpid())
WEAR_ITEM = 7582  # Fractured: a v10.0.7 wearable Sniper skin
ITEM = 4
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + str(detail)) if detail and not ok else ""))


def render(name, market):
    """Renders the menu with the market state; returns the serialized document ("" on failure), quotes as '"'."""
    os.makedirs(SCRATCH, exist_ok=True)
    state = {"menu": True, "connected": True, "ready": True, "inGame": False, "name": "Trader", "classIndex": 0, "region": "...",
             "mapName": "Kanji", "mapFile": "maps/311_kanji.json", "gameName": "ffa on Kanji", "anim_time": 0.8,
             "account": {"name": "Trader", "score": 2000000, "funds": 5000,
                         "skins": [{"ind": ITEM, "cnt": 2}, {"ind": WEAR_ITEM, "cnt": 1, "id": 1000005, "m": {"wear": 0.2, "serial": 4}}]},
             "market": market}
    path = os.path.join(SCRATCH, name + ".json")
    with open(path, "w") as f:
        json.dump({"size": [1280, 720], "background": "#000000", "state": state, "frames": 3}, f)
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    html = os.path.join(SCRATCH, name + ".html")
    env.update(KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_DEBUG="1", XDG_CONFIG_HOME=os.path.join(SCRATCH, "cfg-" + name),
               KRUNKNATIVE_SCENARIO_HTML=html)
    png = os.path.join(SCRATCH, name + ".png")
    r = subprocess.run([BIN, "--render-scenario", path, png], cwd=SRC, env=env, capture_output=True, text=True, timeout=180)
    check(name + ": client_bin exits 0", r.returncode == 0, (r.stdout + r.stderr)[-800:] if r.returncode else "")
    if not os.path.isfile(html):
        check(name + ": the document was written", False)
        return ""
    with open(html, encoding="utf-8", errors="replace") as f:
        return f.read().replace("'", '"')


def around(doc, needle, before=200, after=500):
    k = doc.find(needle)
    return doc[max(0, k - before):k + after] if k >= 0 else doc[-300:]


CARD_TAG = '<div class="itemNametag svelte-r5taai">"%s"</div> <div class="itemName svelte-r5taai'
CARD_WEAR = ('<div class="itemWear svelte-r5taai">%s <span class="wearFloat svelte-r5taai">(%s)</span>'
             '<span class="itemSerial svelte-r5taai">#%d</span></div> <div class="itemType svelte-r5taai">')


def main():
    if not os.path.isfile(BIN):
        print("market_copies_e2e: no client_bin in %s: skipped" % BUILD)
        return 77

    # 1. My Inventory (tab 0, mtab 1): the named, worn copy; a serial-only copy; the stack
    inv = [{"i": ITEM, "c": 2, "qs": 100},
           {"i": WEAR_ITEM, "c": 1, "sid": 1000005, "st": 0, "qs": 50, "m": {"wear": 0.2, "serial": 4, "name": "Lucky"}},
           {"i": ITEM, "c": 1, "sid": 1000006, "st": 0, "qs": 100, "m": {"serial": 7}}]
    doc = render("inventory", {"tab": 0, "mtab": 1, "inventory": inv})
    check("inventory: the copy's name tag above its name (.itemNametag \"Lucky\")", (CARD_TAG % "Lucky") in doc, around(doc, "itemNametag"))
    check("inventory: its wear line: the tier, (0.2000) and #4 before the type (.itemWear)",
          (CARD_WEAR % ("Tested", "0.2000", 4)) in doc, around(doc, "itemWear"))
    check("inventory: a serial-only copy shows #7 alone",
          '<div class="itemWear svelte-r5taai"><span class="itemSerial svelte-r5taai">#7</span></div>' in doc, around(doc, "#7"))
    check("inventory: the stack has neither (two .itemWear, one .itemNametag)",
          doc.count('class="itemWear svelte-r5taai"') == 2 and doc.count('class="itemNametag svelte-r5taai"') == 1,
          "%d / %d" % (doc.count('class="itemWear'), doc.count('class="itemNametag')))

    # 2. Browse: kn_brain's listing rows with the raw copy columns
    listings = [{"i": WEAR_ITEM, "f": 500, "m": 21, "ms": 1760000000000, "p": "bob", "wear": 0.05, "serial": 3, "nametag": "Shiny",
                 "paint_color": None, "paint_shader": None},
                {"i": ITEM, "f": 300, "m": 22, "ms": 1760000001000, "p": "amy", "wear": None, "serial": None, "nametag": None}]
    doc = render("browse", {"tab": 0, "listings": listings})
    check("browse: the listed copy's card names it and shows Flawless (0.0500) #3",
          (CARD_TAG % "Shiny") in doc and (CARD_WEAR % ("Flawless", "0.0500", 3)) in doc, around(doc, "itemNametag"))
    check("browse: the plain listing's card has no copy line", doc.count('class="itemWear svelte-r5taai"') == 1, around(doc, "itemWear"))

    # 3. Listing Details of the wearable item: the row's spans after "500 KR "
    doc = render("detail", {"tab": 0, "item": WEAR_ITEM, "listings": listings[:1], "inventory": inv})
    want = ('<div class="listing-price svelte-kcz1lc">500 KR <span class="listing-nametag svelte-kcz1lc">"Shiny"</span> '
            '<span class="listing-wear svelte-kcz1lc">Flawless (0.0500)</span> <span class="listing-serial svelte-kcz1lc">#3</span> </div>')
    check("listing details: .listing-nametag, .listing-wear and .listing-serial after the price", want in doc, around(doc, "listing-price"))

    # 4. The item page for one copy (openSales' instance)
    inst = {"wear": 0.45, "serial": 1234, "nametag": "Lucky", "paint": "#dc3c3c", "paintShader": None, "seed": None}
    doc = render("instance", {"tab": 4, "item": WEAR_ITEM, "page": True, "instance": inst})
    ov = around(doc, "items-detail-instance-overlay", 0, 1500)
    check("instance: the overlay's Name Tag row (\"Lucky\")",
          '<span class="items-detail-instance-label svelte-dmu4q2">Name Tag</span> <span class="items-detail-instance-nametag svelte-dmu4q2">"Lucky"</span>' in ov, ov)
    check("instance: the Mint row (#1,234)",
          '<span class="items-detail-instance-label svelte-dmu4q2">Mint</span> <span class="items-detail-instance-value svelte-dmu4q2">#1,234</span>' in ov, ov)
    check("instance: the Wear row in the tier's colour (Trashed (0.4500), #faa332)",
          re.search(r'Wear</span> <span class="items-detail-instance-value svelte-dmu4q2" style="color: #faa332;">Trashed \(0\.4500\)</span>', ov) is not None, ov)
    check("instance: the Paint row names the store paint of #dc3c3c", re.search(r'Paint</span> <span class="items-detail-instance-value svelte-dmu4q2">'
                                                                             r'(<img class="items-detail-paint-thumb[^>]*>|<span class="items-detail-paint-swatch[^>]*></span>) '
                                                                             r'Red Paint</span>', ov) is not None, ov)
    doc = render("item-plain", {"tab": 4, "item": WEAR_ITEM, "page": True})
    check("the item page without an instance has no overlay", doc and "items-detail-instance-overlay" not in doc)

    # 5. A wearable item's page: the Mints and Wear tabs, the Mints table
    mints = [{"nm": "Trader", "wear": 0.05, "serial": 1}, {"nm": "bob", "wear": None, "serial": 2}]
    doc = render("mints", {"tab": 4, "item": WEAR_ITEM, "page": True, "stackable": 0, "wearable": True, "dtab": 5, "mints": mints})
    check("mints: the page has the Mints and Wear tabs", ">Mints<" in doc and ">Wear<" in doc, around(doc, "simple-tabs"))
    table = around(doc, "items-detail-instance-table", 0, 2500)
    check("mints: the table lists the owners with the float in the tier's colour and the serial",
          "Trader" in table and "bob" in table and 'style="color: #41c300;">0.0500</td>' in table and "#1" in table and "#2" in table, table)

    failed = [r for r in results if not r[1]]
    print("market_copies_e2e: %d/%d passed" % (len(results) - len(failed), len(results)))
    if not failed:
        shutil.rmtree(SCRATCH, ignore_errors=True)
    else:
        print("kept " + SCRATCH)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

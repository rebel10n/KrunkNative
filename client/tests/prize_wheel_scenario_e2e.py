#!/usr/bin/env python3
"""The prize wheel's spin screen (client/src/ui/store_ui.c prize_wheel, v10.0.7 prizeWheel deob-10.0.7.js:224276-224385)
on --render-scenario, read from the document the frame was captured from (KRUNKNATIVE_SCENARIO_HTML):

  heroic    the built-in Heroic pool wheel: a row per pool (itemPools[pool].name, its colour, its weight), the previews
            from the rarest pool (and the one before it: fewer than 4 items), "-500KR"; + More (showMoreItems) opens the
            items modal in v10's markup (svelte-9ff17r): "Items - Heroic", a section per pool, rarest first, 140 px cards
  ticket    prizeWheel(0, 7993, true): the Universal Spin Ticket's spinWheelId (2) wins over the index: "-10 Tickets",
            "Redeem x10"
  swu       the brain's "swu" (store.applySpinData) replaces the wheels: the new wheel's pools, #spinPromo (promoText on
            promoCol), infoText's lines (markup kept) at calc(100% + 72px) and "Available until: <endDate>"
  expired   a wheel past its endDate does not open (iîìiiii, deob-10.0.7.js:25150-25158)

  python3 -I client/tests/prize_wheel_scenario_e2e.py BUILD_DIR [SOURCE_DIR]   (run from the repository root)
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
SCRATCH = os.path.expanduser("~/.cache/krunknative/prize-wheel/e2e-%d" % os.getpid())
results = []

SPIN_DATA = {
    "season": 10,
    "wheels": [
        {"name": "Tester", "price": 5, "multi": 0, "pools": ["tA", "tB"], "poolWeights": [90, 10],
         "promoText": "Hot", "promoCol": "#123456", "infoText": "Line one\n<b>Two</b>",
         "startDate": "2020-01-01T00:00:00Z", "endDate": "2099-01-01T00:00:00Z"},
        {"name": "Gone", "price": 5, "rarities": [50, 50], "endDate": "2020-01-02T00:00:00Z"},
    ],
    "itemPools": {
        "tA": {"name": "Alpha", "color": "#00ff00", "items": [14, 2]},
        "tB": {"name": "Beta Pool", "color": "#ff00ff", "items": [114, 114, 311]},
    },
}


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + str(detail)) if detail and not ok else ""))


def render(name, **state):
    with open(os.path.join(SRC, "tools/uidiff/scenarios/spin-heroic.json")) as fp:
        scen = json.load(fp)
    scen["state"].update(state)
    scen["frames"] = 2
    os.makedirs(SCRATCH, exist_ok=True)
    path = os.path.join(SCRATCH, name + ".json")
    with open(path, "w") as fp:
        json.dump(scen, fp)
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    html = os.path.join(SCRATCH, name + ".html")
    env.update(KRUNKNATIVE_HEADLESS="1", XDG_CONFIG_HOME=os.path.join(SCRATCH, "cfg"), KRUNKNATIVE_SCENARIO_HTML=html)
    r = subprocess.run([BIN, "--render-scenario", path, os.path.join(SCRATCH, name + ".png")], cwd=SRC, env=env,
                       capture_output=True, text=True, timeout=240)
    check(name + ": client_bin exits 0", r.returncode == 0, (r.stdout + r.stderr)[-800:])
    try:
        with open(html, encoding="utf-8") as fp:
            return fp.read()
    except OSError:
        check(name + ": the document was written", False)
        return ""


def element(doc, elem_id):
    """the element's opening tag and content up to the next element with an id at the same level (good enough here)"""
    i = doc.find('id="%s"' % elem_id)
    if i < 0:
        return ""
    start = doc.rfind("<", 0, i)
    depth, j = 0, start
    for m in re.finditer(r"<(/?)([a-zA-Z0-9]+)[^>]*?(/?)>", doc[start:]):
        if m.group(2).lower() in ("img", "br", "input", "meta", "link", "hr") or m.group(3):
            continue
        depth += -1 if m.group(1) else 1
        if depth == 0:
            j = start + m.end()
            break
    return doc[start:j]


def main():
    if not os.path.isfile(BIN):
        print("prize_wheel_scenario_e2e: no client_bin in %s: skipped" % BUILD)
        return 77

    doc = render("heroic", spinActions=["knShowMoreItems"])
    info = element(doc, "spinInfo")
    rows = re.findall(r"<div class=['\"]colCub['\"] style=['\"]background-color:\s*([^'\";]+)['\"]></div>([^<]*)<span[^>]*>&emsp;([^<]*)%</span>",
                      info.replace(" ", "&emsp;"))
    check("heroic: a row per pool, itemPools[pool].name in its colour with its poolWeight",
          [(n.strip(), w) for _, n, w in rows] == [("Rare", "48"), ("Epic", "35"), ("Legendary", "14"), ("Relic", "2.49"),
                                                   ("Contraband", "0.5"), ("Unobtainable", "0.01")]
          and rows[0][0].lower() == "#2196f3" and rows[5][0].lower() == "#fff53d", rows or info[:600])
    check("heroic: \"-500KR\"", ">-500KR<" in element(doc, "spinCost"), element(doc, "spinCost")[:200])
    items = element(doc, "spinItems")
    shown = [int(x) for x in re.findall(r"viewItem\((\d+)\)", items)]
    check("heroic: 4 previews, + More", len(shown) == 4 and "knShowMoreItems()" in items, items[:400])
    check("heroic: the items modal is v10's (window-title, items-container)",
          '<span class="window-title-text svelte-l3klys">Items - Heroic</span>' in doc and 'class="items-container svelte-9ff17r"' in doc)
    sections = re.findall(r'<div class="rarity-header svelte-9ff17r" style="background-color: ([^;"]*);"><h2 class="rarity-title svelte-9ff17r">([^<]*) <!---->', doc)
    check("heroic: a section per pool, rarest first",
          [n for _, n in sections] == ["Unobtainable", "Contraband", "Relic", "Legendary", "Epic", "Rare"], sections)
    modal = doc[doc.find('class="items-container svelte-9ff17r"'):]
    check("heroic: 140 px market cards that open the item", "market-card" in modal and "viewItem(" in modal and "140px" in modal,
          modal[:600])
    unob = re.findall(r"viewItem\((\d+)\)", modal[:modal.find("Contraband <!---->")] if "Contraband <!---->" in modal else "")
    check("heroic: the Unobtainable pool's 3 items", len(set(unob)) == 3, unob)

    account = {"name": "Spinner", "funds": 1000, "skins": [{"ind": 7993, "cnt": 12}]}
    doc = render("ticket", spin=0, account=account, spinActions=[["prizeWheel", 0, 7993, True]])
    cost = element(doc, "spinCost")
    again = element(doc, "spinAgainBtn")
    check("ticket: the Universal Spin Ticket opens store.wheels[spinWheelId 2]: HEROIC",
          ">HEROIC<" in element(doc, "spinInfo"), element(doc, "spinInfo")[:200])
    check("ticket: \"-10 Tickets\" shown for the 10x ticket spin", ">-10 Tickets<" in cost and "display: none" not in cost, cost[:200])
    check("ticket: \"Redeem x10\" with the ticket's image", "Redeem x10<img" in again, again[:300])
    check("ticket: #spinKR counts the tickets", re.search(r">12</span> Tickets", element(doc, "spinKR")), element(doc, "spinKR")[:200])

    doc = render("swu", spin=0, spinData=SPIN_DATA)
    info = element(doc, "spinInfo")
    check("swu: the pushed wheel opens (TESTER)", ">TESTER<" in info, info[:200])
    check("swu: its pools' rows", re.search(r"#00ff00['\"]></div>Alpha<span[^>]*>&emsp;90%", info.replace(" ", "&emsp;"))
          and re.search(r"#ff00ff['\"]></div>Beta Pool<span[^>]*>&emsp;10%", info.replace(" ", "&emsp;")), info[:800])
    check("swu: infoText's lines (markup kept) at calc(100% + 72px), then Available until",
          '<div class="spinInfoText" style="top: calc(100% + 72px)"><span>Line one</span><span><b>Two</b></span>'
          '<span id="spinEndTime"><br>Available until: 2099-01-01T00:00:00Z</span></div>' in info, info[-500:])
    promo = element(doc, "spinPromo")
    check("swu: #spinPromo reads promoText on promoCol", ">Hot<" in promo and re.search(r"display:\s*block", promo)
          and re.search(r"background-color:\s*(#123456|rgb\(18,\s*52,\s*86\))", promo), promo[:300])
    check("swu: \"-5KR\"", ">-5KR<" in element(doc, "spinCost"))

    doc = render("expired", spin=1, spinData=SPIN_DATA)
    check("expired: a wheel past its endDate does not open", ">GONE<" not in element(doc, "spinInfo"), element(doc, "spinInfo")[:200])

    failed = sum(1 for _, ok, _ in results if not ok)
    print("prize_wheel_scenario_e2e: %d checks, %d failed" % (len(results), failed))
    if not failed:
        shutil.rmtree(SCRATCH, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

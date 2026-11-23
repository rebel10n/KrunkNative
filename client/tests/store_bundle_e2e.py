#!/usr/bin/env python3
"""The Store's bundles, Premium and Redeem pieces on --render-scenario fixtures, checked in the rendered document.

Each case writes a scenario (client/src/ui/ui_scenario.c), renders it with client_bin and reads the frame's serialized
DOM (KRUNKNATIVE_SCENARIO_HTML) for Krunker v10.0.7's markup:
  - the bundle popup (bundle_offers.c, deob-10.0.7.js:215745-215950): discount, "Time-limited offer!", the KR / Junk /
    Premium days lines, twelve item slots, the struck price, BUY / GIFT, the countdown in getReadableTime's format;
  - "gbdr" (kio_checkout.c, :215686-215705): the automatic popup with the conditional bundle and its 24-hour countdown,
    none within 8 hours of the last (lastBundlePopupShown) or when "gbdr" [6] says not to; every bundle owned:
    "You got them all!" / "More Incoming!"; the Store's grid draws the conditional bundles first (:129801);
  - a Box Office bundle's popup from the brain's rows;
  - Premium Benefits (svelte-1vap6hy, :128613-128630), View Tickets with every TICKET item (:220796-220814), the home
    dots switching the panel in place (:129779-129783), krunker.io's vouchPop (:224172-224181), the Eterno Shop's
    "Error: <message>" and Retry (:130637-130660).

  python3 -I client/tests/store_bundle_e2e.py BUILD_DIR [SOURCE_DIR]   (run from the repository root)
"""
import json
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "cmake-build-debug")
SRC = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else ROOT
BIN = os.path.join(BUILD, "client_bin")
SCRATCH = os.path.expanduser("~/.cache/krunknative/store-bundle/e2e-%d" % os.getpid())
results = []

KIO_STORAGE = {
    "kio_access_token": "h.eyJ1c2VybmFtZSI6IlBsYXllciIsImV4cCI6NDEwMjQ0NDgwMH0.s",
    "kio_username": "Player",
    "kio_level": "5",
}


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + str(detail)) if detail and not ok else ""))


def iso(ms):
    return time.strftime("%Y-%m-%dT%H:%M:%S.000Z", time.gmtime(ms / 1000.0))


def bundle(id_, name, **kw):
    row = {"ID": id_, "NAME": name, "PRICE": 4.99, "ORIGINAL_PRICE": None, "DISCOUNT_PCT": None, "SKINS": "1,2", "EXPIRY": None,
           "INFO": "Info", "FUNDS": 0, "JUNK": 0, "PREMIUM": 0, "EVENT_NAME": None, "EVENT_ENDS_AT": None, "DATE": "2026-10-01T00:00:00.000Z"}
    row.update(kw)
    return row


def render(name, state):
    """Renders the state; returns the serialized document ("" on failure)."""
    os.makedirs(SCRATCH, exist_ok=True)
    base = {"menu": True, "connected": True, "ready": True, "inGame": False, "name": "Player", "classIndex": 0, "region": "...",
            "mapName": "Kanji", "mapFile": "maps/311_kanji.json", "gameName": "ffa on Kanji", "anim_time": 0.8, "window": None}
    base.update(state)
    path = os.path.join(SCRATCH, name + ".json")
    with open(path, "w") as f:
        json.dump({"size": [1280, 720], "background": "#000000", "state": base}, f)
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
        return f.read()


def element(doc, id_):
    """The opening tag and the inner text up to the next element of the same id's closing (rough: to the next id='...')."""
    m = re.search(r'<[a-z]+ [^>]*id="%s"[^>]*>' % re.escape(id_), doc)
    return m.group(0) if m else ""


def shown(doc, id_):
    tag = element(doc, id_)
    m = re.search(r'display:\s*([a-z-]+)', tag)
    return bool(tag) and (not m or m.group(1) != "none")


def section(doc, start_id, length=12000):
    i = doc.find('id="%s"' % start_id)
    return doc[i:i + length] if i >= 0 else ""


def main():
    if not os.path.isfile(BIN):
        print("store_bundle_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    now = time.time() * 1000.0

    # 1. a krunker.io bundle's popup from its card (bundlePopup(true, false, 41)); "gbdr" [6] keeps the automatic one off
    timed = bundle(41, "Starter Pack", PRICE=4.99, ORIGINAL_PRICE=9.99, DISCOUNT_PCT=50, SKINS="1,2,99999", EXPIRY="2099-01-01T00:00:00.000Z",
                   INFO="Five <b>skins</b>", FUNDS=600, JUNK=2.5, PREMIUM=3)
    doc = render("popup", {"officialSession": True, "storage": KIO_STORAGE, "kioBundles": [["abd", [timed]], ["gbdr", [41], [], {}, [], 0, 1]],
                           "windowActions": [["bundlePopup", True, False, 41]]})
    pop = section(doc, "bundlePop")
    check("popup: #bundlePop is up", shown(doc, "bundlePop"), element(doc, "bundlePop"))
    check("popup: the discount label", "-50% OFF" in pop, pop[:300])
    check("popup: the time-limited label", "Time-limited offer!" in pop)
    check("popup: the KR line", re.search(r">600</span> <span[^>]*>KR</span>", pop) is not None)
    check("popup: the Junk line (toFixed(2), plumbing)", re.search(r">2\.50</span> <span[^>]*>plumbing</span>", pop) is not None)
    check("popup: the Premium days line", re.search(r">3</span> <span[^>]*>Premium Days</span>", pop) is not None)
    check("popup: INFO as HTML", "<b>skins</b>" in pop)
    check("popup: twelve item slots", pop.count('class="bundlItmC"') == 12, pop.count('class="bundlItmC"'))
    check("popup: an empty slot's grey border", pop.count("border: 3px solid #707070") == 10, pop.count("border: 3px solid #707070"))
    check("popup: the struck price", "previously $9.99" in pop)
    check("popup: BUY and GIFT", "BUY $4.99 USD" in pop and re.search(r'class="bundlPopBuyGift"[^>]*>GIFT</div>', pop) is not None)
    check("popup: the countdown's label", "Offer ends in:" in pop)
    m = re.search(r'id="timer-text"[^>]*>([^<]*)<', pop)
    check("popup: the countdown in getReadableTime's format", m is not None and re.fullmatch(r"\d+y (\d+d )?(\d+h )?\d+m ", m.group(1)) is not None,
          m.group(1) if m else "no #timer-text")
    check("popup: the timer's skull", "img/skull-small.svg" in pop and "img/warning-octagon-red.svg" in pop)

    # 1b. a collection folds into one slot with "+N" (the collection's other store skins) and its later skins take no
    # slot (deob-10.0.7.js:215897-215919): Group C (5284, 5302, 5305, 5308 in skins.json), then skin 1
    coll = bundle(42, "Group Pack", SKINS="5284,5302,1")
    doc = render("collection", {"officialSession": True, "storage": KIO_STORAGE, "kioBundles": [["abd", [coll]], ["gbdr", [42], [], {}, [], 0, 1]],
                                "windowActions": [["bundlePopup", True, False, 42]]})
    pop = section(doc, "bundlePop")
    check("collection: twelve item slots", pop.count('class="bundlItmC"') == 12, pop.count('class="bundlItmC"'))
    check("collection: one slot for Group C with +3", pop.count('class="bundlItmCnt"') == 1 and ">+3</div>" in pop,
          re.findall(r'class="bundlItmCnt"[^<]*<', pop))
    check("collection: 5302 takes no slot", "viewItem(5284)" in pop and "viewItem(5302)" not in pop and "viewItem(1)" in pop)
    check("collection: ten grey slots", pop.count("border: 3px solid #707070") == 10, pop.count("border: 3px solid #707070"))

    # 2. "gbdr" with a conditional bundle: the automatic popup picks it, with its 24-hour countdown
    cond = bundle(30, "Comeback Deal", PRICE=1.99, SKINS="3")
    main_row = bundle(16, "Main Pack", PRICE=9.99, DATE="2026-11-01T00:00:00.000Z")
    gbdr = ["gbdr", [30, 16], [cond], {"30": [{"condition_type": "valid_for_24_hours"}]}, [{"bundle_id": 30, "first_seen": iso(now - 3600 * 1000)}]]
    doc = render("auto", {"officialSession": True, "storage": KIO_STORAGE, "kioBundles": [["abd", [main_row]], gbdr]})
    pop = section(doc, "bundlePop")
    check("auto: #bundlePop is up", shown(doc, "bundlePop"), element(doc, "bundlePop"))
    check("auto: the conditional bundle", "Comeback Deal" in pop, pop[:400])
    m = re.search(r'id="timer-text"[^>]*>([^<]*)<', pop)
    check("auto: 23 hours left", m is not None and m.group(1).startswith(("22h 59m", "23h 0m", "22h 58m")), m.group(1) if m else "no #timer-text")
    check("auto: the shop button's note", "Time-limited Offer Available!" in doc and "Time left: 2" in doc)

    # 3. within 8 hours of the last one, or "gbdr" [6]: no popup
    doc = render("capped", {"officialSession": True, "storage": dict(KIO_STORAGE, lastBundlePopupShown="%.0f" % (now - 60000)),
                            "kioBundles": [["abd", [main_row]], gbdr]})
    check("capped: no popup within 8 hours", not shown(doc, "bundlePop") or "Comeback Deal" not in section(doc, "bundlePop"))
    doc = render("flag6", {"officialSession": True, "storage": KIO_STORAGE, "kioBundles": [["abd", [main_row]], gbdr + [0, 1]]})
    check("flag6: no popup", not shown(doc, "bundlePop") or "Comeback Deal" not in section(doc, "bundlePop"))

    # 4. every bundle owned: the home card's text
    doc = render("owned", {"officialSession": True, "storage": KIO_STORAGE, "kioBundles": [["abd", [main_row]], ["gbdr", [], [], {}, [], 0, 1]]})
    check("owned: You got them all!", re.search(r'id="homeStoreAdTx"[^>]*>You got them all!<', doc) is not None, element(doc, "homeStoreAdTx"))
    check("owned: More Incoming!", re.search(r'id="bundleQuickBuyB"[^>]*>More Incoming!<', doc) is not None, element(doc, "bundleQuickBuyB"))

    # 5. the Store's grid: the conditional bundles ahead of the store's
    doc = render("grid", {"officialSession": True, "storage": KIO_STORAGE, "window": 14, "kioBundles": [["abd", [main_row]], gbdr + [0, 1]]})
    grid = doc[doc.find('class="bundleCols'):]
    a, b = grid.find("b_30.png"), grid.find("b_16.png")
    check("grid: the conditional bundle first", 0 <= a < b, (a, b))

    # 6. a Box Office bundle's popup from the brain's rows
    bo = bundle(7, "Box Pack", PRICE=2.99, FUNDS=100, OWNED=True)
    doc = render("boxoffice", {"v10responses": {"/api/shop/bundles": {"data": [bo]}}, "windowActions": [["bundlePopup", True, False, 7]]})
    pop = section(doc, "bundlePop")
    check("boxoffice: the popup", shown(doc, "bundlePop") and "Box Pack" in pop, element(doc, "bundlePop"))
    check("boxoffice: owned: GIFT $2.99 USD, no BUY", "GIFT $2.99 USD" in pop and "BUY $" not in pop)

    # 7. Premium Benefits: every benefit, no account needed
    doc = render("benefits", {"windowActions": [["showPremiumBenefits"]]})
    check("benefits: the modal", "Premium Benefits" in doc and "svelte-1vap6hy" in doc)
    check("benefits: 18 benefits", doc.count("premText svelte-1vap6hy") == 18, doc.count("premText svelte-1vap6hy"))
    check("benefits: the last one", "Increased Asset Upload Limit" in doc)

    # 8. View Tickets: every TICKET item, a count of 0 and one without a wheel too
    doc = render("tickets", {"account": {"name": "Player", "funds": 10, "skins": [{"ind": 5321, "cnt": 0}, {"ind": 6913, "cnt": 2}, {"ind": 1, "cnt": 1}]},
                             "windowActions": [["windows.13.viewTickets"]]})
    check("tickets: two ticket cards", doc.count('class="ticketCard"') == 2, doc.count('class="ticketCard"'))
    check("tickets: the count 0 shows", ">x0<" in doc and ">x2<" in doc)

    # 9. the home dots: the panel switches in place
    doc = render("dots", {"window": 14, "windowActions": [["knStoreSubTab", 2]]})
    panels = ["none" if re.search(r'display:\s*none', t) else "" for t in re.findall(r'<div class="sub-tab-panel svelte-lri212"[^>]*>', doc)]
    check("dots: the third panel shows", panels == ["none", "none", ""], panels)

    # 10. krunker.io's Redeem Keys: v10's vouchPop
    doc = render("vouch", {"officialSession": True, "storage": KIO_STORAGE, "windowActions": [["voucherPopup"]]})
    check("vouch: the code box", 'id="redVouch"' in doc and "Enter KR Code" in doc and "redVouchBtn" in doc)

    # 11. the Eterno Shop's failure: the brain's message and a Retry
    doc = render("eterno", {"window": 14, "windowActions": [["windows.13.switchTab", 5]],
                            "v10responses": {"/api/shop/displays?tag=limited": {"__status": 503, "error": "Shop is restocking"}}})
    check("eterno: Error: <message>", "Error: Shop is restocking" in doc, re.findall(r'error-text[^>]*>[^<]*', doc)[:2])
    check("eterno: Retry fetches again", "knEternoRetry()" in doc)

    failed = sum(1 for _, ok, _ in results if not ok)
    print("store_bundle_e2e: %d checks, %d failed" % (len(results), failed))
    if not failed:
        subprocess.run(["rm", "-rf", SCRATCH], check=False)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

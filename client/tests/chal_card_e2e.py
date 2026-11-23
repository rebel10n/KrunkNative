#!/usr/bin/env python3
"""v10.0.7's challenge cards on --render-scenario fixtures (client/src/ui/v10_rewards.c chal_card, battlepass_ui.c
gen_daily_card; account_ui.c account_challenge_progress / account_challenge_goal).

The Challenges window (tools/uidiff/scenarios/rewards-chall-home.json) and the Battle Pass Challenges tab
(rewards-bp-chal.json) are rendered with a few challenges changed, and the captured document
(KRUNKNATIVE_SCENARIO_HTML) must show:
  - genChallengeCard's title and RP tiles (deob-10.0.7.js:221432-221444): a REW.title tile opening the Titles
    window, the RP tile in currencies.RP.color #4caf50 (:15601) with commaNumber(rp) (:23949)
  - displayProgress / displayGoal (:180768-180806): a DAT.bits challenge counts the set bits (PV 5 of val 7 = 2/3)
  - the bar's Math.min(100, progress / goal * 100) (:221449, :219241): PV past the goal fills 100 %, not more
  - genDailyCard's REW.sk 0 is no reward (`REW.sk || null`, :219236)

  python3 -I client/tests/chal_card_e2e.py BUILD_DIR [SOURCE_DIR]   (run from the repository root)
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
SCRATCH = os.path.expanduser("~/.cache/krunknative/chal-card/e2e-%d" % os.getpid())
results = []
CARD = re.compile(r"class=[\"']chalCard[\"' ]")


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + str(detail)[:600]) if detail and not ok else ""))


def render(base, name, edit):
    """the scenario `base` with `edit(challenges)` applied -> the captured document"""
    with open(os.path.join(SRC, "tools/uidiff/scenarios", base + ".json")) as f:
        sc = json.load(f)
    edit({c["CI"]: c for c in sc["state"]["account"]["challenges"]})
    os.makedirs(SCRATCH, exist_ok=True)
    path, png, html = (os.path.join(SCRATCH, name + ext) for ext in (".json", ".png", ".html"))
    with open(path, "w") as f:
        json.dump(sc, f)
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env.update(KRUNKNATIVE_HEADLESS="1", KRUNKNATIVE_SCENARIO_HTML=html, XDG_CONFIG_HOME=os.path.join(SCRATCH, "cfg"))
    r = subprocess.run([BIN, "--render-scenario", path, png], cwd=SRC, env=env, capture_output=True, text=True, timeout=180)
    check(name + ": client_bin exits 0", r.returncode == 0, (r.stdout + r.stderr)[-800:])
    try:
        with open(html, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        check(name + ": the document was written", False, html)
        return ""


def card(doc, text):
    """the chalCard holding `text` (up to the next card)"""
    at = doc.find(text)
    if at < 0:
        return ""
    starts = [m.start() for m in CARD.finditer(doc, 0, at)]
    nxt = CARD.search(doc, at)
    return doc[starts[-1] if starts else at:nxt.start() if nxt else len(doc)]


def width(cardhtml):
    m = re.search(r"chalXPBarC[\"'][^>]*width:\s*([0-9.]+)%", cardhtml)
    return float(m.group(1)) if m else None


def home(ch):
    # Get a 15 killstreak: PV past the goal, KR + 1,500 RP + title 90
    ch[13].update(PV=20, PT=0, REW={"kr": 50, "rp": 1500, "title": [90]})
    # the boss challenge as a DAT.bits one: val 7 = 3 bits to set, PV 5 = 2 of them
    ch[14].update(PV=5, PT=0, N="Find the three relics", DAT={"ky": "rlc", "val": 7, "bits": 1, "sm": 1, "boss": 1})


def bp(ch):
    ch[15].update(PV=40, PT=0)  # 15 headshots: PV past the goal
    ch[16].update(PT=0, PV=1, REW={"bpp": 50, "sk": 0})  # REW.sk 0: no skin tile


def main():
    if not os.path.isfile(BIN):
        print("chal_card_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    doc = render("rewards-chall-home", "chall-home", home)
    ks = card(doc, "Get a 15 killstreak")
    check("RP tile: #4caf50 and commaNumber", "chalRewRP" in ks and "#4caf50" in ks and "1,500" in ks, ks)
    check("title tile opens the Titles window", "showTitlesWindow()" in ks and "workspace_premium" in ks, ks)
    check("PV past the goal: the text is PV/goal", "20/15" in ks, ks)
    check("PV past the goal: the bar stops at 100%", width(ks) == 100.0, width(ks))
    relic = card(doc, "Find the three relics")
    check("DAT.bits: set bits of PV over set bits of val", "2/3" in relic, relic)
    check("DAT.bits: the bar at 2/3", width(relic) is not None and abs(width(relic) - 200.0 / 3) < 0.01, width(relic))

    doc = render("rewards-bp-chal", "bp-chal", bp)
    hs = card(doc, "40/15")
    check("daily card: PV past the goal keeps PV/goal", bool(hs), "no 40/15 card")
    check("daily card: the bar stops at 100%", width(hs) == 100.0, width(hs))
    score = card(doc, "1/3000")
    check("daily card: REW.sk 0 draws no skin", bool(score) and "chalRewSkn" not in score, score)

    failed = [r for r in results if not r[1]]
    print("chal_card_e2e: %d/%d passed" % (len(results) - len(failed), len(results)))
    if not failed:
        shutil.rmtree(SCRATCH, ignore_errors=True)
    else:
        print("work dir kept: " + SCRATCH)
    return 1 if failed else 0


sys.exit(main())

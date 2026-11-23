#!/usr/bin/env python3
"""The raffle window (client/src/ui/v10_misc_popups.c, v10.0.7's svelte-1rj22es) on --render-scenario answers.

Each case renders state.community.popups.lottery (the GET /api/lottery/3 answer) headless and reads the
document the frame was captured from (KRUNKNATIVE_SCENARIO_HTML):

  open      a cost's name: a skin's own name in its rarity colour (Contraband `dark`), a currency's "KR Currency";
            "(N entries/each)"; "Available: <funds>"; the cost cards without the item info, the Grand Prize card with
            it and viewItem(); the Grand Prize is the lowest prize_pool_id; the muted line with no frequency; the
            stepper / Add All / Enter play playSelect(0.1) (deob-10.0.7.js:127740-127757, :128041-128043, :127826)
  drawn     an ended, drawn window: "Next Draw: Drawing in progress" (deob-10.0.7.js:127996-128000)
  drawing   an ended window not drawn yet: "Status: Drawing in progress"
  ended     data.event_ended with no window: "The raffle event has ended" (deob-10.0.7.js:128541)

  python3 -I client/tests/lottery_scenario_e2e.py BUILD_DIR [SOURCE_DIR]   (run from the repository root)
"""
import copy
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
SCRATCH = os.path.expanduser("~/.cache/krunknative/lottery/e2e-%d" % os.getpid())
results = []

LOTTERY = {
    "id": 3,
    "lottery": {"id": 3, "title": "Anniversary", "theme": "neutral"},
    "event_ended": False,
    "costs": [
        {"cost_type": "skin", "cost_ref": "114", "cost_quantity": 1, "num_entries": 1},   # Flame Tamer, Contraband
        {"cost_type": "skin", "cost_ref": "14", "cost_quantity": 1, "num_entries": 2},    # Blaze AK, Rare
        {"cost_type": "currency", "cost_ref": "1", "cost_quantity": 100, "num_entries": 5},
    ],
    "current_draw": {
        "id": 7, "ends_at": "2030-01-01T00:00:00Z", "drawn_at": None, "player_entries": 0, "total_entries": 12,
        "prizes": [
            {"prize_pool_id": 2, "prize_type": "skin", "prize_ref": "2", "prize_quantity": 1},
            {"prize_pool_id": 1, "prize_type": "skin", "prize_ref": "311", "prize_quantity": 1},
        ],
    },
    "previous_draws": [],
}


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print("%s %s%s" % ("PASS" if ok else "FAIL", name, (" -- " + str(detail)) if detail and not ok else ""))


def render(name, lottery, seconds_left):
    with open(os.path.join(SRC, "tools/uidiff/scenarios/popup-lottery.json")) as fp:
        scen = json.load(fp)
    popups = scen["state"]["community"]["popups"]
    popups.pop("lotteryError", None)
    popups["lottery"] = lottery
    popups["lotterySecondsLeft"] = seconds_left
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


def between(doc, start, end):
    i = doc.find(start)
    j = doc.find(end, i + 1) if i >= 0 else -1
    return doc[i:j] if i >= 0 and j > i else ""


def main():
    if not os.path.isfile(BIN):
        print("lottery_scenario_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    doc = render("open", LOTTERY, 3600)
    check("open: a Contraband cost's name is `dark`", re.search(r'class="cost-name svelte-1rj22es dark"[^>]*>Flame Tamer<', doc),
          re.findall(r'<div class="cost-name[^>]*>[^<]*<', doc))
    m = re.search(r'class="cost-name svelte-1rj22es" style="color: (#[0-9a-fA-F]+)">Blaze AK<', doc)
    check("open: a skin cost's name is its rarity colour", m and m.group(1).lower() != "#ffffff", m and m.group(1))
    check("open: a currency cost is named \"KR Currency\"", ">KR Currency<" in doc)
    check("open: entries per unit read \"(N entries/each)\"",
          "(1 entry/each)" in doc and "(2 entries/each)" in doc and "(0.05 entries/each)" in doc,
          re.findall(r'cost-entries[^>]*>([^<]*)<', doc))
    check("open: the KR cost's Available is the account's funds", "Available: 2500" in doc)
    costs = between(doc, 'class="costs-row', 'class="enter-lottery-button')
    check("open: the cost cards have no item info (showItemInfo false)", costs and "market-card" in costs and "itemInfo" not in costs)
    grand = between(doc, 'class="grand-prize-display', 'class="additional-prizes-box')
    check("open: the Grand Prize is the lowest prize_pool_id, with its info and viewItem",
          "viewItem(311)" in grand and "itemInfo" in grand, grand[:300])
    check("open: the muted line shows without a frequency",
          re.search(r'description-muted svelte-1rj22es">A new draw happens every\s+hours, with 1 grand prize winner and up to 1 extra winner', doc),
          between(doc, "description-muted", "</span>"))
    check("open: the stepper, Add All and Enter play playSelect(0.1)",
          "playSelect(0.1);knMiscLotQty(0,1)" in doc and "playSelect(0.1);knMiscLotAll(0)" in doc and "playSelect(0.1);knMiscLotEnter()" in doc)
    check("open: the countdown runs", re.search(r'Next Draw:</span> <span class="timer-value svelte-1rj22es"[^>]*>1h 0m 0s<', doc))

    drawn = copy.deepcopy(LOTTERY)
    drawn["current_draw"]["drawn_at"] = "2029-12-31T23:59:00Z"
    doc = render("drawn", drawn, -5)
    check("drawn: an ended, drawn window reads \"Next Draw: Drawing in progress\"",
          re.search(r'Next Draw:</span> <span class="timer-value svelte-1rj22es">Drawing in progress<', doc) and "Status:" not in doc)
    doc = render("drawing", LOTTERY, -5)
    check("drawing: an ended window not drawn yet reads \"Status: Drawing in progress\"",
          "drawing-status" in doc and "Status:" in doc and "Next Draw:" not in doc)
    ended = copy.deepcopy(LOTTERY)
    del ended["current_draw"]
    ended["event_ended"] = True
    doc = render("ended", ended, 0)
    check("ended: data.event_ended shows the event's end", "The raffle event has ended. Thank you for participating!" in doc)

    failed = sum(1 for _, ok, _ in results if not ok)
    print("lottery_scenario_e2e: %d checks, %d failed" % (len(results), failed))
    shutil.rmtree(SCRATCH, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

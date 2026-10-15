#!/usr/bin/env python3
"""Generates the S5 uidiff scenarios (notification center, game announcement banner, emoji panel,
item tooltips): tools/uidiff/scenarios/kn-notify-*.json, kn-tip-*.json and the matching regions.

    python3 -I tools/uidiff/gen_v10_notify.py

The state is injected through `state.notify` (client/src/ui/v10_notify.c v10_notify_scenario). The
notification center and the announcement are checked against v10's own captures
(tools/uidiff/capture-v10.js notify-center / notify-announcement: the bundle's notification and
announcement stores driven through __knScope, doc/ref/krunker-v10/notify/). The emoji panel and the
tooltips use KrunkNative's own llvmpipe renders (doc/ref/krunknative/), a regression baseline for
the markup, classes and geometry.
The JSON files are committed; edit this script and re-run instead of editing them by hand.
"""
import copy
import json
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SC = os.path.join(HERE, "scenarios")
RG = os.path.join(HERE, "regions")
REF = "doc/ref/krunknative"
V10_REF = "doc/ref/krunker-v10/notify"  # capture-v10.js dir: notify/


def owned(indices):
    """the stand-in account's inventory [{ind, cnt}] (account_ui_scenario): emoji are usable when owned
    in online mode (the default; a guest owns none), the hat picker lists owned cards"""
    return [{"ind": i, "cnt": 1} for i in indices]


def emoji_indices():
    with open(os.path.join(REPO, "assets", "skins.json"), encoding="utf-8") as fh:
        skins = json.load(fh)
    return [i for i, s in enumerate(skins) if s.get("type") == 17]  # EMOJI


def load(name):
    with open(os.path.join(SC, name + ".json")) as fh:
        return json.load(fh)


def region(name, rect, note, **kw):
    return dict({"name": name, "rect": rect, "note": note}, **kw)


def emit(name, base, notify, regions, note, ref=None, **state_kw):
    """ref: the v10 capture (tools/uidiff/capture-v10.js dir notify/, its name); None = KN's own render"""
    sc = load(base)
    st = copy.deepcopy(sc["state"])
    st.update(state_kw)
    st["notify"] = notify
    if ref:
        where, src = "%s/%s.png" % (V10_REF, ref), "Reference: v10's own capture (capture-v10.js %s, the bundle's notification store driven through __knScope)." % ref
    else:
        where, src = "%s/%s.png" % (REF, name), "Reference: KN's own llvmpipe render (doc/ref/krunknative/), a regression baseline."
    out = {"ref": where, "size": [1280, 720], "background": sc.get("background", "#000000"),
           "state": st, "frames": 3, "note": note + " " + src}
    with open(os.path.join(SC, name + ".json"), "w") as fh:
        json.dump(out, fh, indent=2)
        fh.write("\n")
    with open(os.path.join(RG, name + ".json"), "w") as fh:
        json.dump({"scenario": name, "defaults": {"fuzz": 6, "max_pct": 0.5}, "regions": regions}, fh, indent=2)
        fh.write("\n")


# v10 g:197557 (nrigjp) stacks the newest first; the bars are frozen at remainingPct (the countdown runs 100 -> 0)
NOTIFICATIONS = [
    {"title": "Friend Request", "text": "Wintercrystal followed you. Follow them back to become friends.", "duration": 10000, "remainingPct": 100},
    {"title": "Message from Bot", "text": "gg, rematch?", "duration": 6000, "remainingPct": 62.5},
    {"title": "Notification", "text": "Settings Loaded", "duration": 4000, "remainingPct": 15},
]


def main():
    os.makedirs(RG, exist_ok=True)
    emit("kn-notify-center", "ingame-plain", {"notifications": NOTIFICATIONS},
         [region("center", [8, 440, 240, 170], "#notificationCenter: three stacked notifications, newest on top, bars at 100 / 62.5 / 15 %. max_pct 5.5: the layout matches Chrome's to the sub-pixel (rects probed in the capture); what is left is rasterisation: the titles' synthetic bold (font-weight 600, GameFont has no bold face) is anchored bottom-left by FT_Outline_EmboldenXY where Chrome centres it (~3 %: centring it in text.c drops the title rows to 0.1 %), and the 1.74 px expiration bars at fractional y under the in-game 0.579 scale, which Chrome antialiases over three rows (~1.5 %)", max_pct=5.5)],
         "In game, v10's notification center with three stacked notifications (newest on top, countdown bars frozen).",
         ref="notify-center")
    emit("kn-notify-announcement", "ingame-plain", {"announcement": "Double XP weekend is live! Play now."},
         [region("banner", [8, 566, 240, 48], ".game-announcement.show with its .ga-message line, bottom left above the chat. max_pct 5.5: the label's synthetic bold (font-weight bold) rasterised uncentred (~2.8 %, see kn-notify-center) and its 0.58 px underline (1px under the 0.579 scale) that Chrome spreads over two rows (~1.6 %); a full-width underline (max-width: fit-content lost) is 6 %", max_pct=5.5)],
         "In game, v10's game announcement banner showing a server message.", ref="notify-announcement")
    emit("kn-notify-emoji", "ingame-chat-open", {"emoji": {"chat": "gg :ok", "selected": 1}},
         [region("panel", [0, 380, 300, 240], "#emojiPanelHolder: the filtered emoji list above the chat, the second one selected")],
         "Chat open, ':ok' typed: v10's emoji panel filtered by the word, the second emoji selected.",
         account={"name": "Local User", "skins": owned(emoji_indices())})
    emit("kn-tip-item", "customize-hat", {"tip": {"selector": ".skinCard[data-index]", "kind": "item"}},
         [region("tooltip", [752, 118, 156, 56], "createTip over the first hat card (Medic Helmet): #knTip 9 px above the card, name in its rarity colour, Est KR")],
         "Hat picker with createTip on the first card: name in its rarity colour, lore, Est KR.",
         account={"name": "Local User", "skins": owned([111, 113, 115, 117, 120, 121])})
    emit("kn-tip-streak", "customize-streaks", {"tip": {"selector": "[data-index][onmouseover*=createStreakTip]", "kind": "streak"}},
         [region("tooltip", [552, 110, 348, 64], "createStreakTip over the first killstreak (Slimer): name and description above the card")],
         "Killstreak window with createStreakTip over the first streak: name and description.")
    sp = load("spin-starter")["state"]
    sp = dict(sp, spinHistory=[{"ind": 111, "dte": 1000000000000, "fds": 500}, {"ind": 112, "dte": 1000000000000, "fds": 1200}])
    emit("kn-tip-spin", "spin-starter", {"tip": {"selector": "#spinHistoryList .spinPrevIcon", "kind": "spin"}},
         [region("name-est", [575, 264, 150, 36], "createSpinHistoryTip over the first Spin History item: name in its rarity colour, Est KR (left 150 px: the tooltip's width follows the Date line)"),
          region("cost", [575, 316, 150, 23], "Cost: <fds> KR and the bottom edge; the Date line above counts from the wall clock and sets the tooltip's width, so it and the right edge are left out")],
         "Spin History popup with createSpinHistoryTip over the first item (the Date line varies with the clock).",
         spinHistory=sp["spinHistory"])


if __name__ == "__main__":
    main()

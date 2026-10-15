#!/usr/bin/env python3
"""Generates the frame-sequence uidiff scenarios: tools/uidiff/scenarios/seq-*.json and
tools/uidiff/regions/seq-*.json (README.md "Frame sequences").

    python3 -I tools/uidiff/gen_sequences.py

References: doc/ref/krunker-v10.0.2/sequences/<name>/<t>.png, from the OFFLINE v10 sandbox
(tools/uidiff/capture-v10-seq.js: CDP virtual time, CSS animations driven at their virtual age).
Each scenario renders the same frames natively on the virtual clock (client/src/ui/ui_scenario.c
anim_times / anim_origin) and uidiff diffs them per frame plus the "timing" assertions. The
JSON files are committed; edit this script and re-run instead of editing them by hand.

Keys a scenario adds to the single-frame format: "anim_times", "anim_origin", "pending" (the
native side is not built yet: the owner removes it, UI-PARITY-AUDIT section 4) and
"unreachable" (the offline v10 sandbox cannot produce the state, so there is no reference:
reported, never faked). A region's "pending" skips just that region.
"""
import json
import os
import re
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REF = "doc/ref/krunker-v10.0.2/sequences/"
KIND = {"text": (12, 3.0), "flat": (6, 0.5), "image": (8, 1.5), "scene": (12, 3.0)}  # fuzz %, max differing %
scen, regions = {}, {}


def rng(a, b, s):
    return list(range(a, b + 1, s))


MENU = {"menu": True, "connected": True, "ready": True, "inGame": False, "name": "Guest", "classIndex": 0,
        "region": "...", "mapName": "Kanji", "mapFile": "maps/311_kanji.json", "gameName": ""}
GAME = {"menu": False, "window": None, "connected": True, "ready": True, "inGame": True, "mapName": "Kanji",
        "mapFile": "maps/311_kanji.json", "modeName": "Free for All", "name": "Local User", "region": "...",
        "hud": {"timerText": "03:45"}, "chat": []}
BOT_KILL = ["notarget on", "bot 1", {"at_ms": 1500, "cmd": "kill @bots"}]


def R(name, rect, kind="text", note=None, max_pct=None, fuzz=None, **extra):
    f, m = KIND[kind]
    r = {"name": name, "rect": [int(v) for v in rect], "kind": kind,
         "fuzz": fuzz if fuzz is not None else f, "max_pct": max_pct if max_pct is not None else m}
    if note:
        r["note"] = note
    r.update(extra)
    return r


def T(name, rect, threshold=0.5, tolerance_ms=None, **extra):
    """A timing assertion: the rect's change from the first frame crosses `threshold` of its peak
    within tolerance_ms (default one 60 Hz frame) of the reference."""
    t = {"name": name, "rect": [int(v) for v in rect], "threshold": threshold}
    if tolerance_ms is not None:
        t["tolerance_ms"] = tolerance_ms
    t.update(extra)
    return t


def sc(name, times, origin, state, regs, timing=(), bg="checker", size=(1280, 720), note="", pending=None,
       unreachable=None):
    d = {"ref": REF + name, "size": list(size), "background": bg, "anim_times": times, "anim_origin": origin,
         "state": state}
    if pending:
        d["pending"] = pending
    if unreachable:
        d["unreachable"] = unreachable
    d["note"] = note
    scen["seq-" + name] = d
    regions["seq-" + name] = {"regions": list(regs), "timing": list(timing)}


# ---- A. boot / loading / instructions

sc("boot-curtain", [0, 100, 200, 350, 500, 700, 900], "init", dict(MENU),
   [R("view", [900, 130, 300, 260], "flat", "the checker under #instructionsFadeBG (#222): the curtain's opacity", fuzz=3)],
   [T("curtain", [900, 130, 300, 260])],
   note="The boot curtain #instructionsFadeBG (#222 over the 3D view, here the checker) fading out over 0.7 s "
        "ease-in-out after the first INIT (v10:219396). Reference: capture-v10-seq.js boot-curtain.")
sc("loading-map-name", [0], "boot", dict(MENU, ready=False, boot={"loadGamNm": "play", "map": "Kanji"}),
   [R("loading", [540, 260, 200, 110], "text", "#loadGamNm \"Loading Kanji ...\" on #loadingBg")],
   bg="#222222",
   note="The loading screen of a ?play=Kanji link (v10:197704). Reference: capture-v10-seq.js loading-map-name.")
sc("click-to-play-grow", rng(0, 1400, 200), "cycle", dict(MENU),
   [R("click-to-play", [500, 270, 280, 60], "text", "#instructions CLICK TO PLAY while it grows", max_pct=4.0)],
   [T("grow", [500, 270, 280, 60])], bg="#222222",
   note="CLICK TO PLAY (#instructions grow 0.8 s infinite alternate): one 1.6 s cycle from the animation start "
        "(origin cycle = capture-v10-seq.js restartAnims).")
sc("auto-respawn", [0, 1000, 2000, 2900], "death", dict(GAME, autoRespawn=3, console=["kill"]),
   [R("subtitle", [480, 280, 320, 80], "text", "#instructions \"Auto Respawning in N\"")],
   unreachable="v10 never shows \"Auto Respawning in {0}\" on a death: the death handler calls _67300(forceSpawn + 1) "
               "(v10:220379) while _69619 (in play) is still true, so _67303 stores null (v10:209740) and the 1 s chain "
               "stops after its first tick_0. Tried in the sandbox with forceSpawn 3, locked and unlocked: "
               "#instructions stays CLICK TO PLAY. The native state.autoRespawn (S1) is covered by its own scenario",
   note="Auto Respawn 3 s (state.autoRespawn is S1's, ui-s1-boot): no v10 reference is reachable.")
sc("lost-connection", [0, 500], "applied", dict(MENU, connected=False, connectError="DISCONNECTED"),
   [R("box", [450, 270, 380, 180], "text", "the Lost Connection box (DISCONNECTED, Find Game / Servers / Settings)")],
   bg="#222222",
   note="The Lost Connection box (the \"error\" packet, player.disconnect \"DISCONNECTED\"); no open transition: the "
        "500 ms frame equals the first. Reference: capture-v10-seq.js lost-connection.")
sc("game-updating", [0, 500], "applied", dict(MENU, connected=False, connectError="GAME UPDATING"),
   [R("box", [450, 300, 380, 120], "text", "the Game Updating box (the art behind it is not painted offline)")],
   bg="#222222",
   note="The Game Updating screen (v10:221859). Reference: capture-v10-seq.js game-updating.")

# ---- C/D. in game

DEATH_BAR = [0, 560, 1280, 160]
sc("death", [0, 250, 500, 750, 1000, 1500, 2000], "death", dict(GAME, console=["kill"]),
   [R("death-bar", DEATH_BAR, "scene", "the bottom card (backdrop-filter blur over the checker) moving up", max_pct=8.0),
    R("you-died", [580, 560, 320, 160], "text", "\"You Died\" + the stat lines", max_pct=10.0,
      pending="S4: \"Time Survived : 2s\" (v10) shows Unknown natively")],
   [T("moveup", DEATH_BAR)],
   note="Self-kill: the \"You Died\" card over the checker (backdrop-filter blur), death-ui-moveup / chat-moveup 1.5 s.")
sc("kill", [0, 100, 200, 300, 500, 700, 1000, 1500, 2000, 3500, 4000, 4200], "kill", dict(GAME, console=BOT_KILL),
   [R("killfeed", [1060, 40, 220, 60], "text", "#killFeed rows (gone at 4 s)",
      pending="T: the sandbox servers name the players differently (native Guest 1 / a random bot, v10 Local User / Bot); "
              "needs a player-name override in the scenario"),
    R("popup", [560, 220, 160, 70], "text", "the +75 score popup", max_pct=6.0)],
   [T("popup", [560, 220, 160, 70])],
   note="The local player kills a bot (`bot 1`, then `kill @bots`): kill feed row (gone after 4 s), score popup, medal.")
sc("hit", [0, 50, 100, 150, 200, 300, 500, 700, 1000], "applied", dict(GAME, hit={"kill": 0, "elapsed_ms": 0}),
   [R("hit-marker", [600, 320, 80, 80], "image", "the hit marker over the aim dot", max_pct=4.0)],
   [T("marker", [600, 320, 80, 80])],
   note="The hit marker from the origin (state.hit; v10: DO_DAMAGE of 40 on a bot). Damage numbers: not injectable "
        "natively yet.")
sc("emoji-panel", [0, 100, 200, 300, 500], "applied", dict(GAME, chatOpen=True, chatInput=":"),
   [R("panel", [0, 400, 400, 320], "text", "the emoji panel over the chat")],
   [T("slide", [0, 400, 400, 320])],
   pending="S5: the in-game emoji panel (svelte-13g6jqo, slideInUp) is MISSING",
   unreachable="the sandbox capture hung after the first frame (CDP timeout at 900 s; the reference holds only 0 ms, not used): "
               "the chat Enter then \":\" keyup path does not lay out #emojiPanelHolder > * within the budget",
   note="Chat open with \":\" typed: the emoji panel. Reference: capture-v10-seq.js emoji-panel.")
sc("game-announcement", [0, 100, 200, 300, 500, 1000], "applied", dict(GAME),
   [R("banner", [340, 0, 600, 120], "text", "the announcement banner")],
   unreachable="the GAME_ANNOUNCEMENT handler (_70210, v10:222148) is registered on the matchmaker socket when it "
               "connects; the sandbox socket has no event table (sock.events is null) and the $gameAnnouncement "
               "store (_33843) is module-private",
   note="A GAME_ANNOUNCEMENT {message}: svelte-1m80xxc (S5 builds it, add a state key then).")

# ---- E. end of match

sc("end-vic", rng(0, 3000, 100), "end", dict(GAME, console=["end"]),
   [R("vic-title", [300, 270, 680, 140], "scene", "the victory title on the overlay canvas", max_pct=6.0)],
   [T("title", [300, 270, 680, 140])],
   note="The match end (console `end`: 3 s timer; origin at the end packet): victory title + #endUI every 100 ms for 3 s.")
sc("end-results", [0, 2000, 4000, 5000, 5500, 6000, 7000], "end", dict(GAME, console=["end"]),
   [R("results", [353, 100, 579, 330], "text", "#endTabbedView and the title above it", max_pct=8.0)],
   [T("results", [353, 100, 579, 330])],
   note="The end animation, then the results (tabs, Combat Earnings).")
sc("end-collect-button", [0, 500, 1000, 1500, 2000, 2500, 3000], "end", dict(GAME, console=["end"]),
   [R("button", [440, 420, 400, 80], "text", "the Collect Rewards button")],
   [T("pulse", [440, 420, 400, 80])],
   pending="S4: the Collect Rewards button (svelte-3svau8) for otherData.items is MISSING; add a state key for the "
           "items and switch anim_origin to selector:.collect-rewards-button",
   note="Results with two reward skins: the Collect Rewards button, 3 s pulse. Reference: capture-v10-seq.js "
        "end-collect-button.")
sc("end-rewards-overlay", [0, 250, 500, 750, 1000, 1500, 2000, 2500, 3000, 4000], "end", dict(GAME, console=["end"]),
   [R("cards", [240, 100, 800, 520], "scene", "the reward cards flipping, then flying to the backpack")],
   [T("flip", [240, 100, 800, 520])],
   pending="S4: the rewards overlay (svelte-1sac6ik: card flip, fly to the backpack) is MISSING",
   note="Collect clicked: the rewards overlay sequence (v10:195771+). Reference: capture-v10-seq.js "
        "end-rewards-overlay.")

# ---- F/G/H. notifications, tooltips, layouts

sc("notification-register", [0, 100, 200, 300, 500, 1000, 2000], "selector:.register-notification", dict(MENU),
   [R("toast", [900, 40, 380, 140], "text", "the guest Register notification")],
   [T("slide", [900, 40, 380, 140])], bg="#222222",
   unreachable="the sandbox did not lay out .register-notification within 70 s of virtual time after the boot "
               "(capture-v10-seq.js notification-register: origin not reached); the v10 notification center is MISSING "
               "natively (S5), so there is no reference to diff against",
   note="The guest Register notification sliding in (slideIn 0.3 s, 15 s countdown bar). Reference: "
        "capture-v10-seq.js notification-register.")
sc("tooltip-loadout", [0, 100, 300], "applied", dict(MENU, window=3),
   [R("tip", [400, 200, 300, 200], "text", "createTip")], bg="#222222",
   unreachable="a guest owns no skins: Change Loadout lists no item with createTip (0 on every tab), and the store "
               "window (showWindow(14)) throws without the signed-in store data",
   note="Customize (Loadout), the pointer on the first item: the item tooltip (S5; also needs a pointer state key).")
sc("mobile-menu", [0], "applied", dict(MENU),
   [R("menu", [0, 0, 1280, 720], "scene", "the mobile menu layout", max_pct=10.0)], bg="#222222",
   pending="S8: no state key turns the mobile UI on (toggleMobileUI(true))",
   note="The mobile menu layout (toggleMobileUI(true)). Reference: capture-v10-seq.js mobile-menu.")
sc("menu-1000", [0], "cycle", dict(MENU),
   [R("menu", [0, 0, 1000, 720], "scene", "the menu at 1000 px wide (the @media rules)", max_pct=10.0)],
   bg="#222222", size=(1000, 720),
   pending="E3: the v10 narrow menu (Skin Manager in the nav, no Quick Match panel, the model shown) is not a CSS @media "
           "result: none of the page's max-width rules matches a menu element (test_css_media covers the engine's @media at 1000 px); "
           "its cause is not found yet",
   note="The menu at 1000 px wide (the @media rules). Reference: capture-v10-seq.js menu-1000.")


def main():
    for name in sorted(scen):
        with open(os.path.join(HERE, "scenarios", name + ".json"), "w") as fh:
            json.dump(scen[name], fh, indent=2)
            fh.write("\n")
        data = {"scenario": name, "defaults": {"fuzz": 6, "max_pct": 0.5}, "regions": regions[name]["regions"]}
        if regions[name]["timing"]:
            data["timing"] = regions[name]["timing"]
        text = json.dumps(data, indent=2)
        text = re.sub(r'"rect": \[\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\]', r'"rect": [\1, \2, \3, \4]', text)
        text = re.sub(r'"size": \[\s*(\d+),\s*(\d+)\s*\]', r'"size": [\1, \2]', text)
        with open(os.path.join(HERE, "regions", name + ".json"), "w") as fh:
            fh.write(text + "\n")
        print("%-28s %2d regions %d timing" % (name, len(regions[name]["regions"]), len(regions[name]["timing"])))


if __name__ == "__main__":
    main()

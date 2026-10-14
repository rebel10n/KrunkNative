#!/usr/bin/env python3
"""Generates the v10 in-game uidiff scenarios: tools/uidiff/scenarios/v10/ingame-*.json and
tools/uidiff/regions/v10/ingame-*.json.

    python3 -I tools/uidiff/gen_v10_ingame.py

References: doc/ref/krunker-v10/ingame/, captured from the OFFLINE v10 sandbox (the pinned v10.0.2
client on the mirrored page, nothing leaving the machine: tools/uidiff/capture-v10.js). The states
are the v7.1.4 scenarios' (gen_scenarios.py, imported: same names, same sandbox moves), with v10's
values where v10 differs (the Assault Rifle's 30 rounds); the region rects come from the v10
captures' own layout (<name>.rects.json). Render on the v10 page: KRUNKNATIVE_UI_DIR=ui/v10.
The JSON files are committed; edit this script and re-run instead of editing them by hand.
"""
import copy
import json
import os
import re
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gen_scenarios as g  # noqa: E402  (builds the v7.1.4 scenario table on import, writes nothing)

REF = "doc/ref/krunker-v10"
R, pad = g.R, g.pad
scen, regions = {}, {}


def rects(name):
    with open(os.path.join(g.REPO, REF, "ingame", name + ".rects.json")) as fh:
        return json.load(fh)


def state_of(v7_name, **kw):
    st = copy.deepcopy(g.scen[v7_name]["state"])
    hud = st.get("hud")
    if isinstance(hud, dict) and hud.get("ammoMax") == 28:  # v10's Assault Rifle: 30 rounds (v7: 28)
        hud["ammoMax"] = 30
        hud["ammo"] = 30 if hud.get("ammo") == 28 else hud.get("ammo")
    if isinstance(hud, dict) and "scoreboard" not in st:
        # the sandbox's local player as the board shows it (v10 always shows #leaderboardHolder)
        st["scoreboard"] = [{"rank": 1, "name": "Local User", "score": 0}]
    st.update(kw)
    return st


def add(name, state, regs, note, frames=3):
    scen[name] = {"ref": "%s/ingame/%s.png" % (REF, name), "size": [1280, 720], "background": g.GAME_BG,
                  "state": state, "frames": frames, "note": note + " Reference: the offline v10 sandbox (capture-v10.js)."}
    regions[name] = regs


def hud_regions(rc, extra=()):
    regs = g.hud_regions(rc, extra)
    if "leaderboardHolder" in rc:
        regs.append(R("leaderboard", pad(rc["leaderboardHolder"], 2), "text",
                      "#leaderboardHolder: v10 always shows it (switchLeaderboard sets display = true, which CSS ignores)"))
    for r in regs:
        if r["name"] == "ammo":
            r["note"] = "#ammoHolder: 30 | 30 + ammo icon"
    return regs


def death_regions(rc, self_kill=False):
    """v10's death screen is v7's markup (v10:220372) at v7's places: the gen_scenarios.py rects."""
    regs = [R("chat", pad(rc["chatHolder"], 2), "text", "#chatHolder (moves right while dead)")] if "chatHolder" in rc else []
    if self_kill:
        regs += [R("death-self", [595, 580, 90, 110], "text", "`You` / `Died` (death-row-center-self)"),
                 R("death-time", [705, 570, 150, 25], "text", "Time Survived: differs per run", skip=True),
                 R("death-stats", [705, 595, 150, 90], "text", "Shots Fired / Accuracy / Kills / Score rows"),
                 R("empty-top-left", [0, 0, 300, 100], "flat", "HUD hidden", expect="background")]
    else:
        regs += [dict(r, note=r["note"].replace("Desert Eagle", "Semi Auto")) for r in g.DEATH_REGS]
        for r in regs:
            if r["name"] == "death-cards":
                r["note"] += " (KrunkNative's offline sandbox names us Guest_1, v10's Local User)"
                r["skip"] = True
    return regs


def main():
    rc = rects("ingame-plain")
    add("ingame-plain", state_of("ingame-plain"), hud_regions(rc), "Plain HUD (FFA on Kanji), full ammo, over flat black.")
    add("ingame-scoped", state_of("ingame-scoped"),
        [r for r in hud_regions(rects("ingame-scoped")) if r["name"] != "crosshair"], "Aiming down sights (right mouse held).")
    add("ingame-reloading", state_of("ingame-reloading", hud=dict(state_of("ingame-reloading")["hud"], ammo=27)),
        hud_regions(rects("ingame-reloading")), "Three shots then R: reloading.")
    add("ingame-scoreboard", state_of("ingame-scoreboard"), hud_regions(rects("ingame-scoreboard")),
        "Tab held: #centerLeaderDisplay (one row) over the HUD.")
    add("ingame-chat-open", state_of("ingame-chat-open"), hud_regions(rects("ingame-chat-open")),
        "Enter then `gg`: the chat input focused.")
    rc = rects("ingame-chat-lines")
    add("ingame-chat-lines", state_of("ingame-chat-lines"), g.chat_regions(rc, 1280, 720),
        "12 sandbox lines: #chatList at its 250 px max-height, scrolled to the newest.")
    rc = rects("ingame-killfeed")
    add("ingame-killfeed", state_of("ingame-killfeed", scoreboard=[{"rank": 1, "name": "Local User", "score": 75, "me": True}, {"rank": 2, "name": "Bot", "score": 0}]),
        hud_regions(rc), "A kill on a sandbox bot: #killFeed under the two-row leaderboard (the +75 score pop-up and the 1 KS badge come from the kill itself, not the scenario).")
    rc = rects("ingame-death")
    st = state_of("ingame-death")
    st["killcard"] = dict(st["killcard"], weapon="Semi Auto", weaponIcon="icon_8", classIndex=6)  # the v10 sandbox bot's
    add("ingame-death", st, death_regions(rc), "Killed by a sandbox bot: the head-to-head death screen.", frames=g.DEATH_FRAMES)
    rc = rects("ingame-death-self")
    add("ingame-death-self", state_of("ingame-death-self"), death_regions(rc, True), "Killed with no killer: `You Died` + stats.",
        frames=g.DEATH_FRAMES)
    rc = rects("ingame-spectate")
    add("ingame-spectate", state_of("ingame-spectate", hud=dict(state_of("ingame-spectate")["hud"], timerText="03:45")),
        [R("spec-names", pad(rc["specNames"], 2), "text", "#specNames: Spectating (free cam)"),
         R("spec-info", pad([591, 12, 98, 52], 2), "text", "#specTimer 03:45 + #specGameInfo Free For All"),
         R("spec-controls", pad(rc["specControlHolder"], 2), "text", "#specControlHolder rows"),
         R("chat", pad(rc["chatHolder"], 2), "text", "#chatHolder"),
         R("leaderboard", pad(rc["leaderboardHolder"], 2), "flat", "#leaderboardHolder (empty while spectating)")
         if "leaderboardHolder" in rc else R("leaderboard", [1068, 8, 204, 18], "flat", "#leaderboardHolder", skip=True),
         R("empty-bottom-left", [300, 600, 700, 100], "flat", "no player HUD while spectating", expect="background")],
        "Spectating the sandbox (free cam): v10's #spectateUI.")
    rc = rects("ingame-minimap")
    add("ingame-minimap", state_of("ingame-minimap", settings={"showMinimap": True}),
        hud_regions(rc, [R("minimap", pad(rc["miniMapOverlay"], 2), "text",
                           "#miniMapOverlay: round (.roundMap) from the first frame, the timer under it. The offline v10 "
                           "sandbox draws no map in it (Kanji's mmId image is not in the mirror); KrunkNative draws "
                           "Kanji's blocks", skip=True),
                         R("minimap-ring", [10, 6, 130, 22], "image", "the ring's top edge and the N letter")]),
        "The minimap on Kanji (v10's showMinimap is off by default: the capture turned it on) at the pose v10's sandbox spawned at.")
    for name, mode, msg in (("ingame-mode-point", "Hardpoint", "next objective 00:45"), ("ingame-mode-ctf", "Capture the Flag", "")):
        rc = rects(name)
        regs = g.mode_regions(rc)
        for r in regs:
            if r["name"] == "game-message":  # KrunkNative's top margin for #topMidHold (ui/v10/css/hud.css)
                r.update(rect=[400, 8, 480, 30], ref_offset=[0, -8],
                         note=r["note"] + "; KrunkNative starts #topMidHold at the corners' 15px margin (hud.css), 8 px lower")
        add(name, state_of(name), regs,
            "%s on Kanji: the team card and #gameMessage." % mode)
    rc = rects("ingame-endscreen")
    add("ingame-endscreen", state_of("ingame-endscreen"),
        [R("victory-text", [420, 200, 440, 130], "text", "`#1 VICTORY` (vicTitle)", skip=True),
         R("chat", pad(rc["chatHolder"], 2), "text", "#chatHolder"),
         R("empty-top-left", [0, 0, 300, 100], "flat", "HUD hidden on the end screen", expect="background"),
         R("empty-bottom-right", [1100, 520, 180, 200], "flat", "HUD hidden on the end screen", expect="background")],
        "endGame(): the end animation, HUD hidden.")
    rc = rects("ingame-endscreen-late")
    st = state_of("ingame-endscreen")
    st["endscreen"] = dict(st["endscreen"], results=1, nextRound="00:17")
    regs = [R("chat", pad(rc["chatHolder"], 2), "text", "#chatHolder")]
    if "endTabbedView" in rc:
        regs.append(R("end-tabs", pad(rc["endTabbedView"], 2), "text",
                      "#endTabbedView: Leaderboard / Performance / Medals / Rewards, Combat Earnings, Combat Record, Double XP"))
    regs.append(R("end-timer", [520, 92, 240, 34], "text", "#endTimer Next Round in 00:17 (its box is 9 px high; the italic text "
                  "overflows it). The engine's synthetic oblique antialiases the slanted stems differently", max_pct=15.0))
    add("ingame-endscreen-late", st, regs, "The results after the end animation (Performance tab, a guest).")

    for d in ("scenarios", "regions"):
        os.makedirs(os.path.join(HERE, d, "v10"), exist_ok=True)
    for name in sorted(scen):
        with open(os.path.join(HERE, "scenarios", "v10", name + ".json"), "w") as fh:
            json.dump(scen[name], fh, indent=2)
            fh.write("\n")
        regs = [r for r in regions[name] if r["rect"][2] > 1 and r["rect"][3] > 1]
        text = json.dumps({"scenario": "v10/" + name, "defaults": {"fuzz": 6, "max_pct": 0.5}, "regions": regs}, indent=2)
        text = re.sub(r'"rect": \[\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\]', r'"rect": [\1, \2, \3, \4]', text)
        with open(os.path.join(HERE, "regions", "v10", name + ".json"), "w") as fh:
            fh.write(text + "\n")
        print("v10/%-22s %2d regions" % (name, len(regs)))


if __name__ == "__main__":
    main()

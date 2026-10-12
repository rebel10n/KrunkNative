#!/usr/bin/env python3
"""Generates tools/uidiff/scenarios/*.json and tools/uidiff/regions/*.json (Krunker v7.1.4).

    python3 -I tools/uidiff/gen_scenarios.py

The JSON files are committed; edit this script and re-run instead of editing them by hand.
References: doc/ref/krunker-v7.1.4/ (tools/uidiff/capture-v7.js). Region rects come from the
reference's own layout (<name>.rects.json, viewport rects of the page's elements written by the
capture tool) wherever an element exists, else they are measured on the PNG. Rects are [x, y, w, h]
in output pixels. See README.md for the region keys (fuzz, max_pct, skip, mask, ...). The
v3.1.1 scenarios are in git history (doc/ref/krunker-v3.1.1/ keeps their references).
"""
import json
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
REF = "doc/ref/krunker-v7.1.4"

KIND = {  # default fuzz (%), max differing pixels (%)
    "text": (12, 3.0),
    "flat": (6, 0.5),
    "image": (8, 1.5),
    "scene": (12, 3.0),
}
ANIM = 0.8  # capture-v7.js pauses every CSS animation at 800 ms


def R(name, rect, kind="text", note=None, skip=False, max_pct=None, fuzz=None, **extra):
    f, m = KIND[kind]
    r = {"name": name, "rect": [int(v) for v in rect], "kind": kind,
         "fuzz": fuzz if fuzz is not None else f,
         "max_pct": max_pct if max_pct is not None else m}
    if skip:
        r["skip"] = True
    r.update(extra)
    if note:
        r["note"] = note
    return r


def rects(path):
    with open(os.path.join(REPO, REF, path + ".rects.json")) as fh:
        return json.load(fh)


def pad(rect, p=2, w=1280, h=720):
    x, y, rw, rh = rect
    x0, y0 = max(0, x - p), max(0, y - p)
    x1, y1 = min(w, x + rw + p), min(h, y + rh + p)
    return [x0, y0, x1 - x0, y1 - y0]


def scale(regs, k):
    out = []
    for r in regs:
        r = dict(r)
        r["rect"] = [int(round(v * k)) for v in r["rect"]]
        out.append(r)
    return out


scen = {}
regions = {}


def add(name, scenario, regs):
    scen[name] = scenario
    regions[name] = regs


# --------------------------------------------------------------------------- menu
# 1280x720: #uiBase scale min(1280/2210, 720/1170) = 0.579 (scaleUI 0.7). Background #d6c6c6 under
# the page (capture-v7.js hides the 3D canvas and paints the page background that colour).
MENU_BG = "#d6c6c6"


def chrome(rc, w=1280, h=720, dimmed=False):
    """Regions of the menu chrome from a capture's rects (rc)."""
    k = "dimmed under the window backdrop; " if dimmed else ""
    regs = []

    def opt(name, key, kind, note, **kw):
        if key in rc:
            regs.append(R(name, pad(rc[key], 2, w, h), kind, k + note, **kw))

    opt("login-button", "signedOutHeaderBar", "text", "#signedOutHeaderBar: Login or Register")
    opt("menu-items", "menuItemContainer", "text", "#menuItemContainer: 7 items (no Exit: browser page)")
    opt("logo", "mainLogo", "image", "#mainLogo (logo_6) + #mainLogoFace f_0")
    opt("season", "seasonLabel", "text", "#seasonLabel Season 7")
    opt("header-right", "headerRight", "image", "#headerRight: region, ping, social icons, client/editor images")
    opt("top-left-info", "tlInfHold", "image", "#tlInfHold: home store ad box, New to Krunker?, Season 7 battle pass")
    opt("now-playing", "mapInfoHld", "text", "#mapInfoHld: Now Playing: ffa on Kanji")
    if "subLogoButtons" in rc and "mapInfoHld" in rc:
        x, y, bw, bh = rc["subLogoButtons"]
        my = rc["mapInfoHld"][1] + rc["mapInfoHld"][3]
        regs.append(R("bottom-buttons", pad([x, my, bw, y + bh - my], 2, w, h), "text",
                      k + "Host Game / Server Finder / Invite / Join / police buttons"))
    opt("streams", "streamContainer", "text", "#streamContainer: Loading Streams...")
    opt("terms", "termsInfo", "text", "#termsInfo: Contact / Experiments / Terms / Changelog")
    opt("chat", "chatHolder", "text", "#chatHolder: chat input bar (menu)")
    opt("customize-button", "customizeButton", "text", "#customizeButton")
    opt("class-icon", "menuClassIcn", "image", "#menuClassIcn: primary weapon preview")
    if "menuClassName" in rc and "menuClassSubtext" in rc:
        a, b = rc["menuClassSubtext"], rc["menuClassName"]
        x0, y0 = min(a[0], b[0]), min(a[1], b[1])
        x1, y1 = max(a[0] + a[2], b[0] + b[2]), max(a[1] + a[3], b[1] + b[3])
        regs.append(R("class-text", pad([x0, y0, x1 - x0, y1 - y0], 2, w, h), "text",
                      k + "#menuClassSubtext / #menuClassName over the 3D preview's legs", max_pct=6.0))
    regs.append(R("class-preview", [int(w * 0.70), int(h * 0.50), int(w * 0.24), int(h * 0.27)], "scene",
                  "3D class preview (v7's three.js preview vs the engine's)", skip=True))
    return regs


rc = rects("menu/menu-1280")
regs = chrome(rc)
regs.append(R("click-to-play", pad(rc["instructions"], 3), "text", "#instructions CLICK TO PLAY (animations at 0.8 s)"))
regs.append(R("spectate", pad(rc["spectButton"], 2), "text", "#spectButton label + switch"))
add("menu-1280", {"ref": REF + "/menu/menu-1280.png", "size": [1280, 720], "background": MENU_BG,
                  "state": {"menu": True, "window": None, "connected": True, "ready": True, "inGame": False,
                            "name": "Guest", "classIndex": 0, "region": "...", "mapName": "Kanji",
                            "gameName": "ffa on Kanji", "anim_time": ANIM},
                  "frames": 3, "note": "Returning guest's menu, no window (v7.1.4 sandbox chrome undone: CONNECTED_MENU)."},
    regs)
rc = rects("menu/menu-1920")
regs = chrome(rc, 1920, 1080)
regs.append(R("click-to-play", pad(rc["instructions"], 3, 1920, 1080), "text", "#instructions CLICK TO PLAY"))
add("menu-1920", {"ref": REF + "/menu/menu-1920.png", "size": [1920, 1080], "background": MENU_BG,
                  "state": dict(scen["menu-1280"]["state"]), "frames": 3, "note": "Same as menu-1280 at 1920x1080."},
    regs)

# --------------------------------------------------------------------------- windows
# name, window, extra state, note
WINDOWS = [
    ("win-settings-general", 0, {"settingsTab": 0, "settingsView": "advanced"}, "Game Settings, advanced view, General"),
    ("win-settings-controls", 0, {"settingsTab": 1, "settingsView": "advanced"}, "Game Settings, advanced view, Controls"),
    ("win-settings-display", 0, {"settingsTab": 2, "settingsView": "advanced"}, "Game Settings, advanced view, Display"),
    ("win-settings-render", 0, {"settingsTab": 3, "settingsView": "advanced"}, "Game Settings, advanced view, Render"),
    ("win-settings-game", 0, {"settingsTab": 4, "settingsView": "advanced"}, "Game Settings, advanced view, Game"),
    ("win-settings-sound", 0, {"settingsTab": 5, "settingsView": "advanced"}, "Game Settings, advanced view, Sound"),
    ("win-settings-basic", 0, {"settingsTab": 0, "settingsView": "basic"}, "Game Settings, basic view (v7 default), General"),
    ("win-servers", 2, {}, "Servers (windows[1]); the reference lists 3 mocked matchmaker games, the scenario none"),
    ("win-loadout", 3, {"windowActions": [["windows.2.switchTab", 1]]}, "Change Loadout, Loadout tab"),
    ("win-classes", 3, {"windowActions": [["windows.2.switchTab", 0]]}, "Change Loadout, Class tab (v3's Select Class)"),
    ("win-mods", 4, {}, "Mod Manager"),
    ("win-profile-login", 5, {}, "Account: Register or Login"),
    ("win-keybinds", 7, {"windowActions": [["windows.6.switchTab", 0]]}, "Change Controls, General tab"),
    ("win-host", 8, {}, "Host Custom Game (hosting is server_bin's in KrunkNative)"),
    ("win-spray", 9, {}, "Select Spray"),
    ("win-games", 10, {}, "Custom Games (home tab, loading)"),
    ("win-shop", 14, {}, "Store (home tab)"),
    ("win-join", 24, {}, "Join"),
    ("win-download", 25, {}, "Client download"),
    ("win-ranked", 27, {}, "Ranked (loading bar)"),
    ("win-attach", 31, {}, "Select Attachment"),
    ("win-reticle", 32, {}, "Select Reticle"),
    ("win-challenges", 33, {}, "Challenges: LOGIN REQUIRED"),
    ("win-changelog", 35, {}, "Changelog: Whats New in v7.1.4"),
    ("win-host-type", 42, {}, "Host Game: server type picker"),
    ("win-server-finder", 48, {}, "Server Finder: server type picker"),
]
# windows whose content depends on data KrunkNative does not have in a scenario: body skipped
BODY_SKIP = {
    "win-servers": "server rows come from the matchmaker (mocked in the reference, none in the scenario)",
    "win-games": "the game list loads from the API (Loading... in the reference)",
}
for name, win, extra, note in WINDOWS:
    rc = rects("windows/" + name)
    regs = [r for r in chrome(rc, dimmed=True) if r["name"] in ("login-button", "menu-items", "logo")]
    x, y, w, h = rc["menuWindow"]
    head = min(h, 82)
    regs.append(R("window-header", pad([x, y, w, head], 2), "text", "#menuWindow top (tabs / search / header)"))
    if h > head + 4:
        regs.append(R("window-body", pad([x, y + head, w, h - head], 2), "text",
                      BODY_SKIP.get(name, "#menuWindow body"), skip=name in BODY_SKIP))
    st = {"window": win}
    st.update(extra)
    add(name, {"ref": "%s/windows/%s.png" % (REF, name), "size": [1280, 720], "background": MENU_BG,
               "state": dict(scen["menu-1280"]["state"], **st), "frames": 3, "note": note}, regs)

# ------------------------------------------------------------------------ in-game
# In-game references are v7's sandbox (Free for All on Kanji, local player "Local User") with the
# 3D canvas hidden over flat black, so the render compares over the same flat black. The
# crosshair is drawn on v7's overlay canvas (the engine draws it with divs, ui/css/hud.css).
GAME_BG = "#000000"


def hud(**kw):
    base = {"health": 100, "maxHealth": 100, "healthSegments": 6, "ammo": 28, "ammoMax": 28, "ammoLimit": False,
            "melee": False, "reloading": False, "reloadPercent": 0, "timerText": "03:45", "kills": 0, "deaths": 0,
            "score": 0, "weaponName": "Assault Rifle", "weaponIcon": "icon_2", "classIndex": 0, "scoped": False,
            "fps": 0, "aim": 0, "spread": 100, "scoreboard": False, "ping": 0,
            "loadout": [{"name": "Assault Rifle", "icon": "icon_2", "melee": False},
                        {"name": "Pistol", "icon": "icon_3", "melee": False},
                        {"name": "Combat Knife", "icon": "icon_0", "melee": True}],
            "loadoutIndex": 0}
    base.update(kw)
    return base


BOT_CHAT = [{"text": "Spawned a Bot", "color": "#fc03ec"}]


def game_state(hudv, chat=(), **kw):
    st = {"menu": False, "window": None, "connected": True, "ready": True, "inGame": True, "mapName": "Kanji",
          "modeName": "Free for All", "name": "Local User", "region": "...", "anim_time": ANIM, "hud": hudv,
          "chat": list(chat)}
    st.update(kw)
    return st


def hud_regions(rc, extra=()):
    regs = []

    def opt(name, key, kind, note, **kw):
        if key in rc:
            regs.append(R(name, pad(rc[key], 2), kind, note, **kw))

    opt("timer", "timerHolder", "text", "#timerHolder: timer icon + 03:45")
    opt("match-info", "matchInfo", "text", "#matchInfo: Free for All / on Kanji")
    opt("ping", "pingDisplay", "text", "#pingDisplay 0 PING")
    opt("chat-input", "chatInputHolder", "text", "#chatInputHolder: channel icon, Enter Message, mic [V]")
    opt("chat-rows", "chatList", "text", "#chatList rows")
    opt("player-card", "bottomLeftPlayer", "image", "#bottomLeftPlayer: class image, FFA, health bar, 100")
    opt("weapons", "weapHolder", "image", "#weapHolder: Assault Rifle / [E] Pistol / [Q] Combat Knife")
    opt("ammo", "ammoHolder", "text", "#ammoHolder: 28 | 28 + ammo icon")
    opt("killfeed", "killFeed", "text", "#killFeed row: You [AR] Bot")
    opt("scoreboard", "centerLeaderDisplay", "text", "#centerLeaderDisplay: Name / Score / Kills / Deaths + Local User")
    regs.append(R("crosshair", [590, 310, 100, 100], "image",
                  "crosshair: v7 draws it on #game-overlay, the engine with hud.css divs", max_pct=4.0))
    regs.extend(extra)
    return regs


def game(name, state, regs, note, size=(1280, 720)):
    add(name, {"ref": "%s/ingame/%s.png" % (REF, name), "size": list(size), "background": GAME_BG, "state": state,
               "frames": 3, "note": note}, regs)


game("ingame-plain", game_state(hud()), hud_regions(rects("ingame/ingame-plain")),
     "Plain HUD in the v7 sandbox (FFA on Kanji), full ammo, over flat black.")
game("ingame-scoped", game_state(hud(aim=1)),
     [r for r in hud_regions(rects("ingame/ingame-scoped")) if r["name"] != "crosshair"] +
     [R("aim-dot", [610, 330, 60, 60], "image", "aiming the Assault Rifle: the overlay's red aim dot, no crosshair")],
     "Assault Rifle aimed (right mouse held): crosshair hidden, red aim dot.")
game("ingame-reloading", game_state(hud(ammo=27, reloading=True)), hud_regions(rects("ingame/ingame-reloading")),
     "3 shots fired, R pressed: 27 | 28 while reloading.")
game("ingame-scoreboard", game_state(hud(scoreboard=True), scoreboard=[{"rank": 1, "name": "Local User", "score": 0}]),
     hud_regions(rects("ingame/ingame-scoreboard")), "Tab held: v7's #centerLeaderDisplay table, one row.")
game("ingame-chat-open", game_state(hud(), chatOpen=True, chatInput="gg"), hud_regions(rects("ingame/ingame-chat-open")),
     "Enter pressed, `gg` typed into #chatInput.")
game("ingame-killfeed",
     game_state(hud(score=75, kills=1), chat=BOT_CHAT,
                killfeed=[{"killer": "You", "killerIsYou": True, "victim": "Bot", "weapon": "Assault Rifle",
                           "weaponIcon": "icon_2", "headshot": False, "age_ms": 500}]),
     hud_regions(rects("ingame/ingame-killfeed"),
                 [R("score-popup", [580, 100, 120, 60], "scene", "+75 score popup on the overlay canvas", skip=True),
                  R("weapon-streak", [1170, 540, 30, 20], "text", "weapon kill streak counter (1KS) next to the rack",
                    skip=True)]),
     "The local player killed a sandbox bot (spawned with pressButton(107)): kill feed + Spawned a Bot.")
DEATH_REGS = [
    R("death-weapons", [380, 570, 470, 75], "text",
      "#deathUIHolder top row: [Desert Eagle] icon, kills 1 - 0, [Assault Rifle] icon"),
    R("death-cards", [380, 650, 460, 50], "image", "class images + lvl, Bot VS Local User"),
    R("death-time", [950, 570, 160, 25], "text", "Time Survived: differs per run", skip=True),
    R("death-stats", [950, 595, 160, 90], "text", "Shots Fired / Accuracy / Kills / Score rows"),
    R("empty-top-left", [0, 0, 300, 100], "flat", "HUD hidden on the death screen", expect="background"),
    R("empty-bottom-right", [1100, 520, 180, 200], "flat", "weapon rack / ammo hidden", expect="background"),
]
rc = rects("ingame/ingame-death")
game("ingame-death",
     game_state(hud(health=0, deaths=1), chat=BOT_CHAT, dead=True,
                killcard={"killer": "Bot", "weapon": "Desert Eagle", "weaponIcon": "icon_11", "level": 1, "myLevel": 1,
                          "score": 0, "classIndex": 10, "stats": [4000, 0, "0%", 0, 0]}),
     [R("chat", pad(rc["chatHolder"], 2), "text", "#chatHolder (moves right while dead)")] + DEATH_REGS,
     "Killed by a sandbox bot (server-side players.kill): v7's head-to-head death screen.")
rc = rects("ingame/ingame-death-self")
game("ingame-death-self",
     game_state(hud(health=0, deaths=1), dead=True, killcard={"stats": [2000, 0, "0%", 0, 0]}),
     [R("chat", pad(rc["chatHolder"], 2), "text", "#chatHolder"),
      R("death-self", [595, 580, 90, 110], "text", "`You` / `Died` (death-row-center-self)"),
      R("death-time", [705, 570, 150, 25], "text", "Time Survived: differs per run", skip=True),
      R("death-stats", [705, 595, 150, 90], "text", "Shots Fired / Accuracy / Kills / Score rows"),
      R("empty-top-left", [0, 0, 300, 100], "flat", "HUD hidden", expect="background")],
     "Killed with no killer: `You Died` + stats.")
rc = rects("ingame/ingame-endscreen")
game("ingame-endscreen", game_state(hud(), endscreen={"place": 1, "result": 1, "text": "#1 VICTORY", "nextRound": "00:00"}),
     [R("victory-text", [420, 300, 440, 100], "text", "`#1 VICTORY` is drawn on v7's overlay canvas (vicTitle)", skip=True),
      R("chat", pad(rc["chatHolder"], 2), "text", "#chatHolder"),
      R("empty-top-left", [0, 0, 300, 100], "flat", "HUD hidden on the end screen", expect="background"),
      R("empty-bottom-right", [1100, 520, 180, 200], "flat", "HUD hidden on the end screen", expect="background")],
     "sandboxGame.endGame(): end animation (#1 VICTORY), HUD hidden.")
rc = rects("ingame/ingame-pause-esc")
pause = [r for r in chrome(rc) if r["name"] in ("login-button", "menu-items", "logo", "header-right", "now-playing",
                                                "bottom-buttons", "customize-button", "terms", "chat")]
pause.append(R("click-to-play", pad(rc["instructions"], 3), "text", "#instructions CLICK TO PLAY"))
pause.append(R("crosshair", [590, 310, 100, 100], "scene", "v7 keeps drawing the crosshair on its overlay canvas "
               "behind the menu", skip=True))
add("ingame-pause-esc", {"ref": REF + "/ingame/ingame-pause-esc.png", "size": [1280, 720], "background": GAME_BG,
                         "state": dict(scen["menu-1280"]["state"], inGame=True, name="Local User"), "frames": 3,
                         "note": "Pointer lock released in game: the menu over the (hidden) scene."}, pause)

# --------------------------------------------------------------------------- hub
# social.html with a socket that opens and never answers: the guest chrome with every view
# loading. Only the navigation bar, the store badges and the popups are compared.
HUB = [("hub-maps", "p=maps", []), ("hub-feed", "p=feed", []), ("hub-leaders", "p=leaders", []),
       ("hub-market", "p=market", []), ("hub-mods", "p=mods", []),
       ("hub-login", "p=maps", ["loginPopup"]), ("hub-settings", "p=maps", ["openSettings"])]
for name, query, actions in HUB:
    rc = rects("hub/" + name)
    regs = [R("profile", pad(rc["profileInfo"], 2), "text", "#profileInfo: Login + Search User"),
            R("nav-buttons", pad(rc["navButtons"], 2), "image", "#navButtons icons"),
            R("nav-title", pad(rc["navTitle"], 2), "text", "#navTitle: Terms | settings, KRUNKER .io"),
            R("badges", [830, 640, 450, 72], "image", "Steam / App Store / Google Play badges"),
            R("editor-badge", pad(rc["editrDB"], 2), "image", "#editrDB")]
    if "genericPop" in rc:
        regs.append(R("popup", pad(rc["genericPop"], 2), "text", "#genericPop"))
    else:
        regs.append(R("loading", pad(rc["loadMessage"], 0), "text", "#loadMessage LOADING (no social server)",
                      max_pct=5.0))
    st = {"page": "hub", "pageQuery": query, "anim_time": ANIM}
    if actions:
        st["pageActions"] = actions
    add(name, {"ref": "%s/hub/%s.png" % (REF, name), "size": [1280, 720], "background": "#000000", "state": st,
               "frames": 3, "note": "Social hub (ui/social.html), guest, %s%s." % (query, " + " + ", ".join(actions) if actions else "")},
        regs)


def main():
    for d in ("scenarios", "regions"):
        os.makedirs(os.path.join(HERE, d), exist_ok=True)
        for f in os.listdir(os.path.join(HERE, d)):
            if f.endswith(".json") and f[:-5] not in scen:
                os.remove(os.path.join(HERE, d, f))  # v3.1.1 scenarios with no v7 counterpart
    for name in sorted(scen):
        with open(os.path.join(HERE, "scenarios", name + ".json"), "w") as fh:
            json.dump(scen[name], fh, indent=2)
            fh.write("\n")
        regs = [r for r in regions[name] if r["rect"][2] > 1 and r["rect"][3] > 1]
        with open(os.path.join(HERE, "regions", name + ".json"), "w") as fh:
            json.dump({"scenario": name, "defaults": {"fuzz": 6, "max_pct": 0.5}, "regions": regs}, fh, indent=2)
            fh.write("\n")
        print("%-24s %2d regions (%d skipped)" % (name, len(regs), sum(1 for r in regs if r.get("skip"))))


if __name__ == "__main__":
    main()

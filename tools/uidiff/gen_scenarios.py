#!/usr/bin/env python3
"""Generates tools/uidiff/scenarios/*.json and tools/uidiff/regions/*.json.

    python3 -I tools/uidiff/gen_scenarios.py

The JSON files are committed; edit this script and re-run instead of editing
them by hand. Rects are [x, y, w, h] in output pixels, measured on the
reference captures (doc/ref/krunker-v3.1.1/). See README.md for the region
keys (fuzz, max_pct, skip, mask, mask_colors, ref_offset).
"""
import json
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
REF = "doc/ref/krunker-v3.1.1"

KIND = {  # default fuzz (%), max differing pixels (%)
    "text": (12, 3.0),
    "flat": (6, 0.5),
    "image": (8, 1.5),
    "scene": (12, 3.0),
}


def R(name, rect, kind="text", note=None, skip=False, max_pct=None, fuzz=None, **extra):
    f, m = KIND[kind]
    r = {"name": name, "rect": list(rect), "kind": kind,
         "fuzz": fuzz if fuzz is not None else f,
         "max_pct": max_pct if max_pct is not None else m}
    if skip:
        r["skip"] = True
    r.update(extra)
    if note:
        r["note"] = note
    return r


def colors(name, rect, cols, note, cfuzz=2.5, **kw):
    """Opaque-colour masked region (in-game: the reference is the background)."""
    return R(name, rect, kw.pop("kind", "text"), note, mask="colors", mask_colors=list(cols),
             mask_color_fuzz=cfuzz, **kw)


def scale(regs, k):
    out = []
    for r in regs:
        r = dict(r)
        r["rect"] = [int(round(v * k)) for v in r["rect"]]
        if "ref_offset" in r:
            r["ref_offset"] = [int(round(v * k)) for v in r["ref_offset"]]
        out.append(r)
    return out


scen = {}
regions = {}


def add(name, scenario, regs):
    scen[name] = scenario
    regions[name] = regs


# --------------------------------------------------------------------------- menu
# 1280x720: #uiBase scale 1280/1700 = 0.753. Background #d6c6c6 = the canvas colour
# under #instructionHolder's 50 % dim (the screen shows #6b6363).
MERCH_NOTE = ("#merchImg (scaleBob 0.4s ease-in-out alternate); anim_time picked by sweeping the "
              "renderer over 0..1.6 s and matching this reference's merch crop")


def chrome(with_instructions, merch_note=MERCH_NOTE):
    regs = [
        R("logo", (400, 8, 480, 144), "image", "img/logo_4.png scaled by #uiBase"),
        R("login-button", (0, 0, 206, 50), "text", "#signedOutHeaderBar: Login or Register"),
        R("menu-items", (0, 50, 82, 670), "text", "#menuItemContainer: 6 icons + labels"),
        R("spectate", (88, 48, 92, 20), "text", "Spectate label + switch"),
        R("merch", (88, 68, 90, 80), "image", merch_note),
        R("top-right-bar", (1090, 0, 190, 42), "image", "signal icon + 4 social sprite buttons"),
        R("loading-streams", (1140, 44, 140, 22), "text", "#streamContainer"),
        R("class-name-tag", (1100, 262, 66, 24), "text", "#menuClassNameTag: Guest"),
        R("class-preview", (1040, 288, 190, 96), "scene", "3D class preview canvas", skip=True),
        R("class-text", (1060, 383, 150, 38), "text",
          "#menuClassSubtext/#menuClassName drawn over the 3D preview character", skip=True),
        R("class-icon-button", (1018, 424, 36, 40), "image", "class icon button left of Customize"),
        R("customize-button", (1056, 424, 154, 40), "text", "Customize button"),
        R("bottom-buttons", (406, 598, 466, 38), "text", "Host Game / Servers / Ranked / Invite / Join"),
        R("empty-left", (90, 160, 300, 420), "flat", "nothing may be painted here (scenario background only)"),
        R("empty-bottom-right", (900, 480, 380, 110), "flat", "nothing may be painted here"),
    ]
    if with_instructions:
        regs.append(R("click-to-play", (536, 338, 208, 36), "text",
                      "#instructions `grow` (from 30px/opacity .3 to 32px/1, 0.8s ease alternate): anim_time "
                      "picked by sweeping the renderer and matching the reference's ink size/opacity", max_pct=5.0))
    return regs


MENU_STATE = {"menu": True, "window": None, "settingsTab": 0, "connected": True, "ready": True,
              "inGame": False, "name": "Guest", "classIndex": 0, "region": "..."}


def menu(name, ref, size, anim_time, regs, note, **state):
    st = dict(MENU_STATE)
    st.update(state)
    st["anim_time"] = anim_time
    add(name, {"ref": ref, "size": list(size), "background": "#d6c6c6", "state": st, "frames": 3,
               "note": note}, regs)


# CLICK TO PLAY: ink 188x24 px at peak R 201 (opacity .79) in real-1280, 284x37 at R 212 (.89)
# in real-1920 -> renderer sweep minimum at 0.325 s / 0.435 s.
menu("menu-1280", REF + "/menu/real-1280.png", (1280, 720), 0.325, chrome(True),
     "Logged-out loaded menu, no window. anim_time 0.325: `grow` phase of the capture "
     "(merch phase follows; its own best match is 0.4).")
menu("menu-1920", REF + "/menu/real-1920.png", (1920, 1080), 0.435, scale(chrome(True), 1.5),
     "Same as menu-1280 at 1920x1080 (#uiBase scale 1.129 = 1.5x the 1280 layout); anim_time 0.435 = `grow` phase.")


def win_regions(y, h, header_h=0, extra=None, body=True):
    regs = chrome(False)
    if header_h:
        regs.append(R("window-header", (406, y, 468, header_h), "text",
                      "sticky header (search, preset select, tabs)"))
        if body:
            regs.append(R("window-body", (406, y + header_h, 468, h - header_h), "text"))
    elif body:
        regs.append(R("window-body", (406, y, 468, h), "text"))
    regs.extend(extra or [])
    return regs


# Settings General: the reference capture holds the two rows the offline reference server's
# page mod adds ("Force Offline Mode", "Enable Developer Console"; mod/mods.ts). KrunkNative
# ports that mod (client/src/ui/ui_mods.c), so the window lines up with no ref_offset.


WINDOWS = [
    # name, window, tab, ref window y, h, header_h, anim_time (merch), note
    ("win-settings-general", 0, 0, 153, 422, 70, 0.275, "Settings / General tab"),
    ("win-settings-controls", 0, 1, 153, 434, 70, 0.000, "Settings / Controls tab"),
    ("win-settings-display", 0, 2, 153, 434, 70, 0.275, "Settings / Display tab"),
    ("win-settings-render", 0, 3, 153, 434, 70, 0.250, "Settings / Render tab"),
    ("win-settings-game", 0, 4, 153, 434, 70, 0.050, "Settings / Game tab"),
    ("win-settings-sound", 0, 5, 215, 296, 70, 0.325, "Settings / Sound tab (short window, vertically centred)"),
    ("win-classes", 6, 0, 153, 435, 0, 0.300, "Select Class: 3x3 class cards, scrollable"),
    ("win-loadout", 3, 0, 154, 417, 0, 0.200, "Change Loadout"),
    ("win-attach", 31, 0, 301, 140, 0, 0.275, "Select Attachment: None + Reflex Sight cards (nested rounded clips)"),
    ("win-reticle", 32, 0, 153, 435, 0, 0.375, "Select Reticle: 14 reticle cards with preview images, scrollable"),
    ("win-servers", 2, 0, 293, 140, 0, 0.025, "Servers (sticky serverHeader, 2 regions, 0 online)"),
    ("win-games", 10, 0, 153, 435, 0, 0.075, "Custom Games: map list with placeholder thumbnails"),
    ("win-host", "host", 0, 153, 435, 0, 0.275, "Host Custom Game (138 KB window, map cards with thumbnails)"),
    ("win-hostX", "host", 0, 153, 435, 0, 0.125,
     "Host Custom Game via the bottom Host Game button (openHostWindow); visually identical to win-host"),
    ("win-join", "join", 0, 316, 93, 0, 0.400, "Join: URL input + button"),
    ("win-keybinds", 7, 0, 153, 435, 0, 0.275, "Change Controls: key labels + delete_forever icons"),
    ("win-mods", 4, 0, 307, 112, 0, 0.075, "Mod Manager: links, tabs, No Mods Found"),
    ("win-profile-login", 5, 0, 286, 153, 0, 0.225, "Account: two inputs with placeholders, two buttons"),
    ("win-ranked", 27, 0, 279, 167, 0, 0.175, "Ranked: LOGIN REQUIRED card with the `clear` Material icon"),
    ("win-challenges", 33, 0, 279, 167, 0, 0.025, "Challenges: LOGIN REQUIRED card"),
    ("win-download", 25, 0, 307, 112, 0, 0.125, "Client: three installer links with images"),
]
# Small cards whose content a broken renderer can drop without tripping window-body's 3 %.
WINDOW_EXTRA = {
    "win-attach": [R("attach-image", (548, 350, 70, 60), "image",
                     "Reflex Sight preview: rounded image inside the rounded card (nested clip); dropped = 21 %, "
                     "ours 1.9 % (1 px card offset)", max_pct=3.0)],
    "win-reticle": [R("scrollbar", (858, 140, 16, 446), "image",
                      "#menuWindow's ::-webkit-scrollbar (track #f5f5f5 + #868686 thumb, 12 px, radius 10); "
                      "hidden = 41 %, ours 6.3 %: Chrome rasterises the 9.04 device px bar from x 866, ours "
                      "anti-aliases from 865 (sub-pixel placement under #uiBase scale 0.753)", max_pct=8.0)],
}
# This reference was captured over CDP without headless Chromium's --hide-scrollbars.
WINDOW_STATE = {"win-reticle": {"scrollbars": True}}
for name, win, tab, y, h, hh, t, note in WINDOWS:
    regs = win_regions(y, h, hh, WINDOW_EXTRA.get(name))
    menu(name, "%s/windows/%s.png" % (REF, name), (1280, 720), t, regs,
         note + "; anim_time %.3f = the reference's merch (scaleBob) phase" % t, window=win, settingsTab=tab,
         **WINDOW_STATE.get(name, {}))

# ------------------------------------------------------------------------ in-game
# In-game scenarios render over flat black (GAME_BG), not over the reference: with the
# reference as background every translucent HUD element is blended twice, and worse, an
# element that is not drawn at all leaves the reference's own pixels in place and passes
# trivially (that is how `#1 VICTORY`, drawn on Krunker's overlay canvas and not by the
# HTML UI, "passed" at 0 %). Over black, regions compare only opaque pixels: mask "colors"
# keys on the element's opaque colours (glyph cores of #fff text, #fc03ec server chat,
# #9eeb56 health segments, the class icon palette, ...) in the reference OR the render,
# with a 2.5 % colour fuzz that excludes translucent text (#maxHP rgba(255,255,255,.8) is
# #cfd1d0 in the reference and #cccccc over black). A missing element shows black on the
# reference's opaque pixels and fails. Regions that must stay empty use
# "expect": "background".
GAME_BG = "#000000"
WHITE = "#ffffff"
CLASS0_PALETTE = ["#3d3d3d", "#72503e", "#282828", "#bc9c7a", "#dfd7d4", "#95381e", "#cfbc8f", "#a9785d", "#b2acaa"]


def hud(**kw):
    base = {"health": 100, "maxHealth": 100, "healthSegments": 6, "ammo": 30, "ammoMax": 30, "ammoLimit": False,
            "melee": False, "reloading": False, "reloadPercent": 0, "timerText": "02:42", "kills": 0, "deaths": 0,
            "score": 0, "weaponName": "Assault Rifle", "weaponIcon": "icon_2", "classIndex": 0, "scoped": False,
            "fps": 0, "aim": 0, "spread": 100, "scoreboard": False, "ping": 2,
            "loadout": [{"name": "Assault Rifle", "icon": "icon_2", "melee": False},
                        {"name": "Pistol", "icon": "icon_3", "melee": False},
                        {"name": "Combat Knife", "icon": "icon_0", "melee": True}],
            "loadoutIndex": 0}
    base.update(kw)
    return base


CHAT2 = [{"text": "You have entered the Soul Sanctum", "color": "#fc03ec"},
         {"text": "Guest_1 joined the game", "color": "#fc03ec"}]


def game_state(hudv, chat=CHAT2, **kw):
    st = {"menu": False, "window": None, "connected": True, "ready": True, "inGame": True,
          "mapName": "Soul Sanctum", "modeName": "Free for All", "name": "Guest_1", "region": "New Jersey",
          "anim_time": 0.0, "hud": hudv, "chat": chat}
    st.update(kw)
    return st


SKIP_CHAT_INPUT = R("chat-input", (10, 603, 180, 26), "text",
                    "#chatInput placeholder is rgba(255,255,255,.7) on rgba(0,0,0,.4): translucent only, "
                    "double-blended over the reference background", skip=True)
SKIP_MIC = R("chat-mic", (195, 603, 45, 24), "text", "#voiceDisplay has opacity .5: translucent only", skip=True)
SKIP_PING = R("ping", (138, 8, 40, 28), "text",
              "#pingText rgba(255,255,255,.5) and the signal icon are translucent", skip=True)


def hud_regions(chat_rows=(14, 538, 180, 62), chat_cols=("#fc03ec",), extra=(), chat_input=SKIP_CHAT_INPUT):
    regs = [
        colors("timer", (18, 16, 104, 34), [WHITE, "#ca56eb"], "#timerHolder: timer icon + #timerVal (box is translucent)"),
        SKIP_PING,
        colors("game-mode", (10, 60, 80, 18), [WHITE],
               "#curGameInfo first line `Free for All` (`on <map>` is rgba .6)"),
        colors("chat-rows", chat_rows, chat_cols, "#chatList rows: coloured message glyphs (row boxes are rgba(0,0,0,.2))"),
        chat_input,
        SKIP_MIC,
        colors("class-icon", (12, 634, 66, 66), CLASS0_PALETTE, "class icon (pixelated, transparent texels excluded)",
               kind="image", fuzz=8, max_pct=1.5),
        colors("health-value", (85, 636, 95, 24), [WHITE, "#eb4343"], "#healthValue `100` + red cross (`| 100` is rgba .8)"),
        colors("health-bar", (82, 664, 190, 38), ["#9eeb56"], "6 .healthBarSeg fills (opaque #9eeb56)",
               kind="flat", fuzz=6, max_pct=1.0),
        colors("ammo", (1125, 650, 140, 40), [WHITE, "#ffd542"], "#ammoVal + `!!!` (#ammoMax is rgba .7)"),
        colors("weapon-active", (1180, 538, 96, 30), [WHITE], "active .weapItem icon (opacity 1)", kind="image",
               fuzz=10, max_pct=2.5),
        colors("weapon-keys", (1250, 568, 26, 60), [WHITE], "[E] / [Q] key labels"),
        R("weapon-holstered", (1170, 568, 80, 66), "image",
          "holstered .weapItem icons have opacity .7: translucent only", skip=True),
        R("crosshair", (584, 298, 112, 120), "scene", "default crosshair bars over the 3D scene", skip=True),
    ]
    regs.extend(extra)
    return regs


def game(name, ref, size, state, regs, note, background=GAME_BG):
    add(name, {"ref": ref, "size": list(size), "background": background, "state": state, "frames": 3,
               "note": note}, regs)


IG = REF + "/ingame/"
game("ingame-plain", IG + "ingame-plain.png", (1280, 720), game_state(hud(timerText="02:42")), hud_regions(),
     "Plain HUD, full ammo, over flat black; regions compare the reference's opaque colours only.")
game("ingame-scoped", IG + "ingame-scoped.png", (1280, 720),
     game_state(hud(timerText="02:41", scoped=False, aim=1)), hud_regions(),
     "Aiming the Assault Rifle down its sight (no scope reticle; aim 1 hides the crosshair).")
game("ingame-reloading", IG + "ingame-reloading.png", (1280, 720),
     game_state(hud(timerText="02:39", ammo=27, reloading=True, reloadPercent=0)),
     hud_regions(extra=[R("reload-msg", (560, 420, 160, 40), "text",
                          "#reloadMsg `[R] Reload` is not visible in the capture (reloading just started)", skip=True)]),
     "Reloading with 27|30; the capture shows no reload message yet.")
game("ingame-scoreboard", IG + "ingame-scoreboard.png", (1280, 720),
     game_state(hud(timerText="02:35", ammo=27, scoreboard=True),
                scoreboard=[{"rank": 1, "name": "Guest_1", "score": 0}]),
     hud_regions(extra=[colors("scoreboard", (481, 345, 316, 24), [WHITE],
                               "#leaderContainerD .newLeaderItem row: rank/name/score glyphs (row box is translucent)")]),
     "Tab held: one .newLeaderItem row (the local player Guest_1) in the centre.")
CHAT4 = CHAT2 + [{"text": "Guest_1: gg", "color": "#ffffff", "name": "Guest_1", "msg": "gg"},
                 {"text": "Guest_2 joined the game", "color": "#fc03ec"}]
game("ingame-killfeed", IG + "ingame-killfeed.png", (1280, 720),
     game_state(hud(timerText="02:16", ammo=24), chat=CHAT4,
                killfeed=[{"killer": "You", "killerIsYou": True, "victim": "Guest_2", "weapon": "Assault Rifle",
                           "weaponIcon": "icon_2", "headshot": True, "age_ms": 500}]),
     hud_regions(chat_rows=(14, 488, 180, 112), chat_cols=("#fc03ec", WHITE), extra=[
         colors("killfeed-row", (1104, 10, 166, 28), [WHITE, "#eb5656"],
                "#killFeed row: `You`, skull, victim `Guest_2` (#eb5656); the weapon icon is translucent"),
         R("score-popup", (590, 240, 100, 40), "scene", "+125 score popup is drawn by the game, not the HUD",
           skip=True)]),
     "One kill-feed row top right, 4 chat rows, 24|30.")
game("ingame-chat-open", IG + "ingame-chat-open.png", (1280, 720),
     game_state(hud(timerText="02:34", ammo=27), chatOpen=True, chatInput="gg"),
     hud_regions(chat_input=colors("chat-input", (10, 603, 180, 26), [WHITE],
                                   "#chatInput focused: typed `gg` (#fff) and the caret (blink phase unknown)",
                                   max_pct=4.0)),
     "Chat input focused with text `gg`.")
CHAT_DEATH = [{"text": "You have entered the Soul Sanctum", "color": "#fc03ec"},
              {"text": "Guest_2 joined the game", "color": "#fc03ec"}]
game("ingame-death", IG + "ingame-death.png", (1280, 720),
     game_state(hud(timerText="02:15", health=0, deaths=1, ping=3), chat=CHAT_DEATH, dead=True, name="Guest_2",
                killcard={"killer": "Guest_1", "weapon": "Assault Rifle", "level": 1, "score": 125, "classIndex": 0}),
     [colors("chat-rows", (14, 618, 180, 62), ["#fc03ec"], "chat rows (HUD bottom-left hidden while dead)"),
      R("chat-input", (10, 683, 180, 26), "text", SKIP_CHAT_INPUT["note"], skip=True),
      R("chat-mic", (195, 683, 45, 24), "text", SKIP_MIC["note"], skip=True),
      colors("killcard-text", (488, 600, 300, 110), [WHITE, "#ffea78"],
             "#killCardHolder: `Killed by`, killer name, `lvl 1`, `+125` (#ffea78); the weapon line is translucent"),
      colors("killcard-icon", (500, 636, 62, 66), CLASS0_PALETTE, "killer class image", kind="image", fuzz=8,
             max_pct=1.5),
      R("empty-top-left", (0, 0, 200, 100), "flat", "timer/ping/game-info hidden while dead", expect="background")],
     "Death card (local player Guest_2): HUD holders hidden, kill card at the bottom centre.")
CHAT_END = CHAT2 + [{"text": "Guest_2 joined the game", "color": "#fc03ec"},
                    {"text": "You're on a 5 Kill Streak", "color": "#ffffff"},
                    {"text": "You're on a 10 Kill Streak", "color": "#ffffff"}]
game("ingame-endscreen", IG + "ingame-endscreen.png", (1280, 720),
     game_state(hud(timerText="00:00", ammo=5, kills=11, score=750, ping=1), chat=CHAT_END,
                endscreen={"place": 1, "text": "#1 VICTORY", "nextRound": "00:23"}),
     [R("victory-text", (420, 300, 440, 100), "text",
        "`#1 VICTORY` is drawn on Krunker's overlay canvas, not by the HTML UI (#victorySub stays empty), so "
        "the native client does not draw it", skip=True),
      colors("chat-rows", (14, 545, 180, 135), ["#fc03ec", WHITE], "5 chat rows (white `You're` spans)"),
      R("chat-input", (10, 683, 180, 26), "text", SKIP_CHAT_INPUT["note"], skip=True),
      R("chat-mic", (195, 683, 45, 24), "text", SKIP_MIC["note"], skip=True),
      R("empty-top-left", (0, 0, 200, 100), "flat", "HUD hidden on the end screen", expect="background"),
      R("empty-bottom-right", (1100, 530, 180, 190), "flat", "ammo/weapon rack hidden on the end screen",
        expect="background")],
     "End screen: #1 VICTORY, HUD hidden, chat still visible.")

# ESC pause: the menu over the live scene. The reference is dimmed 50 % by
# #instructionHolder; backgrounds/ingame-pause-esc-undimmed.png = reference x 2
# (`magick <ref> -evaluate multiply 2`, exact to +-1 for scene pixels, which are all < 128
# there), so the render's own dim reproduces the scene. Pixels under translucent UI are
# still wrong in that background, so text over the scene uses colour/ink masks.
PAUSE_STATE = dict(MENU_STATE)
PAUSE_STATE.update({"inGame": True, "mapName": "Soul Sanctum", "modeName": "Free for All", "region": "New Jersey",
                    "gameName": "ffa_Soul Sanctum", "chat": CHAT4[:3], "name": "Guest", "anim_time": 0.325})
PAUSE_OPAQUE = ("logo", "login-button", "class-icon-button", "customize-button", "bottom-buttons", "class-preview",
                "class-text")
pause_regs = [r for r in chrome(True) if r["name"] in PAUSE_OPAQUE]
pause_regs += [
    R("spectate", (88, 48, 92, 20), "text",
      "Spectate label/switch are translucent: the un-dimmed background is wrong under any reference UI pixel",
      skip=True),
    R("merch", (88, 68, 90, 80), "image", "merch over the scene; scaleBob phase not measured on this capture",
      skip=True),
    colors("menu-items", (0, 50, 82, 670), [WHITE, "#ffc710"],
           "#menuItemContainer labels + icons (column bg is rgba(0,0,0,.75) over the scene)", cfuzz=6),
    colors("class-name-tag", (1100, 262, 66, 24), [WHITE], "#menuClassNameTag `Guest` glyph cores"),
    R("click-to-play", (536, 338, 208, 36), "text",
      "#instructions is rgba(255,255,255,.8) x grow opacity over the scene: translucent only", skip=True),
    colors("region-label", (1040, 4, 92, 22), [WHITE], "#menuRegionLabel: New Jersey"),
    R("social-buttons", (1136, 0, 144, 42), "image", "4 social sprite buttons"),
    R("featured-creators", (1110, 44, 166, 20), "text",
      "#streamContainer `Featured Creators` list comes from the live Krunker streams feed; the offline client "
      "keeps `Loading Streams...`", skip=True),
    R("creator-row", (1130, 66, 146, 34), "text", "streams feed content (see featured-creators)", skip=True),
    colors("chat-rows", (100, 598, 170, 82), ["#fc03ec", WHITE], "in-game chat next to the menu column"),
    R("chat-input", (96, 683, 170, 26), "text", SKIP_CHAT_INPUT["note"], skip=True),
    R("chat-mic", (275, 683, 45, 24), "text", SKIP_MIC["note"], skip=True),
    colors("game-name", (1118, 646, 156, 24), [WHITE], "#gameNameHolder: ffa_Soul Sanctum"),
    colors("bottom-links", (1000, 683, 280, 26), [WHITE],
           "#baseLinks: `Advertise` (#fff); Support/Terms/v3.1.1 are translucent grey"),
]
game("ingame-pause-esc", IG + "ingame-pause-esc.png", (1280, 720), PAUSE_STATE, pause_regs,
     "Escape pressed in game: menu over the live scene (background = the reference un-dimmed x2, see "
     "backgrounds/), chat kept, "
     "game name + base links shown.", background="tools/uidiff/backgrounds/ingame-pause-esc-undimmed.png")

HIT_CHAT = [{"text": "Guest_1 joined the game", "color": "#fc03ec"}, {"text": "Guest_2 joined the game", "color": "#fc03ec"}]
# 1280x577: #uiBase scale = min(1280/1700, 577/900) = 0.641, HUD anchored to the viewport edges.
hit_regs = [
    colors("timer", (8, 8, 102, 44), [WHITE, "#ca56eb"], "#timerHolder icon + digits"),
    R("ping", (114, 6, 36, 26), "text", SKIP_PING["note"], skip=True),
    colors("game-mode", (8, 54, 70, 14), [WHITE], "#curGameInfo first line"),
    colors("chat-rows", (14, 432, 140, 46), ["#fc03ec"], "2 chat rows"),
    R("chat-input", (8, 476, 156, 24), "text", SKIP_CHAT_INPUT["note"], skip=True),
    R("chat-mic", (170, 478, 36, 20), "text", SKIP_MIC["note"], skip=True),
    colors("class-icon", (10, 504, 56, 56), CLASS0_PALETTE, "class icon", kind="image", fuzz=8, max_pct=1.5),
    colors("health-value", (72, 504, 84, 22), [WHITE, "#eb4343"], "#healthValue + red cross"),
    colors("health-bar", (68, 528, 164, 34), ["#9eeb56"], "health segments", kind="flat", fuzz=6, max_pct=1.0),
    colors("ammo", (1150, 514, 124, 40), [WHITE, "#ffd542"], "#ammoVal + !!!"),
    colors("weapon-active", (1192, 418, 86, 28), [WHITE], "active weapon icon", kind="image", fuzz=10, max_pct=2.5),
    R("hitmarker", (596, 240, 92, 100), "text",
      "crosshair + hit marker 363 ms after the hit; the red damage number is drawn by the game", skip=True),
]
game("ingame-hitmarker", IG + "ingame-hitmarker-burst1-1-363ms.png", (1280, 577),
     game_state(hud(timerText="01:36", ammo=21, ping=1), chat=HIT_CHAT, mapName="Shipyard",
                hit={"elapsed_ms": 363, "kill": False}),
     hit_regs, "Hit-marker frame 363 ms after a hit on Shipyard, captured at 1280x577.")


def main():
    for d in ("scenarios", "regions"):
        os.makedirs(os.path.join(HERE, d), exist_ok=True)
    for name in sorted(scen):
        with open(os.path.join(HERE, "scenarios", name + ".json"), "w") as fh:
            json.dump(scen[name], fh, indent=2)
            fh.write("\n")
        with open(os.path.join(HERE, "regions", name + ".json"), "w") as fh:
            json.dump({"scenario": name, "defaults": {"fuzz": 6, "max_pct": 0.5}, "regions": regions[name]}, fh,
                      indent=2)
            fh.write("\n")
        rs = regions[name]
        print("%-24s %2d regions (%d skipped)" % (name, len(rs), sum(1 for r in rs if r.get("skip"))))


if __name__ == "__main__":
    main()

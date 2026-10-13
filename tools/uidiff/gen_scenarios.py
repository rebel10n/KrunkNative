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
import re
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
inline_rects = set()  # region files written with one-line "rect": [x, y, w, h] arrays


def add(name, scenario, regs, one_line_rects=False):
    scen[name] = scenario
    regions[name] = regs
    if one_line_rects:
        inline_rects.add(name)


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
    if "termsInfo" in rc:
        # KrunkNative drops v7's "Experiments" link and its separator (tools/ui-import/index_page.py,
        # an intentional difference), so the right-aligned #termsInfo is narrower and only its right
        # part ("| Terms | Changelog", 150 of its 320 px at 1280x720) lines up with the reference.
        x, y, tw, th = rc["termsInfo"]
        part = int(round(tw * 150 / 320))
        regs.append(R("terms", pad([x + tw - part, y, part, th], 2, w, h), "text",
                      k + "#termsInfo right part: | Terms | Changelog (KN has no Experiments link)"))
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



# the local server's map: the v3.1.1 Kanji these scenarios have always run on (its file is
# assets/maps/311_kanji.json; "maps/kanji.json" is v7's Kanji, which ingame-minimap loads)
V3_KANJI = "maps/311_kanji.json"

rc = rects("menu/menu-1280")
regs = chrome(rc)
regs.append(R("click-to-play", pad(rc["instructions"], 3), "text", "#instructions CLICK TO PLAY (animations at 0.8 s)"))
regs.append(R("spectate", pad(rc["spectButton"], 2), "text", "#spectButton label + switch"))
add("menu-1280", {"ref": REF + "/menu/menu-1280.png", "size": [1280, 720], "background": MENU_BG,
                  "state": {"menu": True, "window": None, "connected": True, "ready": True, "inGame": False,
                            "name": "Guest", "classIndex": 0, "region": "...", "mapName": "Kanji",
                            "mapFile": V3_KANJI,
                            "gameName": "ffa on Kanji", "anim_time": ANIM},
                  "frames": 3, "note": "Returning guest's menu, no window (v7.1.4 sandbox chrome undone: CONNECTED_MENU)."},
    regs)
rc = rects("menu/menu-1920")
regs = chrome(rc, 1920, 1080)
regs.append(R("click-to-play", pad(rc["instructions"], 3, 1920, 1080), "text", "#instructions CLICK TO PLAY"))
add("menu-1920", {"ref": REF + "/menu/menu-1920.png", "size": [1920, 1080], "background": MENU_BG,
                  "state": dict(scen["menu-1280"]["state"]), "frames": 3, "note": "Same as menu-1280 at 1920x1080."},
    regs)

# The Lost Connection screen (capture-v7.js: io_error's UI steps + f2040): #menuHolder and the
# chat hidden, only the #instructionsUpdate error box and #termsInfo over the background. The
# box's tab row is not compared: KrunkNative adds a fifth tab (Play Offline) and sizes the cells
# by their text (menu_ui.c). Its "Region:" line is not either (v7 prints "0" with no region).
# Where the menu chrome was (logo, menu items, the bottom buttons, the class box, the chat) the
# reference is the flat background; the Spectate switch's place lies inside the box.
DISCONNECT_ERROR = "Could not connect to the game server"
for tag, (w, h) in (("1280", (1280, 720)), ("1920", (1920, 1080)), ("1024", (1024, 768))):
    rc = rects("menu/menu-disconnected-" + tag)
    menu_rc = rects("menu/menu-" + tag)
    x, y, bw, bh = rc["instructionsUpdate"]
    regs = [R("error-head", pad([x, y, bw, int(bh * 0.585)], 2, w, h), "text",
              "#instructionsUpdate: Lost Connection header, the error, the ID / Code / Reason line (the 30 / 28 px "
              "lines sit up to 1 px off Chromium's: 3.6% at 1280x720; 29% with the menu chrome over the box)", max_pct=5.0),
            R("error-extensions", [x, y + int(bh * 0.69), bw, int(bh * 0.08)], "text",
              "#instructionsUpdate: If you have any extensions, disable them"),
            R("error-tabs", [x, y + int(bh * 0.81), bw, bh - int(bh * 0.81)], "text",
              "the instructionsTabs row: KrunkNative's five tabs (Play Offline) vs v7's four", skip=True),
            R("no-spectate", pad(menu_rc["spectButton"], 2, w, h), "text",
              "the menu's Spectate switch place: inside the error box, no switch (hideUI: toggleMenuHider(false)); "
              "a small rect on the 28 px error line, whose 1 px offset alone is up to 13%; the switch is 58-65%",
              max_pct=20.0)]
    for name, key in (("no-logo", "mainLogo"), ("no-menu-items", "menuItemContainer"), ("no-bottom-buttons", "subLogoButtons"),
                      ("no-customize", "customizeButton"), ("no-chat", "chatHolder"), ("no-top-left", "tlInfHold")):
        regs.append(R(name, pad(menu_rc[key], 2, w, h), "flat", "#%s's place: #menuHolder / #chatHolder hidden" % key))
    if "termsInfo" in rc:
        tx, ty, tw, th = rc["termsInfo"]
        part = int(round(tw * 150 / 320))
        regs.append(R("terms", pad([tx + tw - part, ty, part, th], 2, w, h), "text",
                      "#termsInfo right part: | Terms | Changelog (KN has no Experiments link)"))
    add("menu-disconnected-" + tag, {"ref": "%s/menu/menu-disconnected-%s.png" % (REF, tag), "size": [w, h],
                                     "background": MENU_BG,
                                     "state": dict(scen["menu-1280"]["state"], connected=False, ready=False,
                                                   connectError=DISCONNECT_ERROR), "frames": 3,
                                     "note": "The Lost Connection screen at %dx%d (connect error): the error box alone, "
                                             "no menu chrome and no Spectate switch; Find a Game / Play Offline on one line." % (w, h)},
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
    # season-system: signed in (season_fixture.json; capture-v7.js hands the same account to v7)
    ("win-battlepass", 6, {"account": "free"}, "Battle Pass, signed in: free track, level 4"),
    ("win-battlepass-vip", 6, {"account": "vip"}, "Battle Pass, signed in: VIP tier"),
    ("win-battlepass-chal", 6, {"account": "chal"}, "Battle Pass, Challenges tab: the daily cards (+ KrunkNative's weekly ones)"),
    ("win-challenges-acc", 33, {"account": "free"}, "Challenges, signed in: map, weapon and nightmare sections"),
]
with open(os.path.join(HERE, "season_fixture.json")) as _fh:
    SEASON_ACCOUNT = json.load(_fh)
SEASON_ACCOUNT.pop("_comment", None)


def season_account(kind):
    acc = json.loads(json.dumps(SEASON_ACCOUNT))
    if kind == "vip":
        acc["battlepass"]["progress"].update(tier=1, items_claimed="20001,21001,21004")
    if kind == "chal":  # v7 has no weekly challenges: the variant lists the daily ones only
        acc["battlepass"]["tab"] = 1
        acc["challenges"] = [c for c in acc["challenges"] if c["T"] != "weekly"]
    return acc
# windows whose content depends on data KrunkNative does not have in a scenario: body skipped
BODY_SKIP = {
    "win-servers": "server rows come from the matchmaker (mocked in the reference, none in the scenario)",
    "win-games": "the game list loads from the API (Loading... in the reference)",
}
# window headers with a photo: the engine samples a downscaled background-size: cover image
# differently from Chromium's filtering (texture-level pixel differences, same placement)
HEADER_PHOTO = {
    "win-changelog": (14.0, ".logVersionHed: img/backgrounds/whatsnew.png (1125x360, cover, ~0.53x): "
                            "~10% of the photo's pixels differ past fuzz 12 from Chromium's resampling"),
}
for name, win, extra, note in WINDOWS:
    rc = rects("windows/" + name)
    regs = [r for r in chrome(rc, dimmed=True) if r["name"] in ("login-button", "menu-items", "logo")]
    x, y, w, h = rc["menuWindow"]
    head = min(h, 82)
    photo = HEADER_PHOTO.get(name)
    regs.append(R("window-header", pad([x, y, w, head], 2), "text",
                  "#menuWindow top (tabs / search / header)" + ("; " + photo[1] if photo else ""),
                  max_pct=photo[0] if photo else None))
    if h > head + 4 and extra.get("settingsTab") == 0:
        # General tab: KrunkNative's fifth preset box (Beautiful, ui_settings.c sync_preset_boxes)
        # narrows the row of .setSugBox2 boxes, an intentional difference; the rest of the body
        # is compared as usual.
        row = 128
        regs.append(R("preset-boxes", pad([x, y + head, w, row], 2), "text",
                      "Default / Pro / Performance / Beautiful / Custom: KN adds the Beautiful box", skip=True))
        regs.append(R("window-body", pad([x, y + head + row, w, h - head - row], 2), "text", "#menuWindow body"))
    elif h > head + 4:
        regs.append(R("window-body", pad([x, y + head, w, h - head], 2), "text",
                      BODY_SKIP.get(name, "#menuWindow body"), skip=name in BODY_SKIP))
    st = {"window": win}
    st.update(extra)
    if "account" in st:
        st["name"] = "Player"
        st["account"] = season_account(st["account"])
    add(name, {"ref": "%s/windows/%s.png" % (REF, name), "size": [1280, 720], "background": MENU_BG,
               "state": dict(scen["menu-1280"]["state"], **st), "frames": 3, "note": note}, regs)

# ------------------------------------------------------------------- prize wheel
# The store's Starter wheel (state.spin: prizeWheel(0) with noAnim, the wheel at its full size;
# the reference calls v7's prizeWheel(0, null, false, false, true)). #spinWindow is a 1240px
# canvas in #uiBase's scale; the wheel drawn on it has radius width / 2 * 0.5 (v7.1.4), so the
# region is the disc's bounding square, not the canvas. The four preview icons are random picks.
# win-spin-item*: signed in (season_fixture.json), the spin's reply (state.spinResult: roll 40,
# skin 15 "Digital AK") and the reveal: the name card and buttons are compared; v7 renders the item
# in 3D (three.js), KrunkNative shows its preview image in the same box (skipped).
def text_column(rect, share=0.42):
    x, y, w, h = rect
    return [x + w * (1 - share) / 2, y, w * share, h]


SPIN_SIZES = (("", (1280, 720)), ("-1920", (1920, 1080)), ("-1024", (1024, 768)), ("-portrait", (720, 1280)))
for tag, (sw, sh) in SPIN_SIZES:
    name = "win-spin" + tag
    rc = rects("windows/" + name)
    x, y, w, h = rc["spinWindow"]
    r = w / 4.0
    cx, cy = x + w / 2.0, y + h / 2.0
    disc = pad([cx - r, cy - r, 2 * r, 2 * r], 4, sw, sh)
    regs = [R("wheel", disc, "image",
              "the disc (#2b2b2b rim, slices at 0.95 R, pointer, centre disc) + #spinButton SPIN -50KR", fuzz=12, max_pct=3.0),
            R("spin-info", pad(rc["spinInfo"], 2, sw, sh), "text", "#spinInfo: STARTER and the rarity odds (v7 lists 0% rows)"),
            R("spin-items", pad(rc["spinItems"], 2, sw, sh), "image", "#spinItems: four random previews + More", skip=True)]
    if "spinHistoryBtn" in rc:
        regs.append(R("spin-history", pad(rc["spinHistoryBtn"], 2, sw, sh), "text",
                      "#spinHistoryBtn: Spin History" + ("" if sw == 1280 else "; the scaled button lands 1px lower than in Chromium (its borders)"),
                      max_pct=3.0 if sw == 1280 else 8.0))
    add(name, {"ref": "%s/windows/%s.png" % (REF, name), "size": [sw, sh], "background": MENU_BG,
               "state": dict(scen["menu-1280"]["state"], window=None, spin=0), "frames": 3,
               "note": "Store prize wheel (Starter), %dx%d" % (sw, sh)}, regs)

    name = "win-spin-item" + tag
    rc = rects("windows/" + name)
    regs = [R("item", disc, "scene", "the won item over the wheel: v7's three.js model vs KrunkNative's preview image", skip=True),
            # the card's text column only: the item (3D in v7, an image here) shows through the card's sides
            R("item-name", pad(text_column(rc["spinItemName"]), 2, sw, sh), "text", "#spinItemName text: Digital AK / Assault Rifle / NEW"
              + ("; at 720 px the subtitle is a 7px font whose glyphs land 1px apart (and random confetti)" if sw < 1000 else ""),
              max_pct=9.0 if sw < 1000 else None),
            R("item-buttons", pad(rc["spinUIBtns"], 2, sw, sh), "text", "#spinUIBtns: respin 50 KR, ?, Equip Item")]
    add(name, {"ref": "%s/windows/%s.png" % (REF, name), "size": [sw, sh], "background": MENU_BG,
               "state": dict(scen["menu-1280"]["state"], window=None, spin=0, spinResult={"rand": 40, "item": 15},
                             name="Player", account=season_account("free")), "frames": 3,
               "note": "Store prize wheel (Starter): the won item's reveal, %dx%d" % (sw, sh)}, regs)

# win-spin-multi: the Starter wheel's 10x spin answered with ten rolls / items (capture-v7.js MULTI),
# revealed: the highest rarity first (Lineae, Rare), the respin button at 10 x 50 KR and Next Item
MULTI = {"rand": [11.3775, 56.5799, 73.6815, 57.9684, 81.7105, 26.9725, 24.0595, 82.6168, 13.3432, 51.2703],
         "item": [6043, 6062, 6083, 6020, 5996, 6087, 6083, 6033, 6082, 6031]}
rc = rects("windows/win-spin-multi")
x, y, w, h = rc["spinWindow"]
r = w / 4.0
regs = [R("item", pad([x + w / 2.0 - r, y + h / 2.0 - r, 2 * r, 2 * r], 4), "scene",
          "the won item over the wheel: v7's three.js model vs KrunkNative's preview image", skip=True),
        R("item-name", pad(text_column(rc["spinItemName"])), "text", "#spinItemName text: Lineae / Sniper Rifle / NEW"),
        R("item-buttons", pad(rc["spinUIBtns"]), "text", "#spinUIBtns: respin 500 KR, ?, Equip Item, Next Item"),
        R("spin-info", pad(rc["spinInfo"]), "text", "#spinInfo")]
add("win-spin-multi", {"ref": "%s/windows/win-spin-multi.png" % REF, "size": [1280, 720], "background": MENU_BG,
                       "state": dict(scen["menu-1280"]["state"], window=None, spin=0, spinMulti=True, spinResult=MULTI,
                                     name="Player", account=season_account("free")), "frames": 3,
                       "note": "Store prize wheel (Starter) 10x spin: the first (highest rarity) item revealed"}, regs)

# Signed in with spin tickets (capture-v7.js SPIN_SKINS): the Store home (Heroic card), the Prize
# Wheels tab (ticket counts, Ultra's "You have 1", View Tickets "You have 3") and the Spin History
# popup (SPIN_HISTORY, dates in the machine's time zone like v7's toDateString)
SPIN_SKINS = [{"ind": 6423, "cnt": 2}, {"ind": 6426, "cnt": 1}, {"ind": 15, "cnt": 1}]
SPIN_HISTORY = [{"ind": 15, "dte": 1791800000000, "fds": 50}, {"ind": 5996, "dte": 1791790000000, "fds": 50},
                {"ind": 6043, "dte": 1791780000000, "fds": 100}, {"ind": 6062, "dte": 1791000000000, "fds": 0},
                {"ind": 6087, "dte": 1790000000000, "fds": 500}]
spin_acc = dict(season_account("free"), skins=SPIN_SKINS)
for name, extra, note in (("win-shop-spin", {"window": 14}, "Store home tab signed in: the Heroic card"),
                          ("win-shop-wheels", {"window": 14, "windowActions": [["windows.13.switchTab", 1]]},
                           "Store Prize Wheels tab signed in, with spin tickets")):
    rc = rects("windows/" + name)
    x, y, w, h = rc["menuWindow"]
    regs = [R("window-header", pad([x, y, w, 82]), "text", "#menuWindow top (tabs, the Prize Wheels line)"),
            R("window-body", pad([x, y + 82, w, h - 82]), "text",
              "#menuWindow body: the wheel cards; each shows a random item of its pool (yateShuffle), so only measured",
              skip=True)]
    if "shopSideSellB" in rc:
        hx, hy, hw_, hh = rc["shopSideSellB"]
        regs = [R("heroic-card-text", pad([hx, hy + hh * 2 // 3, hw_, hh // 3]), "text",
                  "#shopSideSellB lower third: Heroic Spin / 500 KR | ticket (the random item picture sits above;"
                  " the title's 30px black text-shadow glow differs at the glyph edges)", max_pct=5.0)]
    add(name, {"ref": "%s/windows/%s.png" % (REF, name), "size": [1280, 720], "background": MENU_BG,
               "state": dict(scen["menu-1280"]["state"], name="Player", account=spin_acc, **extra), "frames": 3, "note": note}, regs)
rc = rects("windows/win-spin-history")
add("win-spin-history", {"ref": "%s/windows/win-spin-history.png" % REF, "size": [1280, 720], "background": MENU_BG,
                         "state": dict(scen["menu-1280"]["state"], window=None, spin=0, spinHistory=SPIN_HISTORY,
                                       name="Player", account=spin_acc), "frames": 3,
                         "note": "The Starter wheel's Spin History popup (a spinH reply)"},
    [R("history-popup", pad(rc["spinHistoryPop"]), "text", "#spinHistoryPop: Stats / Recent Items / Load More, the date range, five items")])

# ------------------------------------------------------------------------ in-game
# In-game references are v7's sandbox (Free for All on Kanji, local player "Local User") with the
# 3D canvas hidden over flat black, so the render compares over the same flat black. The

# profile-layout: the signed-in Profile window (showWindow(5), windows[4].gen(): 1280 px wide when
# signed in, 955 px for the guest form) with a long name, a clan and a high level, at the menu
# sizes. capture-v7.js seasonWin('win-profile-acc-*', 5, 'profile') hands v7 the same account.
PROFILE_ACCOUNT = {"name": "xXLongPlayerNameXx", "clan": "KRUNK", "score": 12500000,
                   "stats": {"s": 50000, "h": 21000, "hs": 3000, "c0": 40000}}
# rows of the General Stats view in #menuWindow's own pixels at 1280x720 (764 px wide there),
# measured on the 1280 reference; scaled by the window's width at the other sizes
PROFILE_ROWS = [
    ("profile-tabs", 8, 58, "home / Friends / Partner / Settings / promo: KrunkNative has no Connections tab "
                            "(window.canConnectExternalAccounts, Twitch / Metamask)", True),
    ("account", 62, 160, "Your Account: the picture beside the Name / Clan / Level box (the clipped header of the bug)", False),
    ("stats-header", 224, 68, "Your Stats and the XP bar", False),
    ("stat-tabs", 292, 36, "the stat tabs: KrunkNative adds Medals (kn_brain medal counts)", True),
    ("stat-grid", 328, 126, "General Stats: the .pSt cards", False),
    ("logout", 460, 42, "Logout / Logout of all Sessions", False),
]
for tag, (w, h) in {"1280": (1280, 720), "1920": (1920, 1080), "1024": (1024, 768)}.items():
    name = "win-profile-acc-" + tag
    rc = rects("windows/" + name)
    regs = [r for r in chrome(rc, w, h, dimmed=True) if r["name"] in ("menu-items", "logo")]
    x, y, mw, _ = rc["menuWindow"]
    for r in regs:  # the logo runs behind the window's tab row (compared as profile-tabs): stop above it
        if r["name"] == "logo" and r["rect"][1] + r["rect"][3] > y - 2:
            r["rect"][3] = y - 2 - r["rect"][1]
    k = mw / 764.0
    for rname, ry, rh, note, skip in PROFILE_ROWS:
        regs.append(R(rname, pad([x + 6 * k, y + ry * k, mw - 12 * k, rh * k], 2, w, h), "text", note, skip=skip))
    st = dict(scen["menu-1280"]["state"], window=5, name=PROFILE_ACCOUNT["name"])
    st["account"] = dict(season_account("free"), **PROFILE_ACCOUNT)
    add(name, {"ref": "%s/windows/%s.png" % (REF, name), "size": [w, h], "background": MENU_BG, "state": st, "frames": 3,
               "note": "Profile, signed in at %dx%d: long name, clan, level 106 (the window is 1280 px wide like v7's)" % (w, h)},
        regs)

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
          "mapFile": V3_KANJI,
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


def game(name, state, regs, note, size=(1280, 720), frames=3, background=GAME_BG):
    add(name, {"ref": "%s/ingame/%s.png" % (REF, name), "size": list(size), "background": background, "state": state,
               "frames": frames, "note": note}, regs)


# The death screen slides in (death-ui-moveup / chat-moveup, 1.5 s ease): capture-v7.js took the
# reference settle(800) after the kill, so the render is taken that long after the death too.
DEATH_FRAMES = 6
DEATH_AFTER_MS = 820


game("ingame-plain", game_state(hud()), hud_regions(rects("ingame/ingame-plain")),
     "Plain HUD in the v7 sandbox (FFA on Kanji), full ammo, over flat black.")
game("ingame-scoped", game_state(hud(aim=1)),
     [r for r in hud_regions(rects("ingame/ingame-scoped")) if r["name"] != "crosshair"] +
     [R("aim-dot", [610, 330, 60, 60], "image", "aiming the Assault Rifle: the overlay's red aim dot, no crosshair")],
     "Assault Rifle aimed (right mouse held): crosshair hidden, red aim dot.")
game("ingame-reloading", game_state(hud(ammo=27, reloading=True)), hud_regions(rects("ingame/ingame-reloading")),
     "3 shots fired, R pressed: 27 | 28 while reloading.")
# the interact prompt (Freight's "pickup Junk" interact-key trigger, players.js updateInteract type 7):
# v7 writes the markup, display and bottom: 220px itself (capture-v7.js FORCE_INTERACT); natively
# state.interact puts the same markup through ui_interact_tick
rc = rects("ingame/ingame-interact")
game("ingame-interact", game_state(hud(), interact='Press <span style="color:#FBC02D">[G]</span> to pickup Junk'),
     hud_regions(rc, [R("interact", pad(rc["interactMsg"], 2), "text", "#interactMsg: Press [G] to pickup Junk, bottom 220px")]),
     "Interact prompt of an onInteractKey trigger (Freight's Junk): #interactMsg 220px above the bottom.")
# the weapon charge bar (game-overlay.js interactMProg at the progM* settings' defaults: rotated
# 270 deg, left 56 %, top 50 %, 400 x 30 * 0.5 * uiScale, filling upwards, black shadow stroke).
# capture-v7.js pins the local player's interMProg / overCharged; natively state.charge
# (build_ui_scenario). The grey pages show its rgba(0,0,0,.4) back and stroke. Regions: the bar
# from v7's geometry (uiScale 0.579 at 1280 x 720, 0.869 at 1920 x 1080) padded by 4 px; fuzz 15 %:
# the fill's sub-pixel left edge under the 1 px black stroke is 0x20 in Chromium, 0x00 natively.
CHARGE_GREY = "#808080"
CHARGE_FUZZ = 15
game("ingame-charge", game_state(hud(), charge=0.5),
     [r for r in hud_regions(rects("ingame/ingame-charge")) if r["name"] == "crosshair"] +  # the HUD: ingame-plain
     [R("charge-bar", [712, 298, 18, 124], "image",
        "interactMProg at 0.5: upright, right of the crosshair, lower half white", fuzz=CHARGE_FUZZ)],
     "Charge bar (Charge Rifle / Crossbow / knife throw) half charged: v7's interactMProg.")
game("ingame-charge-1920", game_state(hud(), charge=0.5),
     [R("charge-bar", [1071, 449, 22, 182], "image", "interactMProg at 0.5, uiScale 0.869", fuzz=CHARGE_FUZZ)],
     "Charge bar half charged at 1920 x 1080.", size=(1920, 1080))
game("ingame-charge-over", game_state(hud(), charge=1.1),
     [R("charge-bar", [706, 296, 26, 128], "image",
        "interactMProg at 1.1 (an ovrChrg weapon past full): red, 1.2x thicker, shaken -5.2 px",
        fuzz=CHARGE_FUZZ)],
     "Charge bar over-charging (prog 1.1) over grey: oColor fill, the back and the shadow stroke.",
     background=CHARGE_GREY)
game("ingame-charge-cross", game_state(hud(), charge={"prog": 0, "over": True}),
     [R("charge-bar", [694, 296, 54, 128], "image", "overCharged: the empty back and the red cross", fuzz=CHARGE_FUZZ)],
     "Over-charged weapon (overCharged, prog 0) over grey: the red cross on the empty bar.",
     background=CHARGE_GREY)
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
# The objective modes' HUD (the public rotation), each on Kanji with the sandbox playing that mode
# (capture-v7.js modeScenario: map config.modes). v7 hides #teamScores until a team has scored and
# then lists only the teams that did (index.js f1996); #gameMessage (in #topMidHold: top -5px,
# scale .8) carries the gmsg and the CTF notices (Game.updateFlags: config.flagMsg / flagMsgE /
# flagMsgRC). The #game-overlay canvas (flag pings, objective / zone markers, capture bars) is
# hidden in the references: KrunkNative draws it on its own layer (hud_objectives.c), which
# --render-scenario leaves out. state.modeHud pins what the sandbox's packets would set.
def mode_regions(rc):
    regs = [dict(r, skip=True, note=r["note"] + " (v7 draws it on #game-overlay, hidden in these references)")
            if r["name"] == "crosshair" else r for r in hud_regions(rc)]
    regs.append(R("game-message", [400, 0, 480, 30], "text",
                  "#gameMessage band (#topMidHold top -5px, scale .8): the gmsg / CTF flag notice, or empty"))
    regs.append(R("team-scores", pad(rc["teamScores"], 3) if "teamScores" in rc else [9, 84, 200, 30], "text",
                  "#teamScores under #matchInfo: hidden until a team scores, then only the teams that scored"))
    return regs


def mode_game(name, mode_name, mode_hud, note, chat=()):
    game(name, game_state(hud(), chat=chat, modeName=mode_name, modeHud=mode_hud),
         mode_regions(rects("ingame/" + name)), note)


mode_game("ingame-mode-point", "Hardpoint", {"team": 1, "gameMessage": "next objective 00:45"},
          "Hardpoint at spawn: no team has scored (no #teamScores), gmsg next objective 00:45 (pinned).")
mode_game("ingame-mode-point-scored", "Hardpoint",
          {"team": 1, "teamScores": [30, None], "gameMessage": "next objective 00:45"},
          "Hardpoint, team 1 (the local player's) has 30: #teamScores with that team only, white square.")
mode_game("ingame-mode-ctf", "Capture the Flag", {"team": 1, "gameMessage": ""},
          "Capture the Flag at spawn: no #teamScores, no notice.")
mode_game("ingame-mode-ctf-scored", "Capture the Flag",
          {"team": 1, "teamScores": [1, None], "flagInfo": ["AT BASE", "AWAY"], "gameMessage": ""},
          "Capture the Flag, team 1 (the local player's) captured once: its row with the blue flag icon and AT BASE (team 2 has no row).")
mode_game("ingame-mode-ctf-enemyflag", "Capture the Flag",
          {"team": 1, "flagCarrier": ["enemy", None], "flagInfo": ["Bot", "AT BASE"], "gameMessage": ""},
          "Capture the Flag, a bot carries the local player's flag: #gameMessage Enemy has your Flag (flagMsgE).",
          chat=BOT_CHAT)
mode_game("ingame-mode-kc", "Kill Confirmed", {"team": 1, "gameMessage": ""},
          "Kill Confirmed at spawn: no #teamScores.")
mode_game("ingame-mode-dom", "Domination", {"team": 1, "gameMessage": ""},
          "Domination at spawn: no #teamScores (v7 Kanji has dom zones; 311 Kanji has none, the HUD is the same).")
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
     game_state(hud(health=0, deaths=1), chat=BOT_CHAT, dead=True, after_ms=DEATH_AFTER_MS,
                killcard={"killer": "Bot", "weapon": "Desert Eagle", "weaponIcon": "icon_11", "level": 1, "myLevel": 1,
                          "score": 0, "classIndex": 10, "stats": [4000, 0, "0%", 0, 0]}),
     [R("chat", pad(rc["chatHolder"], 2), "text", "#chatHolder (moves right while dead)")] + DEATH_REGS,
     "Killed by a sandbox bot (server-side players.kill): v7's head-to-head death screen.", frames=DEATH_FRAMES)
rc = rects("ingame/ingame-death-self")
game("ingame-death-self",
     game_state(hud(health=0, deaths=1), dead=True, after_ms=DEATH_AFTER_MS, killcard={"stats": [2000, 0, "0%", 0, 0]}),
     [R("chat", pad(rc["chatHolder"], 2), "text", "#chatHolder"),
      R("death-self", [595, 580, 90, 110], "text", "`You` / `Died` (death-row-center-self)"),
      R("death-time", [705, 570, 150, 25], "text", "Time Survived: differs per run", skip=True),
      R("death-stats", [705, 595, 150, 90], "text", "Shots Fired / Accuracy / Kills / Score rows"),
      R("empty-top-left", [0, 0, 300, 100], "flat", "HUD hidden", expect="background")],
     "Killed with no killer: `You Died` + stats.", frames=DEATH_FRAMES)
# player-viewer WP-E part 2a. The v7 death screen with an account's data on the killer (class
# level from its class stat, the KDR player card stat) and this life's stats; the killer's class
# and weapon are what the sandbox bot had in the capture (capture-v7.js prints them).
rc = rects("ingame/ingame-death-v7")
game("ingame-death-v7",
     game_state(hud(health=0, deaths=1), chat=BOT_CHAT, dead=True, after_ms=DEATH_AFTER_MS,
                killcard={"killer": "Bot", "weapon": "Shotgun", "weaponIcon": "icon_6", "level": 2, "myLevel": 1,
                          "score": 0, "classIndex": 4, "pcStat": ["KDR", "2.50"], "stats": [65000, 42, "38.1%", 3, 450]}),
     [R("chat", pad(rc["chatHolder"], 2), "text", "#chatHolder (moves right while dead)"),
      R("death-weapons", [400, 565, 440, 65], "text", "[Shotgun] icon, kills 1 - 0, [Assault Rifle] icon"),
      R("death-cards", [365, 655, 450, 55], "image", "class images + lvl 2 / lvl 1, Bot + `2.50 KDR` VS Local User"),
      R("death-stats", [945, 565, 180, 125], "text", "Time Survived 1m 5s / Shots Fired 42 / Accuracy 38.1% / Kills 3 / Score 450"),
      R("killer-tag", [1080, 330, 80, 30], "scene", "the kill cam turns to the bot: its 3D nametag (not drawn by "
        "--render-scenario)", skip=True),
      R("empty-top-left", [0, 0, 300, 100], "flat", "HUD hidden on the death screen", expect="background")],
     "v7 death screen: class level, player card stat and life stats from the \"3\" packet (NET_PACKET_DEATH_INFO).",
     frames=DEATH_FRAMES)
rc = rects("ingame/ingame-spectate")
game("ingame-spectate",
     game_state(hud(timerText="03:44"), chat=BOT_CHAT, spectate=True),
     [R("spec-names", pad(rc["specNames"], 2), "text", "#specNames: Spectating (free cam)"),
      R("spec-info", pad([591, 12, 98, 52], 2), "text", "#specTimer 03:44 + #specGameInfo Free For All"),
      R("spec-team", pad(rc["specTeam0"], 2), "image", "#specTeam0: the sandbox bot's row (class / name differ)",
        skip=True),
      R("spec-controls", pad(rc["specControlHolder"], 2), "text",
        "#specControlHolder: Prev / Next / Free Cam [F] / Player/Cam / First Person [R] / Focus [.] / Force "
        "Nametags [M] / Minimap [-] (no Objective Cam row in FFA)"),
      R("chat", pad(rc["chatHolder"], 2), "text", "#chatHolder"),
      R("empty-bottom-left", [300, 600, 700, 100], "flat", "no player HUD while spectating", expect="background")],
     "Spectating the sandbox (Spectate toggle on): free cam, v7's #spectateUI.")
rc = rects("ingame/ingame-minimap")
game("ingame-minimap",
     game_state(hud(), customMap=False, mapFile="maps/kanji.json", pose=[144, -139, -182, -1.5707963267948966, 0]),
     hud_regions(rc, [R("minimap", pad(rc["miniMapOverlay"], 2), "text",
                        "#miniMapCanvas: Kanji's mmId image turned with the view, the N/E/S/W ring, our arrow")]),
     "The minimap on Kanji (an official map in KrunkNative; the reference clears v7's customsData) at a fixed pose.")
# the same on other maps (capture-v7.js minimapScenario: the pose v7's sandbox spawned at)
for mm_name, mm_map, mm_file, mm_pose, mm_what in (
        ("ingame-minimap-sandstorm", "Sandstorm", "maps/sandstorm_v3.json", [-39, 33, -210, 0, 0], "Sandstorm's mmId image"),
        ("ingame-minimap-burg", "Burg", "maps/burg_new.json", [246, 0, -210, -3.141592653589793, 0], "Burg's mmId image"),
        ("ingame-minimap-311-burg", "Burg", "maps/311_burg.json", [102, 0, -365, -1.5707963267948966, 0],
         "v3.1.1 Burg (no mmId): its blocks, darker the higher")):
    rc = rects("ingame/" + mm_name)
    game(mm_name,
         game_state(hud(), customMap=False, mapName=mm_map, mapFile=mm_file, pose=mm_pose),
         hud_regions(rc, [R("minimap", pad(rc["miniMapOverlay"], 2), "text",
                            "#miniMapCanvas: %s turned with the view, the N/E/S/W ring, our arrow" % mm_what)]),
         "The minimap on %s (%s) at the pose v7's sandbox spawned at." % (mm_map, mm_file))
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

# --------------------------------------------------------------------------- chat at other sizes
# #chatHolder follows #uiBase's scale (scaleBase) and its .onMenu / .onGame places (main.css):
# the menu, ESC over a game (.onGame dropped: back beside the menu) and in game at 1024x768,
# 1920x1080 and a tall 720x1280 window, plus #chatList grown to its max-height (chatHeight
# 2.5 = 250 px) and scrolled to the newest of 12 lines. Only the chat is compared here.
CHAT_SIZES = {"1024": (1024, 768), "1920": (1920, 1080), "tall": (720, 1280)}
BOT_LINES = [{"text": "Spawned a Bot" if i % 2 == 0 else "Cleared Bots", "color": "#fc03ec"} for i in range(12)]


def chat_regions(rc, w, h):
    regs = []
    for name, key, note in (("chat-input", "chatInputHolder", "#chatInputHolder: channel icon, Enter Message, mic [V]"),
                            ("chat-rows", "chatList", "#chatList rows")):
        if key in rc:
            regs.append(R(name, pad(rc[key], 2, w, h), "text", note))
    return regs


for tag, (w, h) in CHAT_SIZES.items():
    if tag != "1920":
        rc = rects("menu/menu-" + tag)
        add("menu-" + tag, {"ref": "%s/menu/menu-%s.png" % (REF, tag), "size": [w, h], "background": MENU_BG,
                            "state": dict(scen["menu-1280"]["state"]), "frames": 3,
                            "note": "The menu at %dx%d: only the chat is compared (#chatHolder at .onMenu)." % (w, h)},
            chat_regions(rc, w, h))
        rc = rects("ingame/ingame-pause-" + tag)
        add("ingame-pause-" + tag, {"ref": "%s/ingame/ingame-pause-%s.png" % (REF, tag), "size": [w, h],
                                    "background": GAME_BG,
                                    "state": dict(scen["menu-1280"]["state"], inGame=True, name="Local User",
                                                  chat=BOT_CHAT), "frames": 3,
                                    "note": "ESC in game at %dx%d (a bot spawned first): the chat back at .onMenu." % (w, h)},
            chat_regions(rc, w, h))
    rc = rects("ingame/ingame-chat-" + tag)
    game("ingame-chat-" + tag, game_state(hud(), chat=BOT_CHAT), chat_regions(rc, w, h),
         "In game at %dx%d with the Spawned a Bot line: the chat at .onGame." % (w, h), size=(w, h))
rc = rects("ingame/ingame-chat-lines")
game("ingame-chat-lines", game_state(hud(), chat=BOT_LINES), chat_regions(rc, 1280, 720),
     "12 sandbox lines: #chatList at its 250 px max-height, scrolled to the newest.")

rc = rects("ingame/ingame-spectate-menu")
add("ingame-spectate-menu", {"ref": REF + "/ingame/ingame-spectate-menu.png", "size": [1280, 720], "background": GAME_BG,
                             "state": dict(scen["menu-1280"]["state"], inGame=True, name="Local User", spectate=True,
                                           chat=BOT_CHAT), "frames": 3,
                             "note": "ESC while spectating: .onGame and .onSpect dropped, the chat back at .onMenu."},
    chat_regions(rc, 1280, 720))

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


# --------------------------------------------------------------------------- editor
# v7's editor.html at 1920x937 (#uiBase scale 1), captured by tools/editor/v7ref.js
# (doc/ref/krunker-v7.1.4/editor/). The 3D view (three.js grid and sky vs the native GL view)
# is skipped; the chrome over it (canvas buttons, stats box) is compared with a loose budget.
EW, EH = 1920, 937


def editor_chrome(rc, dimmed=False):
    k = "dimmed under the window backdrop; " if dimmed else ""
    regs = []

    def opt(name, key, kind, note, **kw):
        if key in rc:
            regs.append(R(name, pad(rc[key], 2, EW, EH), kind, k + note, **kw))

    opt("toolbar", "_toolbar", "text", "#_toolbar: File .. Documentation")
    opt("playbar", "playbar", "image", "#playbar: test / host / teleport icons")
    opt("version", "versionTxt", "text", "#versionTxt: Krunker Editor v2.0.0 - Yendis Entertainment")
    if "transform" in rc and "wireframeOption" in rc:
        x0, y0 = rc["transform"][0], rc["transform"][1]
        x1 = rc["wireframeOption"][0] + rc["wireframeOption"][2]
        regs.append(R("canvas-buttons", pad([x0, y0, x1 - x0, 30], 2, EW, EH), "text",
                      k + "#canvasUI buttons over the 3D view (translate .. Wireframe: OFF)", max_pct=6.0))
    # KrunkNative's label reads "Snapping: ON" at boot (snapping is on; v7's label says OFF:
    # an intentional difference, client/src/editor/editor.c)
    opt("snapping", "snappingOption", "text", "#snappingOption: ON natively (intentional), OFF in v7", skip=True)
    opt("canvas-info", "canvasInfo", "text", "#canvasInfo stats over the 3D view", max_pct=6.0)
    opt("canvas-objedit", "canvasObjEdit", "text", "#canvasObjEdit Delete / Duplicate", max_pct=6.0)
    opt("canvas-add", "canvasAdd", "image", "#canvasAdd +", max_pct=6.0)
    # the v7 Essential panel (editor WP4, editor_gameconfig.c)
    opt("left-panel", "left-panel", "text", "left panel: Essential (map config), editor WP4", max_pct=4.0)
    opt("right-panel", "right-panel", "flat", "right panel: blank")
    if "container" in rc:
        regs.append(R("view-3d", rc["container"], "scene", "3D view: v7 three.js vs the native GL view", skip=True))
    return regs


rc = rects("editor/editor-1920")
add("editor-1920", {"ref": REF + "/editor/editor-1920.png", "size": [EW, EH], "background": "#000000",
                    "state": {"page": "editor", "pageQuery": "new", "pageActions": ["closeWindow"], "anim_time": ANIM},
                    "frames": 3, "note": "Map editor (ui/editor.html), new map, Welcome window closed."},
    editor_chrome(rc))
# editor WP6 (editor_net.c): the API windows. v7's capture has no API, so its lists show "Loading..."
# forever; the render has no brain either: net:pending puts the list in that state (else the native
# list shows its empty text, an intentional difference checked by tools/editor/windows_check.py)
for name, ref, action, note in (("editor-win-settings", "window3", "showWindow(3)", "Settings window, General tab"),
                                ("editor-win-controls", "window1", "showWindow(1)", "Controls window, Default tab"),
                                ("editor-win-assets", "window2", ["showWindow(2)", "KE.kn.script('net:pending:2')"], "Assets window, New tab, request pending"),
                                ("editor-win-sounds", "window9", ["showWindow(9)", "KE.kn.script('net:pending:9')"], "Sounds window, New tab, request pending")):
    rc = rects("editor/" + ref)
    regs = [r for r in editor_chrome(rc, True) if r["name"] in ("toolbar", "version", "left-panel")]
    # Secular One rasterises about 1 px higher natively (FreeType vs Chromium): 4 %
    regs.append(R("window", pad(rc["menuWindow"], 2, EW, EH), "text", "#menuWindow: " + note, max_pct=4.0))
    actions = action if isinstance(action, list) else [action]
    add(name, {"ref": "%s/editor/%s.png" % (REF, ref), "size": [EW, EH], "background": "#000000",
               "state": {"page": "editor", "pageQuery": "new", "pageActions": actions, "anim_time": ANIM},
               "frames": 3, "note": "Map editor, new map, %s (%s)." % (note, actions[0])},
        regs)


# the object inspector (editor WP3, editor_inspector.c): a new cube's panel with Transform and Style
# open (v7ref.js state right-object-cube). The 3D view and the stats box show the cube differently
# (three.js vs the native view): only the toolbar, the version text and the right panel are compared.
rc = rects("editor/right-object-cube")
# The left panel is not compared here (editor-1920 compares the Essential panel).
regs = [R(r["name"], r["rect"], r["kind"], r.get("note"), skip=True, max_pct=r["max_pct"], fuzz=r["fuzz"])
        if r["name"] == "left-panel" else r
        for r in editor_chrome(rc) if r["name"] in ("toolbar", "version", "left-panel")]
regs.append(R("right-panel", pad(rc["right-panel"], 0, EW, EH), "text", "right panel: the Cube's object panel (Transform, Style open)", max_pct=4.0))
add("editor-inspector", {"ref": REF + "/editor/right-object-cube.png", "size": [EW, EH], "background": "#000000",
                         "state": {"page": "editor", "pageQuery": "new",
                                   "pageActions": ["closeWindow"] + ["KE.kn.script('%s')" % c for c in  # one command per call
                                                                     ("clear", "add:0", "select:0", "open:transformH", "open:styleH")],
                                   "anim_time": ANIM},
                         "frames": 3, "note": "Map editor, new map, a new Cube selected: the object panel (Transform, Style open)."},
    regs)

# the KrunkScript window (editor WP5, editor_script.c). v7.1.4 opens scripting.html as a browser
# window, so there is no rects capture: the reference is v7's own page (recovered,
# tools/editor/fetch_scripting_ref.sh) in an iframe at that window's place (tools/editor/v7ref.js
# scripting), and the script-* rects are measured on scripting.png. The toolbar and version rects
# are the editor-1920 chrome's.
regs = [{k: v for k, v in r.items() if r["name"] == "toolbar" or k != "note"}
        for r in editor_chrome(rects("editor/editor-1920")) if r["name"] in ("toolbar", "version")]
regs += [
    R("script-tabs", [192, 94, 400, 36], "text", "#tabHolder: Client (selected) / Server, GameFont 12px"),
    {"name": "script-strip", "rect": [600, 94, 940, 36], "kind": "flat",  # the file defaults (fuzz 6, max 0.5%)
     "note": "the empty tab strip (KrunkNative's close button sits right of it, at right:150px)"},
    R("script-label", [1585, 94, 143, 36], "text", "#label KRUNKSCRIPT"),
    R("script-gutter", [192, 130, 50, 656], "text",
      "line numbers and fold markers (CodeMirror measured its gutters at 22px: numbers end at 32px)"),
    R("script-code", [242, 130, 1486, 656], "text",
      "the client template in material-palenight colours; KrunkNative's caret is drawn (focused), v7's unfocused window has none"),
    R("script-compile", [192, 787, 1536, 57], "text",
      "#compile: Validate Script. The window ends at 80% of 937px (843.3): the native engine snaps it 1px higher than Chromium (~3% of this region)",
      max_pct=4.5),
]
add("editor-script-1920", {"ref": REF + "/editor/scripting.png", "size": [EW, EH], "background": "#000000",
                           "state": {"page": "editor", "pageQuery": "new",
                                     "pageActions": ["closeWindow", "KE.kn.script('script:open')"], "anim_time": ANIM},
                           "frames": 3,
                           "note": "Map editor, new map, the KrunkScript window (Scripting / key 0) on the client template. v7.1.4 opens "
                                   "scripting.html as a browser window; the reference is v7's own page (recovered, "
                                   "tools/editor/fetch_scripting_ref.sh) in an iframe at that window's place (tools/editor/v7ref.js scripting)."},
    regs, one_line_rects=True)


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
        text = json.dumps({"scenario": name, "defaults": {"fuzz": 6, "max_pct": 0.5}, "regions": regs}, indent=2)
        if name in inline_rects:
            text = re.sub(r'"rect": \[\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\]', r'"rect": [\1, \2, \3, \4]', text)
        with open(os.path.join(HERE, "regions", name + ".json"), "w") as fh:
            fh.write(text + "\n")
        print("%-24s %2d regions (%d skipped)" % (name, len(regs), sum(1 for r in regs if r.get("skip"))))


if __name__ == "__main__":
    main()

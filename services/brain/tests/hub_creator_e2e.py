#!/usr/bin/env python3
"""The hub's creator tools and friends sidebar through the native client (client/src/ui/social_ui.c), headless:
kn_brain on sqlite + one client_bin at a time, logged in from seeded storage and driven through KRUNKNATIVE_UI_CMDS
(the page's inline handlers, typing into its inputs). Every outcome is checked in the page and the database.

  hub_creator_e2e.py <native build dir> <repo root>

  1. the creator's map page (hub-10.0.7.js glb / sd / dd / dkd / mfnds, v10_catalog.c):
     the Leaderboard tab lists the seeded scores; the Data tab finds a player's keys, deletes one, then Delete By
     Key removes a key of every player; Edit -> Deposit / Withdraw moves the map funds (v10's KR Fee line,
     calcMapFee); Update Leaderboard republishes. The board, rows and popups are v10's markup (:3627-3822, 6653-6679)
  2. a mod's Edit (updateMod -> plm): the Update Mod popup prefilled with the mod's name
  3. a developer's Chat Ban on a profile (/moderation/chat); the profile's badges (the header's selected badge, the
     Badges tab's cards with their art, createBadgeTip); confPremium's gift question and kn_brain's refusal of "prm"
  4. Market > Trading > History pages (loadMoreTradeHistory)
  5. the chat sidebar (chtb): the friend's bubble, a message from them counts on it ("msg"), Friends lists them,
     Remove unfollows
  6. the menu's Upload Mod (window 19, v10's mods_pub, game.deob.js:220943-221050): v10's markup and texts, the
     Select links through the platform file dialog (a stand-in zenity prints the picked path), the size lines and
     titles onFileChange writes, and a publish that lands in published_mods and closes on "Mod Published!"
Screenshots go to the work dir (~/.cache/kn-hub-creator.*, kept on failure or with KN_KEEP=1).
77 = skip (no client_bin / kn_brain / websockets).
"""
import asyncio
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time
import zipfile
import struct
import zlib

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
CLIENT = os.path.join(BUILD, "client_bin")
if not (os.path.exists(BRAIN) and os.path.exists(CLIENT)):
    print("needs client_bin and kn_brain in %s: skip" % BUILD)
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-hub-creator.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
PREFAB_LEADERBOARD = 72
# the platform file dialog (ed_pick_file runs zenity): a stand-in that prints the path in PICK, the file "picked"
PICK_BIN = os.path.join(WORK, "bin")
PICK = os.path.join(WORK, "pick.txt")
os.makedirs(PICK_BIN, exist_ok=True)
with open(os.path.join(PICK_BIN, "zenity"), "w") as f:
    f.write("#!/bin/sh\ncat '%s'\n" % PICK)
os.chmod(os.path.join(PICK_BIN, "zenity"), 0o755)


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def q(sql, *args):
    con = sqlite3.connect(DB, timeout=30)
    try:
        rows = con.execute(sql, args).fetchall()
        con.commit()
        return rows
    finally:
        con.close()


def one(sql, *args):
    r = q(sql, *args)
    return r[0][0] if r and r[0][0] is not None else 0


def wait(fn, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return fn()


class Sock:
    def __init__(self, port):
        self.port, self.ws, self.inbox = port, None, []

    async def open(self):
        self.ws = await websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % self.port, max_size=None)
        return self

    async def call(self, label, *frame, timeout=15.0):
        await self.ws.send(json.dumps(list(frame)))
        end = time.time() + timeout
        while time.time() < end:
            f = json.loads(await asyncio.wait_for(self.ws.recv(), max(0.05, end - time.time())))
            if f and f[0] == "pi":
                await self.ws.send(json.dumps(["po"]))
            elif f and f[0] == label:
                return f
        return None

    async def close(self):
        await self.ws.close()


async def register(port, name):
    s = await Sock(port).open()
    f = await s.call("a", "a", 0, [name, "hub_pass_1", None])
    assert f and f[1] is None, f
    return s, f[2], f[5]


class Client:
    """One headless client_bin on the hub, as `name`."""

    def __init__(self, port, name, token, query, tag, hub=True):
        self.cmds = os.path.join(WORK, tag + ".cmds")
        self.log = os.path.join(WORK, tag + ".log")
        self.tag = tag
        cfg = os.path.join(WORK, tag + "-cfg")
        os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
        with open(os.path.join(cfg, "krunknative", "krunker_username.json"), "w") as f:
            f.write(name)
        with open(os.path.join(cfg, "krunknative", "krunker_token.json"), "w") as f:
            f.write(token)
        open(self.cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                    "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UI_CMDS": self.cmds, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                    "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port,
                    "KRUNKNATIVE_REMOTE_IMAGE_CACHE": os.path.join(WORK, tag + "-images"),  # the pictures it downloads
                    "PATH": PICK_BIN + os.pathsep + os.environ.get("PATH", "")})
        self.out = open(self.log, "w")
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT] + (["--hub", query] if hub else []), stdout=self.out, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=env, cwd=ROOT, start_new_session=True)

    def text(self):
        try:
            with open(self.log, errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def cmd(self, line):
        with open(self.cmds, "a") as f:
            f.write(line + "\n")

    def ask(self, line, pattern, timeout=3.0):
        """sends a printing command; the first match of pattern in what the client printed after it"""
        mark = len(self.text())
        self.cmd(line)
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pattern, self.text()[mark:])
            if m:
                return m
            time.sleep(0.1)
        return None

    def has(self, elem_id, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.ask("rect " + elem_id, r"#%s (rect|: no box)" % re.escape(elem_id), 1.5)
            if m and m.group(1) == "rect":
                return True
            time.sleep(0.3)
        return False

    def textof(self, sel):
        m = self.ask("textof " + sel, r"textof %s = '(.*)'" % re.escape(sel))
        return m.group(1) if m else None

    def text_has(self, sel, needle, timeout=15.0):
        return wait(lambda: needle in (self.textof(sel) or ""), timeout)

    def value(self, elem_id):
        m = self.ask("value " + elem_id, r"value %s = '(.*)'" % re.escape(elem_id))
        return m.group(1) if m else None

    def count(self, sel):
        m = self.ask("count " + sel, r"count %s = (\d+)" % re.escape(sel))
        return int(m.group(1)) if m else -1

    def ui(self, calls):
        self.cmd("ui " + calls)
        time.sleep(0.4)

    def type(self, elem_id, chars):
        self.cmd("clickid " + elem_id)
        time.sleep(0.4)
        self.cmd("text " + chars)
        time.sleep(0.5)

    def shot(self, name):
        path = os.path.join(WORK, "%s-%s.png" % (self.tag, name))
        self.cmd("shot " + path)
        wait(lambda: os.path.exists(path), 10)
        time.sleep(0.3)  # written in one go; let it finish
        return path

    def rect(self, what, nth=0):
        """(x, y, w, h) of #id, or of the nth match of a selector (rectsel: no click)"""
        if what[0] in "#.[" or " " in what:
            m = self.ask("rectsel %d %s" % (nth, what), r"sel %s\[%d\] (rect ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)|: no box)"
                         % (re.escape(what), nth))
        else:
            m = self.ask("rect " + what, r"#%s (rect ([-\d.]+) ([-\d.]+) ([-\d.]+) ([-\d.]+)|: no box)" % re.escape(what))
        return tuple(float(m.group(i)) for i in range(2, 6)) if m and m.group(2) else None

    def pixel_at_centre(self, what, name, nth=0):
        """the screenshot's colour at the centre of an element (r, g, b), None without a box"""
        r = self.rect(what, nth)
        if not r or r[2] < 2 or r[3] < 2:
            return None
        return png_pixel(self.shot(name), int(r[0] + r[2] / 2), int(r[1] + r[3] / 2))

    def close(self):
        self.cmd("quit")
        try:
            self.proc.wait(15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(5)
        self.out.close()


def map_page(port, ids, tokens, names, map_name, map_id, stamp):
    A, B = ids
    c = Client(port, names[0], tokens[0], "p=map&q=" + map_name, "creator")
    try:
        check(c.has("mapTabsP", 60), "the creator's map page")
        # the map's picture: map_info.t, so v10's <user assets>/m<id>/thumb.png (hub-10.0.7.js:3163), which kn_brain answers
        # with the PNG the editor's Publish sent (user_assets.c)
        thumb_url = "http://127.0.0.1:%d/user-assets/m%d/thumb.png" % (port, map_id)
        check(c.count(".mapPageThumb[src='%s']" % thumb_url) == 1, "the map page's thumbnail is m%d/thumb.png" % map_id)
        import urllib.request
        with urllib.request.urlopen(thumb_url, timeout=10) as resp:
            check(resp.status == 200 and resp.read() == png(64, 32), "kn_brain serves the published thumbnail at m%d/thumb.png" % map_id)
        rgb = None
        for _ in range(20):
            rgb = c.pixel_at_centre(".mapPageThumb", "map-thumb")
            if near(rgb, (128, 64, 32)):
                break
            time.sleep(0.5)
        check(near(rgb, (128, 64, 32)), "and the page draws it: %r" % (rgb,))
        # iíïïiìi(am(map_name) + " - Krunker Hub") (hub-10.0.7.js:3164, 7136-7141)
        check(c.textof("title") == map_name + " - Krunker Hub", "document.title: %r" % c.textof("title"))
        c.ui("updateMapTab(2, true)")
        ok = c.text_has("#mapLeaders", names[1])
        board = c.textof("#mapLeaders") or ""
        check(ok and names[0] in board and board.index(names[0]) < board.index(names[1]),
              "Leaderboard tab: both players, the lower score first (Score / ASC): %r" % board)
        check(board.startswith("PlayerScore") and "1. " + names[0] in board and "2. " + names[1] in board,
              "v10's board: Player / Score header, numbered rows (hub-10.0.7.js:3804-3822): %r" % board)
        c.shot("leaderboard")

        c.ui("updateMapTab(3, true)")
        check(c.has("searchDInput"), "the Data tab")
        c.type("searchDInput", names[1])
        c.ui("searchPData()")
        check(c.text_has("#dataList", "coins") and "gems" in (c.textof("#dataList") or ""), "Search lists the player's keys")
        check(c.count("#dataList .deleteDBtn") == 2 and (c.textof("#dataList") or "").startswith("KeyValue"),
              "v10's Data rows: Key / Value header, a delete_forever button per key (:3663-3681)")
        c.shot("data")
        c.ui("deleteData('%s', 'coins')" % names[1])
        check(wait(lambda: not q("SELECT 1 FROM ks_storage WHERE map_id = ? AND account_id = ? AND skey = 'coins'", map_id, B)),
              "the row's delete removed that key of that player")
        check(wait(lambda: "coins" not in (c.textof("#dataList") or "coins")), "and the row left the list")
        c.ui("deleteDataPop()")
        check(c.has("deleteDKey") and c.text_has("#genericPop", "Delete All From Key"), "Delete By Key asks for the key (v10's Delete Data popup)")
        c.ui("window.deleteDataConf()")
        check(c.text_has("#genericPop", "No Key Provided"), "no key: No Key Provided")
        c.ui("deleteDataPop()")
        c.type("deleteDKey", "gems")
        c.ui("window.deleteDataConf()")
        check(c.has("confirmBtn") and c.text_has("#genericPop", "This action is not reversible!"), "and asks again (YES / NO)")
        c.cmd("clickid confirmBtn")
        check(wait(lambda: not q("SELECT 1 FROM ks_storage WHERE map_id = ? AND skey = 'gems'", map_id)),
              "Delete By Key removed the key of every player")

        c.ui("clearPops();openEdit('%s', %d, 0)" % (map_name, map_id))
        check(c.has("mapFundsInp"), "Edit opens the creator's popup")
        check(c.text_has("#genericPop", "Open in Editor") and c.text_has("#genericPop", "Update Leaderboard") and
              c.count("#genericPop .mapFndB") == 2 and (c.textof("#mapFeeVal") or "") == "None",
              "v10's Edit popup: Open in Editor, Update Leaderboard, Withdraw / Deposit, KR Fee None")
        c.type("mapFundsInp", "300")
        check(wait(lambda: (c.textof("#mapFeeVal") or "").replace(" ", "") == "-30KR", 5),
              "calcMapFee: 300 KR shows a -30 KR fee: %r" % c.textof("#mapFeeVal"))
        c.shot("edit")
        c.ui("chngMapFunds(%d, 0)" % map_id)
        check(wait(lambda: one("SELECT map_funds FROM published_maps WHERE map_id = ?", map_id) == 270) and
              one("SELECT player_funds FROM players WHERE player_id = ?", A) == 700,
              "Deposit: 300 KR leave the account, the map gets 270 (the 30 KR fee the popup showed)")
        check(c.text_has("#genericPop", "Success!"), "mfr: v10's SUCCESS popup")
        c.ui("clearPops();openEdit('%s', %d, 270)" % (map_name, map_id))
        c.type("mapFundsInp", "100")
        c.ui("chngMapFunds(%d, 1)" % map_id)
        check(wait(lambda: one("SELECT map_funds FROM published_maps WHERE map_id = ?", map_id) == 170) and
              one("SELECT player_funds FROM players WHERE player_id = ?", A) == 790, "Withdraw: 100 KR from the map, 90 KR back")
        check(c.text_has("#profileKR", "790"), "the KR in the nav bar follows")

        upd = one("SELECT map_updatecounter FROM published_maps WHERE map_id = ?", map_id)
        c.ui("clearPops();openEdit('%s', %d, 170)" % (map_name, map_id))
        c.type("leaderTypeCust", "Laps")
        c.ui("updateMapLB(%d)" % map_id)
        check(wait(lambda: one("SELECT map_updatecounter FROM published_maps WHERE map_id = ?", map_id) == upd + 1),
              "Update Leaderboard republished the map")
        data = json.loads(one("SELECT map_data FROM published_maps WHERE map_id = ?", map_id) or "{}")
        lbs = [o for o in data.get("objects", []) if o.get("i") == PREFAB_LEADERBOARD]
        check(lbs and lbs[0].get("ldc") == "Laps", "the LEADERBOARD carries the custom stat: %r" % lbs)
        check(c.text_has("#mapLeaders", "Laps") and "No Records Yet!" in (c.textof("#mapLeaders") or ""),
              "the Leaderboard tab follows the new display (a custom stat: Box Office keeps no custom scores)")
        c.shot("leaderboard-custom")

        # 2: a mod's Edit
        c.ui("clearPops();updateMod('Hub Pack %s')" % stamp)
        check(c.has("modNameNew"), "Edit on a mod opens the Update Mod popup")
        check(c.value("modNameNew") == "Hub Pack " + stamp, "prefilled with the mod's name: %r" % c.value("modNameNew"))
        check("Update" in (c.textof(".pubHed") or ""), "titled as an update: %r" % c.textof(".pubHed"))
        check(c.count("#publishMod .publishLeft #modThumbSelect") == 1 and c.count("#publishMod .publishRight #modZipSelect") == 1 and
              c.text_has("#pubGame", "Update") and "/ 25" in (c.textof("#modZipSize") or "") and "/ 250" in (c.textof("#modThumbSize") or ""),
              "v10's Publish Mod markup: thumbnail / .zip Select, the 250 KB and 25 MB limits, the Update button (hub-10.0.7.js:6734-6754)")
        c.ui("uploadMod()")
        check(c.text_has("#modUpResp", "zip"), "Update with no .zip: the response line says so: %r" % c.textof("#modUpResp"))
        c.shot("update-mod")
        c.ui("clearPops();updateMod('Not Mine %s')" % stamp)
        check(c.text_has("#genericPop", "Mod not found"), "another account's mod: Mod not found")
    finally:
        c.close()


def staff_profile(port, ids, tokens, names):
    """a developer on bob's profile: Chat Ban (hub-10.0.7.js:2801-2813, 4700-4723) through /moderation/chat"""
    A, B = ids
    q("UPDATE players SET player_developer = 1 WHERE player_id = ?", A)
    q("INSERT INTO reports (reporter_id, target_id, reason, details) VALUES (?, ?, 'Scamming', 'took the KR and left')", A, B)
    q("INSERT INTO player_badges (account_id, badge) VALUES (?, 29), (?, 0)", B, B)
    q("UPDATE players SET player_stats = json_set(player_stats, '$.bdg', 29) WHERE player_id = ?", B)
    # season 9 (brain.ranked.season), placed (6 matches), 1000 + 100 * 3.5 = 1350: Gold 1 (rankedRank 7)
    q("INSERT INTO ranked_profiles (account_id, season, region, cr, wins, losses) VALUES (?, 9, 'na', '3.5', 4, 2)", B)
    c = Client(port, names[0], tokens[0], "p=profile&q=" + names[1], "staff")
    try:
        # the Reports tab (canViewReports, hub-10.0.7.js:2173-2175, 4176-4193)
        check(c.has("pTab_reports", 60), "a developer gets the profile's Reports tab")
        check(c.text_has("#pTabContent_reports", "Scamming") and "took the KR" in (c.textof("#pTabContent_reports") or "")
              and names[0] in (c.textof("#pTabContent_reports") or ""), "it lists the report: reason, reporter, text")
        c.ui("openProfileTab('reports')")
        c.shot("reports")
        check(c.has("chatBanBtn", 60), "a developer sees Chat Ban on a profile")
        check(c.text_has("#chatBanBtn", "Chat Ban"), "labelled Chat Ban (no ban in force)")
        c.ui("chatBanUser()")
        check(wait(lambda: q("SELECT 1 FROM feature_bans WHERE account_id = ? AND feature = 'chat' AND revoked_at IS NULL", B)),
              "Chat Ban: a chat ban in force for bob")
        check(c.text_has("#chatBanBtn", "Chat Unban"), "the button reads Chat Unban")
        c.ui("chatBanUser()")
        check(wait(lambda: not q("SELECT 1 FROM feature_bans WHERE account_id = ? AND feature = 'chat' AND revoked_at IS NULL", B)),
              "Chat Unban revoked it")
        badges(c)
        premium_gift(c, names)
    finally:
        c.close()
        q("UPDATE players SET player_developer = 0 WHERE player_id = ?", A)


def badges(c):
    """bob's badges (seeded: 0 Legacy Challenger, 29 VIP, selected 29): the header's customBadge, the Badges tab's cards
    with their art and createBadgeTip (hub-10.0.7.js:2778-2780, 2966-2977, 6879-6885)"""
    check(c.count("#profileHolder .customBadge[src$='img/badges/badge_18.png']") == 1,
          "the header shows the selected badge (stats.bdg 29, VIP: badge_18.png)")
    check(c.count("#profileHolder .leftTopHolder .playerProfileRank[src$='img/ranked/ranks/rank_gold.svg'][alt='Rank']") == 1,
          "the header's rankedRank: Gold 1, rank_gold.svg (hub-10.0.7.js:2780)")
    c.ui("openProfileTab('badges')")
    check(wait(lambda: c.count("#pTabContent_badges .badgeCard") == 2, 10), "the Badges tab: a card per earned badge")
    check(c.count("#pTabContent_badges .badgeCard[data-index='0'] .badgeImg[src$='textures/badges/badge_0.png']") == 1 and
          c.count("#pTabContent_badges .badgeCard[data-index='29'] .badgeImg[src$='img/badges/badge_18.png']") == 1,
          "each with its art (textures/badges/badge_0.png, img/badges/badge_18.png)")
    check(c.count("#pTabContent_badges .noneText") == 0, "no none text")
    c.cmd("clicksel 0 #pTabContent_badges .badgeCard")
    check(c.text_has(".tooltip", "Legacy Challenger", 5) and "Completed Old Challenge Lvl 30" in (c.textof(".tooltip") or ""),
          "createBadgeTip: the badge's name and description: %r" % c.textof(".tooltip"))
    c.shot("badges")
    c.cmd("move 5 5")
    check(wait(lambda: c.count(".tooltip") == 0, 5), "cancelTip on mouseout removes it")


def premium_gift(c, names):
    """confPremium / premiumGiftConfirmed -> "prm" (hub-10.0.7.js:5751-5759): real money, refused by kn_brain"""
    c.ui("clearPops();confPremium(1)")
    check(c.text_has("#genericPop", "Are you sure you want to Gift '%s' 30 days of Premium?" % names[1]),
          "confPremium(1): the gift question names the profile and 30 days: %r" % c.textof("#genericPop"))
    c.shot("premium-gift")
    c.cmd("clickid confirmBtn")
    check(c.text_has("#genericPop", "Purchases are not available in Krunker Box Office."),
          "YES (premiumGiftConfirmed): kn_brain's no-payments answer: %r" % c.textof("#genericPop"))


def trade_history(port, ids, tokens, names):
    """Market > Trading > History: a page of 30, Load More for the rest (hub-10.0.7.js:2690-2702, 3844-3890)"""
    A, B = ids
    for i in range(35):
        q("INSERT INTO trades (sender_id, receiver_id, status, closed_at) VALUES (?, ?, 'declined', strftime('%Y-%m-%d %H:%M:%f', 'now'))",
          A if i % 2 else B, B if i % 2 else A)
    q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 20, 1)", A)
    q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 867, 1)", A)  # Clickbait: a 2-frame spray, frameT 550
    q("INSERT INTO market_listings (seller_id, skin_id, price) VALUES (?, 21, 1234)", A)
    c = Client(port, names[0], tokens[0], "p=market&q=trades", "trades")
    try:
        check(c.has("o_his", 60), "the Trading tab")
        # iiîiïií (hub-10.0.7.js:2677-2714): three .offers columns side by side, no tabs
        check(c.count("#marketList .offers.scrollItem") == 3 and c.count("#tradeNav") == 0 and c.has("o_in", 5) and c.has("o_out", 5),
              "Incoming, Outgoing and History columns all shown, no tab bar")
        check(wait(lambda: c.count("#o_his .offerHolder[id]") == 30, 15), "History shows the first 30: %d" % c.count("#o_his .offerHolder[id]"))
        check(c.count("#o_his .loadMoreHis") == 1, "and Load More")
        check(c.textof("#hisCnt") == "30", "the History head counts #hisCnt: %r" % c.textof("#hisCnt"))
        date = c.textof("#o_his .offerHolder[id] .offerDate") or ""
        check(re.fullmatch(r"just now|\d+[mhd] ago", date) is not None, "a row's age is v10's (largest unit only): %r" % date)
        # iíîììîì's onclick: viewOffer(trid, we, "name", "ids", "ids") (hub-10.0.7.js:2661-2665)
        check(c.count("#o_his .offerHolder[onclick^='viewOffer('][onclick*=',2,\"']") == 30, "rows call viewOffer(trid, 2, \"name\", ...)")
        c.shot("history")
        c.ui("loadMoreTradeHistory()")
        check(wait(lambda: c.count("#o_his .offerHolder[id]") == 35, 10), "Load More: all 35")
        check(c.count("#o_his .loadMoreHis") == 0 and c.textof("#hisCnt") == "35", "no more pages; #hisCnt reads 35")
        check(c.textof("title") == "Krunker Hub", "the market's document.title is the default: %r" % c.textof("title"))
        # the settings' image collection (hub-10.0.7.js:6335-6440; image_collection.c)
        c.ui("clearPops();imagePopup('customScope')")
        check(c.has("imagesTabLayout"), "imagePopup opens the image collection")
        c.ui("imageNewPopup()")
        check(c.has("newCustName"), "Add an image asks for a name and a link")
        c.type("newCustName", "Blue Scope")
        c.type("newCustImg", "https://example.com/scope.png")
        c.ui("addNewImage()")
        check(c.text_has(".kn-image-grid", "Blue Scope", 10), "the new image is in the collection")
        # the Inventory / My Sales stats (#invStats, hub-10.0.7.js:3308-3349)
        c.ui("clearPops();updateWindow('market','inventory',null,null,'store')")
        check(wait(lambda: re.match(r"^2 / \d+ ItemsEst\. Value [\d,]+ KR$", c.textof("#invStats") or ""), 15),
              "Inventory: N / <skins> Items, Est. Value: %r" % c.textof("#invStats"))
        # the spray's sheet animates (config-10.0.7.js:4870, hub-10.0.7.js:3334-3353): animation sprayAnm<frames>
        # (1 + frameT / 1000)s steps(frames), the list's @keyframes moving it frames * 200px
        spray = "#marketList .sprayMarketImg[style*='sprayAnm2 1.55s steps(2) infinite']"
        check(wait(lambda: c.count(spray) == 1, 10) and c.count("#marketList style") == 1,
              "Clickbait's card has v10's animation and the list its keyframes: %d" % c.count(spray))
        seen = []
        for k in range(8):
            rgb = c.pixel_at_centre("#marketList .sprayMarketImg", "spray-%d" % k)
            if rgb and not any(near(rgb, o, 30) for o in seen):
                seen.append(rgb)
            time.sleep(0.25)
        check(len(seen) >= 2, "and the card steps through its two frames: %r" % (seen,))
        c.ui("updateWindow('market','sales',null,null,'store')")
        check(wait(lambda: (c.textof("#invStats") or "") == "1 / 20 On SalePotential Earnings 1,234 KR", 15),
              "My Sales: 1 / 20 On Sale, Potential Earnings (the listing's price): %r" % c.textof("#invStats"))
    finally:
        c.close()


def market_cards(port, ids, tokens, names):
    """v10's market cards (hub-10.0.7.js:3187-3361): the inventory's (data-index, estimate, List / Quick Sell, Info |
    Preview), Only Show Missing's read-only cards, Stats' In Circulation, the Black Market's with blackMDone; the item
    page (hub-10.0.7.js:3418-3494); a game result's table and a KR giveaway in the feed (hub-10.0.7.js:4875-4944)"""
    A, B = ids
    with open(os.path.join(ROOT, "client", "src", "ui", "economy_black_market_table.c")) as f:
        on_sale = len(re.findall(r"\.hide = 0, \.free_ = 0\}", f.read()))
    c = Client(port, names[0], tokens[0], "p=market&q=inventory", "cards")
    try:
        card = "#marketList .marketCard[data-index='20']"
        check(wait(lambda: c.count(card) == 1, 60), "Inventory: Kodac AK's card carries data-index 20")
        check(c.count(card + "[onmouseover='createTip(this)']") == 1 and c.count(card + "[id^='itemCardinventory_']") == 1,
              "its id is itemCardinventory_<uid>, createTip on hover")
        check(c.count(card + " .cardEst") == 1 and c.count(card + " .cardCnt") == 1, "the estimate and the count")
        acts = c.textof(card + " .cardActions") or ""
        check(acts == "ListQuick SellInfoPreview", "List, Quick Sell, Info | Preview: %r" % acts)
        check(c.count(card + " .cardActionSepVert") == 1, "Info and Preview split by cardActionSepVert")
        c.cmd("clicksel 0 %s .cardAction" % card)  # List: showPopup("inventory", 20, thumbnail, label, ...)
        check(c.has("sellPriceIn"), "List opens showPopup's inventory popup with the price input")
        c.shot("list-popup")
        c.ui("clearPops()")
        c.cmd("clicksel 1 %s .cardAction" % card)  # Quick Sell: confQSPopup
        check(c.has("quickSellBtn") and c.text_has("#quickSellBtn", "Get KR"), "Quick Sell: confQSPopup with Get KR")
        check(c.has("declineBtn", 5), "and NO")
        c.ui("clearPops()")
        # sortPopup's Only Show Missing (hub-10.0.7.js:4518-4600): the store items not owned, read-only
        c.ui("sortPopup()")
        check(c.has("invSortTxt") and c.count("#onlyMissing") == 1 and c.count("#nonTradable") == 1,
              "sortPopup: Only Show Missing and Hide Non-Tradable Skins")
        check("Only Show Missing" in (c.textof("#genericPop") or "") and "Hide Non-Tradable Skins" in (c.textof("#genericPop") or ""),
              "both labelled: %r" % c.textof("#genericPop"))
        c.type("invSortTxt", "Seafarer")
        c.cmd("clicksel 0 .filterSwitch")  # the label's click toggles its hidden checkbox
        time.sleep(0.4)
        c.ui("runSort()")
        miss = "#marketList .marketCard[data-index='21']"
        check(wait(lambda: c.count(miss) == 1, 15), "Only Show Missing lists Seafarer AK (not owned): %r" % (c.textof("#marketList") or "")[:300])
        c.shot("missing")
        check(c.count("#marketList .marketCard[data-index='20']") == 0, "and not the owned Kodac AK")
        check(c.count(miss + " .cardEst") == 0 and c.count(miss + " .cardCnt") == 0, "read-only: no estimate, no count")
        acts = c.textof(miss + " .cardActions") or ""
        # hub-10.0.7.js:3336: the List label stays, without its showPopup onclick
        check(acts == "ListSearchInfoPreview", "read-only: List, then Search instead of Quick Sell: %r" % acts)
        check(c.count(miss + " .cardAction[onclick]") == 3, "read-only: List has no onclick (Search, Info, Preview do)")
        check(c.count("#marketList .marketCard[data-index='21'] .cardAction[onclick^='showPopup(']") == 0, "no showPopup")
        c.ui("clearPops();resetSortFilter(1)")
        # Stats (hub-10.0.7.js:3197-3226): a card per item with In Circulation
        c.ui("clearPops();updateWindow('market','stats',null,null,'store')")
        st = "#marketList .marketCard[data-index='20']"
        check(wait(lambda: c.count(st + " .cardEst") == 1, 15), "Stats: Kodac AK's card")
        check(c.textof(st + " .cardEst") == "In Circulation 1", "In Circulation 1: %r" % c.textof(st + " .cardEst"))
        acts = c.textof(st + " .cardActions") or ""
        check(acts == "PreviewSearchInfo", "Stats: Preview, Search, Info: %r" % acts)
        check(c.count("#marketList .rarestItem") == 0, "no v3.1.1 Rarest Items list")
        c.shot("stats")
        # the Black Market (hub-10.0.7.js:3229-3264): every blackM item not hidden or free, owned ones ticked
        c.ui("updateWindow('market','blackm',null,null,'store')")
        bm = "#marketList .marketCard[data-index='1483']"
        check(wait(lambda: c.count(bm) == 1, 15), "Black Market: 1 Year Cape's card")
        check(c.count("#marketList .marketCard") == on_sale, "a card per blackM item on sale: %d of %d" % (c.count("#marketList .marketCard"), on_sale))
        check(c.textof(bm + " .marketPrice") == "100 KR", "its price: %r" % c.textof(bm + " .marketPrice"))
        check((c.textof(bm + " .cardActions") or "") == "PurchaseInfoPreview", "Purchase, Info | Preview: %r" % c.textof(bm + " .cardActions"))
        check(c.count("#marketList .blackMDone") == 0, "nothing owned: no blackMDone")
        q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 1483, 1)", A)
        c.ui("updateWindow('market','market',null,null,'store')")
        time.sleep(1)
        c.ui("updateWindow('market','blackm',null,null,'store')")
        check(wait(lambda: c.count(bm + " .blackMDone") == 1, 15), "owned: 1 Year Cape's card gets blackMDone")
        check(c.count("#marketList .blackMDone") == 1, "only that one")
        c.cmd("clicksel 0 %s .cardAction" % bm)  # Purchase: showPopup("blackm", ...)
        check(c.has("popupContent") and c.text_has("#popupContent", "100", 5),
              "Purchase opens the Black Market popup with its price: %r" % c.textof("#popupContent"))
        c.shot("blackm")
    finally:
        c.close()
    # the item page (hub-10.0.7.js:3418-3494)
    c = Client(port, names[0], tokens[0], "p=itemsales&i=20", "item")
    try:
        check(c.text_has("#weaponInfo", "Kodac AK", 60), "the item page names the item")
        info = c.textof("#weaponInfo") or ""
        check("In Circulation" in info and "units" in info and "Season" in info and "3D" in info, "On Sale / In Circulation, Season, 3D: %r" % info)
        check(c.count("#weaponInfo .minMax") == 2, "the high / low boxes")
    finally:
        c.close()
    # feed posts: a game result (typ 8) and a KR giveaway (typ 9)
    rows = json.dumps(["s1", names[0], 500, 10, 2, 30, "s2", "rival", 100, 3, 5, 10])
    q("INSERT INTO feed_posts (author_id, body, type, link, link2) VALUES (?, 'gg', 8, '0', ?)", A, rows)
    q("INSERT INTO feed_posts (author_id, body, type, funds, ends_at) VALUES (?, 'free KR', 9, 500, "
      "strftime('%Y-%m-%d %H:%M:%f', 'now', '+2 hours'))", A)
    c = Client(port, names[0], tokens[0], "p=feed", "feed")
    try:
        check(wait(lambda: c.count("#postGameRes") == 1, 60), "the game result's table")
        check(c.count("#postGameRes tr") == 3 and c.count("#postGameRes th") == 6,
              "a header (Rank + the 5 endStats after sid) and a row per player: %d rows" % c.count("#postGameRes tr"))
        res = c.textof("#postGameRes") or ""
        check(res.startswith("RankNameScoreKillsDeathsReward") and ("1.%s5001023" % names[0]) in res and "2.rival100351" in res,
              "the ranks and values: %r" % res)
        check(c.textof(".krGivPost") == "500 KR", "the giveaway's KR: %r" % c.textof(".krGivPost"))
        feed = c.textof("#feedHolder") or c.textof("body") or ""
        check(re.search(r"ends in 1h 5\dm|ends in 2h ", feed) is not None, "its time is when it ends: %r" % feed[:300])
        check(c.count(".krGivFol .follBtnGiv") == 1 and "Follow and Like to Enter" in (c.textof(".krGivFol") or ""),
              "Follow and Like to Enter")
        c.shot("feed")
    finally:
        c.close()


def png(w, h, pad=0):
    raw = b"".join(b"\0" + b"\x80\x40\x20\xff" * w for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    text = chunk(b"tEXt", b"c\0" + b"x" * pad) if pad else b""
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + text +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def png_pixel(path, x, y):
    """(r, g, b) at (x, y) of an 8-bit RGB / RGBA PNG (the client's shot)"""
    with open(path, "rb") as f:
        data = f.read()
    pos, idat, w, h, ctype = 8, b"", 0, 0, 6
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw, stride, prev, row = zlib.decompress(idat), w * bpp, bytearray(w * bpp), None
    for j in range(y + 1):
        line = raw[j * (stride + 1):(j + 1) * (stride + 1)]
        ft, cur = line[0], bytearray(line[1:])
        for i in range(stride):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 1:
                cur[i] = (cur[i] + a) & 255
            elif ft == 2:
                cur[i] = (cur[i] + b) & 255
            elif ft == 3:
                cur[i] = (cur[i] + (a + b) // 2) & 255
            elif ft == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev, row = cur, cur
    return tuple(row[x * bpp:x * bpp + 3])


def near(rgb, want, tol=14):
    return rgb is not None and all(abs(a - b) <= tol for a, b in zip(rgb, want))


def webp_solid(w, h, rgba):
    """a lossless WebP of one colour: every prefix code has one symbol, so the pixels take no bits"""
    bits, n = 0, 0

    def put(v, k):
        nonlocal bits, n
        bits |= (v & ((1 << k) - 1)) << n
        n += k
    put(0x2f, 8)
    put(w - 1, 14)
    put(h - 1, 14)
    put(1 if rgba[3] != 255 else 0, 1)
    put(0, 3)  # version 0, then no transform, no colour cache, no meta prefix codes
    put(0, 3)
    for sym in (rgba[1], rgba[0], rgba[2], rgba[3]):  # green, red, blue, alpha: simple codes of one 8-bit symbol
        put(0b101, 3)
        put(sym, 8)
    put(0b001, 3)  # distance: a simple code of one 1-bit symbol (0)
    put(0, 1)
    data = bits.to_bytes((n + 7) // 8, "little")
    chunk = b"VP8L" + struct.pack("<I", len(data)) + data + (b"\0" if len(data) % 2 else b"")
    return b"RIFF" + struct.pack("<I", 4 + len(chunk)) + b"WEBP" + chunk


PFP_RGB = (32, 192, 64)


def pick(path):
    with open(PICK, "w") as f:
        f.write(path + "\n")


def mod_window(port, ids, tokens, names, stamp):
    """the menu's Upload Mod (window 19): v10.0.7's mods_pub (game.deob.js:220943-221050)"""
    A = ids[0]
    zpath, tpath = os.path.join(WORK, "pack.zip"), os.path.join(WORK, "thumb.png")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_STORED) as z:  # stored: the size line shows a real MB figure
        z.writestr("textures/ak_0.png", png(2, 2, 1500000))
    with open(tpath, "wb") as f:
        f.write(png(4, 4, 2000))
    zmb = "%g" % (round(os.path.getsize(zpath) / 1e6 * 100) / 100)
    tkb = "%g" % (round(os.path.getsize(tpath) / 1e3 * 100) / 100)
    c = Client(port, names[0], tokens[0], "", "modwin", hub=False)
    try:
        check(c.has("menuHolder", 90) or c.has("uiBase", 30), "the game's menu")
        opened = False
        for _ in range(10):  # signed in once the account has loaded; before that it is the static "log in" text
            c.ui("showWindow(19)")
            opened = c.has("modPubBody", 4)
            if opened:
                break
        check(opened, "Upload Mod opens v10's form (#modPubBody)")
        check((c.textof("#modThumbSize") or "") == "0 / 250 KB" and (c.textof("#modZipSize") or "") == "0 / 25 MB",
              "the limits: 0 / 250 KB, 0 / 25 MB (no premium): %r %r" % (c.textof("#modThumbSize"), c.textof("#modZipSize")))
        body = c.textof("#modPubBody") or ""
        check(body.startswith("Thumbnail (PNG)") and "Mod File (.zip)" in body and "Select Thumbnail" in body and
              "Select Mod Zip" in body and body.endswith("Publish"), "v10's labels and links: %r" % body)
        check(c.count("#modPubBody [onclick='windows[18].submit()']") == 1 and c.count("input[id=modNameNew][placeholder='Mod Name']") == 1,
              "Publish is windows[18].submit(), the name's placeholder Mod Name")
        c.ui("windows[18].submit()")
        check(c.text_has("#modUpResp", "Mod name is required", 5), "no name: %r" % c.textof("#modUpResp"))
        c.type("modNameNew", "Window Pack " + stamp)
        c.ui("windows[18].submit()")
        check(c.text_has("#modUpResp", "Select a .zip mod file", 5), "no .zip: %r" % c.textof("#modUpResp"))
        pick(zpath)
        c.ui("selectFile('modZip')")
        check(wait(lambda: (c.textof("#modZipSize") or "") == zmb + " / 25 MB", 5) and c.count("#modZipH[title='pack.zip']") == 1,
              "onFileChange: the .zip's size line %s / 25 MB and its name in #modZipH's title: %r" % (zmb, c.textof("#modZipSize")))
        pick(tpath)
        c.ui("selectFile('modThumb')")
        check(wait(lambda: (c.textof("#modThumbSize") or "") == tkb + " / 250 KB", 5) and c.count("#modThumbH[title='thumb.png']") == 1,
              "onFileChange: the thumbnail's size line %s / 250 KB and its title: %r" % (tkb, c.textof("#modThumbSize")))
        check((c.value("modNameNew") or "") == "Window Pack " + stamp, "picking a file keeps the typed name")
        # the preview is the picked file as a data: URL (v10: FileReader.readAsDataURL), drawn in the 80 x 80 box
        check(c.count("#modThumbPrev[src^='data:image/png;base64,iVBORw0KGgo']") == 1, "the preview's src is the picked PNG's data: URL")
        rgb = None
        for _ in range(10):
            rgb = c.pixel_at_centre("modThumbPrev", "upload-mod")
            if near(rgb, (128, 64, 32)):
                break
            time.sleep(0.5)
        check(near(rgb, (128, 64, 32)), "the preview shows the picked thumbnail (its colour, not the dark box): %r" % (rgb,))
        c.ui("windows[18].submit()")
        check(wait(lambda: one("SELECT count(*) FROM published_mods WHERE mod_name = ? AND creator_id = ?", "Window Pack " + stamp, A) == 1, 60),
              "Publish: the mod is in published_mods")
        check(c.text_has("#popupContent", "Mod Published!", 10), "v10's SUCCESS popup: Mod Published!")
        check(c.count("#modPubBody") == 0, "the window closed")
    finally:
        c.close()


async def friends(port, ids, tokens, names, bob_sock):
    A, B = ids
    # bob's approved profile picture (avatar.c): served at <Box Office>/user-assets/p<id>/profile.png; alice has none (404)
    import base64
    q("INSERT INTO account_avatars (account_id, status, sha256, image, bytes, width, height) VALUES (?, 'approved', 'e2e', ?, 0, 16, 16)",
      B, base64.b64encode(webp_solid(16, 16, PFP_RGB + (255,))).decode())
    pfp = lambda i: "http://127.0.0.1:%d/user-assets/p%d/profile.png?v=" % (port, i)  # noqa: E731
    c = Client(port, names[0], tokens[0], "p=feed", "friends")
    try:
        check(c.has("chatBubHold", 60) and wait(lambda: c.count("#msgCnt%d" % B) == 1, 20), "the chat sidebar shows the friend's bubble")
        check(c.count("#chatBubHold .chatBlobI") == 1, "one bubble: the one friend")
        # not a subscriber: src="" and the onerror's default picture (hub-10.0.7.js:2340)
        check(wait(lambda: c.count("#chatBubHold .chatBlobImg[src='./img/player_pfp.png']") == 1, 10),
              "the bubble of a friend without a subscription: src='' falls back to player_pfp.png (onerror)")
        r = await bob_sock.call("chf", "ict", A, "hi from bob")
        check(r and r[1] is True, "bob sent a message")
        check(wait(lambda: c.textof("msgCnt%d" % B) == "1", 10), "the message counts on bob's bubble (msg push)")
        # the chat's messages: every sender's picture (hub-10.0.7.js:2311), its default when there is none
        c.ui("startChat(%d,0,'%s','',1)" % (B, names[1]))
        # no sm: v10 shows Offline and, gid undefined != "", Join (hub-10.0.7.js:2249-2255)
        check(wait(lambda: (c.textof("#chatWithHead") or "").endswith("Offlinelaunch"), 10) and c.count("#chatWithHead .frndQickJoin") == 1,
              "startChat without a status: Offline, and Join: %r" % c.textof("#chatWithHead"))
        check(wait(lambda: c.count(".chatItPFP[src^='%s']" % pfp(B)) == 1, 15), "bob's message shows p%d/profile.png" % B)
        rgb = None
        for _ in range(20):
            rgb = c.pixel_at_centre(".chatItPFP", "chat-pfp")
            if near(rgb, PFP_RGB):
                break
            time.sleep(0.5)
        check(near(rgb, PFP_RGB), "and draws bob's approved picture (the WebP from Box Office): %r" % (rgb,))
        c.type("chatBoxInput", "heybob")
        c.ui("sendChat()")
        check(wait(lambda: c.count(".chatItPFP[src='./img/player_pfp.png']") == 1, 20),
              "alice's own message: p%d/profile.png answers 404 and the onerror shows player_pfp.png" % A)
        c.ui("closeChat()")
        c.ui("showFriends()")
        check(c.text_has("#genericPop", names[1]) and "Online" in (c.textof("#genericPop") or ""), "Friends lists bob, online")
        check(c.text_has("#genericPop", "Your Friends") and c.text_has("#genericPop", "1 Total"), "v10's header: Your Friends, 1 Total")
        check(wait(lambda: c.count("#genericPop .folPfp[src='./img/player_pfp.png']") == 1, 10) and
              c.count("#genericPop .buttonR[onclick='removeFriend(0)']") == 1 and c.text_has("#genericPop", "Remove Friend"),
              "a friend's row: the default picture (no subscription) and Remove Friend")
        c.shot("friends")
        c.ui("removeFriend(0)")
        check(wait(lambda: not q("SELECT 1 FROM follows WHERE follower_id = ? AND followee_id = ?", A, B)), "Remove unfollowed bob")
        check(wait(lambda: c.count("#chatBubHold .chatBlobImg[onclick]") == 0, 10), "and his bubble left the sidebar")
        # the feed's own Followers / Following (viewFollowers(1) / viewFollowing(1), hub-10.0.7.js:5353-5406)
        c.ui("clearPops();viewFollowers(1)")
        check(c.text_has("#genericPop", "Your followers") and c.text_has("#genericPop", names[1]) and
              c.count("#genericPop .reqFollB[onclick^='instaFollo']") == 1, "Your followers: bob, with Follow: %r" % c.textof("#genericPop"))
        # a follower's picture: p<friendid>/profile.png always (hub-10.0.7.js:5366)
        check(c.count("#genericPop .folPfp[src^='%s']" % pfp(B)) == 1, "bob's follower row loads p%d/profile.png" % B)
        rgb = None
        for _ in range(20):
            rgb = c.pixel_at_centre("#genericPop .folPfp", "follower-pfp")
            if near(rgb, PFP_RGB):
                break
            time.sleep(0.5)
        check(near(rgb, PFP_RGB), "and draws his picture: %r" % (rgb,))
        q("INSERT INTO follows (follower_id, followee_id) VALUES (?, ?)", A, B)
        c.ui("clearPops();viewFollowing(1)")
        check(c.text_has("#genericPop", "You follow") and c.text_has("#genericPop", names[1]) and
              c.count("#genericPop .reqFollB.buttonR[title='Unfollow']") == 1, "You follow: bob, with Unfollow: %r" % c.textof("#genericPop"))
        c.cmd("clicksel 0 #genericPop .reqFollB.buttonR")
        check(wait(lambda: not q("SELECT 1 FROM follows WHERE follower_id = ? AND followee_id = ?", A, B)) and
              wait(lambda: c.count("#genericPop .friendBHold") == 0, 5), "Unfollow (deleteFollower): bob unfollowed, his row gone")
        # favoriteM(kind, this, id) on the Mods page: io.send("fvM", 1, id, 1), the star turns sel (hub-10.0.7.js:6616-6622)
        mod_id = one("SELECT mod_id FROM published_mods WHERE creator_id = ? ORDER BY mod_id LIMIT 1", B)
        c.ui("clearPops();updateWindow('mods')")
        time.sleep(1.5)
        check(wait(lambda: c.textof("title") == "Mods - Krunker Hub", 10), "document.title: Mods - Krunker Hub (hub-10.0.7.js:3130): %r" % c.textof("title"))
        c.ui("windows[1].switchTab(2)")  # New: every mod, newest first
        star = "#modsHolder .creationAction[onclick='favoriteM(1, this, %d);']" % mod_id
        if wait(lambda: c.count(star) == 1, 20):
            c.cmd("clicksel 0 " + star)
            check(wait(lambda: q("SELECT 1 FROM creation_favs WHERE account_id = ? AND kind = 1 AND creation_id = ?", A, mod_id), 10)
                  and c.count(star.replace(".creationAction", ".creationAction.sel")) == 1, "favoriteM: the mod is a favourite, its star sel")
        else:
            check(False, "the Mods page lists bob's mod with its star: %r" % (c.textof("#modsHolder") or "")[:400])
    finally:
        c.close()


def main():
    (port,) = free_ports(1)
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")  # its own config: never the developer's config/config.json
    with open(cfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "hubc-e2e-%d" % os.getpid(),
                             "ranked": {"season": 9}},
                   "uploads": {"dir": os.path.join(WORK, "uploads"), "mod_cooldown_ms": 0},
                   "accounts": {"signup_rewards": {"enabled": False}, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0,
                                "password_iterations": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        for _ in range(200):
            try:
                with open(os.path.join(WORK, "brain.log")) as f:
                    if re.search(r"listening on http://127\.0\.0\.1:%d" % port, f.read()):
                        break
            except OSError:
                pass
            time.sleep(0.1)
        stamp = str(int(time.time()) % 100000)
        names = ["hcral%s" % stamp, "hcrbo%s" % stamp]

        async def setup():
            socks, ids, tokens = [], [], []
            for n in names:
                s, i, t = await register(port, n)
                socks.append(s)
                ids.append(i)
                tokens.append(t)
            q("UPDATE players SET player_score = 2000000, player_funds = 1000 WHERE player_id = ?", ids[0])
            map_name = "Hubc_" + stamp
            doc = {"name": map_name, "modes": [0, 1], "spawns": [[0, 1, 0], [4, 1, 0]],
                   "objects": [{"p": [0, 0, 0], "s": [10, 1, 10]}, {"i": PREFAB_LEADERBOARD, "p": [0, 5, 0], "s": [4, 2, 1]}]}
            import base64
            thumb = "data:image/png;base64," + base64.b64encode(png(64, 32)).decode()  # the editor's capture: (128, 64, 32)
            r = await socks[0].call("pubMR", "uploadM", json.dumps(doc), thumb, "", "", 1)
            check(r is not None, "the creator published a map with a LEADERBOARD: %r" % (r,))
            return socks, ids, tokens, map_name

        socks, ids, tokens, map_name = asyncio.run(setup())
        map_id = one("SELECT map_id FROM published_maps WHERE map_name = ?", map_name)
        for aid, score in ((ids[0], 300), (ids[1], 500)):
            q("INSERT INTO match_results (match_id, account_id, map, score) VALUES (?, ?, ?, ?)", "m-%d" % aid, aid, map_name, score)
        q("INSERT INTO ks_storage (account_id, map_id, skey, svalue, scope) VALUES (?, ?, 'coins', '5', 0), (?, ?, 'gems', '2', 0), "
          "(?, ?, 'gems', '9', 0)", ids[1], map_id, ids[1], map_id, ids[0], map_id)
        q("INSERT INTO published_mods (mod_id, mod_name, creator_id, mod_url, mod_info) VALUES (?, ?, ?, 'ua', '{}'), (?, ?, ?, 'ua', '{}')",
          1000000900 + int(stamp) % 50, "Hub Pack " + stamp, ids[0], 1000000950 + int(stamp) % 50, "Not Mine " + stamp, ids[1])
        map_page(port, ids, tokens, names, map_name, map_id, stamp)
        staff_profile(port, ids, tokens, names)
        trade_history(port, ids, tokens, names)
        market_cards(port, ids, tokens, names)
        mod_window(port, ids, tokens, names, stamp)

        q("INSERT INTO follows (follower_id, followee_id) VALUES (?, ?), (?, ?)", ids[0], ids[1], ids[1], ids[0])

        async def rest():
            s = await Sock(port).open()
            f = await s.call("a", "a", 1, [names[1], None, tokens[1]])
            check(f and f[1] is None, "bob's socket stays signed in (online)")
            await friends(port, ids, tokens, names, s)
            await s.close()

        asyncio.run(rest())
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        brain.terminate()
        try:
            brain.wait(10)
        except subprocess.TimeoutExpired:
            brain.kill()
        log.close()
    print("hub creator e2e (native): " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

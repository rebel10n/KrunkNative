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

    def __init__(self, port, name, token, query, tag):
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
                    "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port})
        self.out = open(self.log, "w")
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT, "--hub", query], stdout=self.out, stderr=subprocess.STDOUT,
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
        return path

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
        check(wait(lambda: one("SELECT map_funds FROM published_maps WHERE map_id = ?", map_id) == 300) and
              one("SELECT player_funds FROM players WHERE player_id = ?", A) == 700, "Deposit: 300 KR into the map")
        check(c.text_has("#genericPop", "Success!"), "mfr: v10's SUCCESS popup")
        c.ui("clearPops();openEdit('%s', %d, 300)" % (map_name, map_id))
        c.type("mapFundsInp", "100")
        c.ui("chngMapFunds(%d, 1)" % map_id)
        check(wait(lambda: one("SELECT map_funds FROM published_maps WHERE map_id = ?", map_id) == 200) and
              one("SELECT player_funds FROM players WHERE player_id = ?", A) == 800, "Withdraw: 100 KR back")
        check(c.text_has("#profileKR", "800"), "the KR in the nav bar follows")

        upd = one("SELECT map_updatecounter FROM published_maps WHERE map_id = ?", map_id)
        c.ui("clearPops();openEdit('%s', %d, 200)" % (map_name, map_id))
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
    c = Client(port, names[0], tokens[0], "p=market&q=trades", "trades")
    try:
        check(c.has("tn_his", 60), "the Trading tab")
        c.ui("updateTrades('his')")
        check(wait(lambda: c.count("#o_his .offerHolder[id]") == 30, 15), "History shows the first 30: %d" % c.count("#o_his .offerHolder[id]"))
        check(c.count("#o_his .loadMoreHis") == 1, "and Load More")
        c.shot("history")
        c.ui("loadMoreTradeHistory()")
        check(wait(lambda: c.count("#o_his .offerHolder[id]") == 35, 10), "Load More: all 35")
        check(c.count("#o_his .loadMoreHis") == 0 and c.has("o_his", 5), "no more pages; the History tab stays open")
        # the settings' image collection (hub-10.0.7.js:6335-6440; image_collection.c)
        c.ui("clearPops();imagePopup('customScope')")
        check(c.has("imagesTabLayout"), "imagePopup opens the image collection")
        c.ui("imageNewPopup()")
        check(c.has("newCustName"), "Add an image asks for a name and a link")
        c.type("newCustName", "Blue Scope")
        c.type("newCustImg", "https://example.com/scope.png")
        c.ui("addNewImage()")
        check(c.text_has(".kn-image-grid", "Blue Scope", 10), "the new image is in the collection")
    finally:
        c.close()


async def friends(port, ids, tokens, names, bob_sock):
    A, B = ids
    c = Client(port, names[0], tokens[0], "p=feed", "friends")
    try:
        check(c.has("chatBubHold", 60) and wait(lambda: c.count("#msgCnt%d" % B) == 1, 20), "the chat sidebar shows the friend's bubble")
        check(c.count("#chatBubHold .chatBlobI") == 1, "one bubble: the one friend")
        r = await bob_sock.call("chf", "ict", A, "hi from bob")
        check(r and r[1] is True, "bob sent a message")
        check(wait(lambda: c.textof("msgCnt%d" % B) == "1", 10), "the message counts on bob's bubble (msg push)")
        c.ui("showFriends()")
        check(c.text_has("#genericPop", names[1]) and "Online" in (c.textof("#genericPop") or ""), "Friends lists bob, online")
        c.shot("friends")
        c.ui("removeFriend(0)")
        check(wait(lambda: not q("SELECT 1 FROM follows WHERE follower_id = ? AND followee_id = ?", A, B)), "Remove unfollowed bob")
        check(wait(lambda: c.count("#chatBubHold .chatBlobImg[onclick]") == 0, 10), "and his bubble left the sidebar")
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
            r = await socks[0].call("pubMR", "uploadM", json.dumps(doc), None, "", "", 1)
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

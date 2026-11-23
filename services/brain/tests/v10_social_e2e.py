#!/usr/bin/env python3
"""The v10 Social hub end to end through the native client, headless (doc/v10/social.md).

  python3 -I services/brain/tests/v10_social_e2e.py <native build dir> <repo root>

kn_brain on sqlite + two headless clients on the v10 page, driven by KRUNKNATIVE_UI_CMDS: clicks
the page's own elements (the Social menu item, the sidebar tabs, the Search tab, New post, the
friends list, the conversation, a profile, the clan view). Outcomes are checked in the database.
77 = skip (no client_bin / kn_brain / websockets).
"""
import asyncio, json, os, re, shutil, sqlite3, subprocess, sys, tempfile, threading, time
try:
    import websockets
except ImportError:
    print("python websockets missing: skip"); sys.exit(77)

BUILD = os.path.abspath(sys.argv[1])
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.getcwd()
BRAIN, CLIENT = os.path.join(BUILD, "services", "brain", "kn_brain"), os.path.join(BUILD, "client_bin")
if not (os.path.exists(BRAIN) and os.path.exists(CLIENT)):
    print("needs client_bin and kn_brain in %s: skip" % BUILD); sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-v10-social.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
PORT = free_ports(1)[0]

def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond: FAILS.append(what)

def q(sql, *args):
    con = sqlite3.connect(DB, timeout=30)
    try:
        rows = con.execute(sql, args).fetchall(); con.commit(); return rows
    finally:
        con.close()

async def register(name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT) as ws:
        await ws.send(json.dumps(["a", 0, [name, "social_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]

def peer_session(name, token, follow, hold_s):
    """A third player on a plain social socket (a thread): signs in, follows `follow` back, stays
    online hold_s seconds (presence), then leaves."""
    async def run():
        async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT) as ws:
            await ws.send(json.dumps(["a", 1, [name, None, token]]))
            await ws.send(json.dumps(["gapi", 1, "POST", "/players/%s/follow" % follow, {"follow": True}]))
            end = time.time() + hold_s
            while time.time() < end:
                try:
                    f = json.loads(await asyncio.wait_for(ws.recv(), 0.5))
                    if f and f[0] == "pi":
                        await ws.send(json.dumps(["po"]))
                except asyncio.TimeoutError:
                    pass
    t = threading.Thread(target=lambda: asyncio.run(run()), daemon=True)
    t.start()
    return t


def dm_session(name, token, to, text, hold_s=6.0):
    """A second session of `name` (a thread) that sends one DM to account `to`, then stays online hold_s."""
    async def run():
        async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT) as ws:
            await ws.send(json.dumps(["a", 1, [name, None, token]]))
            await ws.send(json.dumps(["eb", {"type": "msg:send", "to": to, "text": text}]))
            end = time.time() + hold_s
            while time.time() < end:
                try:
                    f = json.loads(await asyncio.wait_for(ws.recv(), 0.5))
                    if f and f[0] == "pi":
                        await ws.send(json.dumps(["po"]))
                except asyncio.TimeoutError:
                    pass
    t = threading.Thread(target=lambda: asyncio.run(run()), daemon=True)
    t.start()
    return t


class Client:
    def __init__(self, name, token, tag):
        self.tag, self.cmds = tag, os.path.join(WORK, tag + ".cmds")
        self.log = os.path.join(WORK, tag + ".log")
        cfg = os.path.join(WORK, tag + "-cfg")
        os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
        open(os.path.join(cfg, "krunknative", "krunker_username.json"), "w").write(name)
        open(os.path.join(cfg, "krunknative", "krunker_token.json"), "w").write(token)
        open(self.cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                    "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UI_CMDS": self.cmds, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                    "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % PORT})
        self.out = open(self.log, "w")
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT, "--offline"], stdout=self.out, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, env=env, cwd=ROOT, start_new_session=True)

    def text(self):
        try:
            return open(self.log, errors="replace").read()
        except OSError:
            return ""

    def cmd(self, line): open(self.cmds, "a").write(line + "\n")
    def mark(self): return len(self.text())

    def wait_line(self, needle, mark, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            if needle in self.text()[mark:]: return True
            time.sleep(0.15)
        return False

    def has(self, elem_id, timeout=15.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.mark()
            self.cmd("rect " + elem_id)
            if self.wait_line("#%s rect" % elem_id, m, 2.0): return True
            self.cmd("at 5 5")
            time.sleep(0.2)
        return False

    def click(self, elem_id):
        self.cmd("clickid " + elem_id); time.sleep(0.5)

    def textof(self, elem_id, timeout=4.0):
        m = self.mark()
        self.cmd("textof " + elem_id)
        if not self.wait_line("[ui_cmds] textof %s = '" % elem_id, m, timeout): return None
        found = re.search(r"\[ui_cmds\] textof %s = '(.*)'" % re.escape(elem_id), self.text()[m:])
        return found.group(1) if found else None

    def value(self, elem_id, timeout=4.0):
        m = self.mark()
        self.cmd("value " + elem_id)
        if not self.wait_line("value %s = '" % elem_id, m, timeout): return None
        found = re.search(r"value %s = '(.*)'" % re.escape(elem_id), self.text()[m:])
        return found.group(1) if found else None

    def count(self, sel, timeout=4.0):
        m = self.mark()
        self.cmd("count " + sel)
        if not self.wait_line("count %s = " % sel, m, timeout): return -1
        found = re.search(r"count %s = (\d+)" % re.escape(sel), self.text()[m:])
        return int(found.group(1)) if found else -1

    def chat_has(self, needle, timeout=10.0):
        """#chatList's text (textof) contains needle: the in-game chat lines (v10 _49706 / _49707)."""
        end = time.time() + timeout
        while time.time() < end:
            m = self.mark()
            self.cmd("textof chatList")
            if self.wait_line("[ui_cmds] textof chatList = '", m, 2.0):
                found = re.search(r"\[ui_cmds\] textof chatList = '(.*)'", self.text()[m:])
                if found and needle in found.group(1): return True
            time.sleep(0.3)
        return False

    def ui(self, calls):
        self.cmd("ui " + calls); time.sleep(0.6)

    def type(self, elem_id, chars):
        self.click(elem_id); self.cmd("text " + chars); time.sleep(0.5)

    def shot(self, name):
        path = os.path.join(WORK, "%s-%s.png" % (self.tag, name))
        self.cmd("shot " + path); time.sleep(0.8); return path

    def close(self):
        self.cmd("quit")
        try: self.proc.wait(15)
        except subprocess.TimeoutExpired:
            self.proc.kill(); self.proc.wait(5)
        self.out.close()

def main():
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
               "brain": {"host": "127.0.0.1", "port": PORT, "federation": False, "internal_secret": "v10social-%d" % os.getpid()},
               "accounts": {"signup_rewards": {"enabled": False}, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0, "password_iterations": 1000,
                            "news": ["<b>KN news</b>"], "giveaway_check_ms": 500,
                            "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, open(cfg, "w"))
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    clients = []
    try:
        for _ in range(200):
            try:
                if re.search(r"listening on http://127\.0\.0\.1:%d" % PORT, open(os.path.join(WORK, "brain.log")).read()): break
            except OSError: pass
            time.sleep(0.1)
        stamp = str(int(time.time()) % 100000)
        names = ["v10sa%s" % stamp, "v10sb%s" % stamp, "v10sc%s" % stamp]
        ids, tokens = [], []
        for n in names:
            i, t = asyncio.run(register(n)); ids.append(i); tokens.append(t)
        for aid in ids:
            q("UPDATE players SET player_score = 2000000, player_funds = 5000, player_premium = 1 WHERE player_id = ?", aid)
        a, b, c = ids
        q("UPDATE players SET player_moderator = 1 WHERE player_id = ?", a)  # alice sees the Reports tab
        cid = q("INSERT INTO clans (clan_name, clan_playerid, clan_score, clan_discord) VALUES ('V10C', ?, 777777, 'abc') RETURNING clan_id", a)[0][0]
        q("INSERT INTO clan_members (account_id, clan_id, rank, score) VALUES (?, ?, 3, 5000)", a, cid)
        q("INSERT INTO follows (follower_id, followee_id) VALUES (?, ?), (?, ?)", a, b, b, a)
        q("INSERT INTO chat_messages (sender_id, receiver_id, body) VALUES (?, ?, 'see you in game')", b, a)
        # carol's ranked match (the profile's Match History, P_PROFILE_MATCHES): Alpha 3 - 1, carol on Bravo with alice
        q("INSERT INTO mm_ranked_matches (match_id, queue_id, region, brain_id, status, result, finished_at, season, map_index, mode_index)"
          " VALUES ('v10sm1', 'q', 'eu', 'b', 'done', ?, strftime('%Y-%m-%d %H:%M:%f', 'now'), 9, 2, 0)",
          json.dumps({"teamScores": [3, 1], "winner": 0, "players": [{"accountId": c, "objective": 7}]}))
        q("INSERT INTO mm_ranked_match_players (match_id, account_id, username, team, rating_key, rating_before, rating_after, kills, deaths, assists,"
          " score, damage_done, victory) VALUES ('v10sm1', ?, ?, 0, 'v10', 1000, 1030, 25, 10, 3, 3300, 4000, 1),"
          " ('v10sm1', ?, ?, 1, 'v10', 1000, 982, 12, 20, 2, 1800, 2100, 0)", a, names[0], c, names[2])
        A, B = Client(names[0], tokens[0], "alice"), Client(names[1], tokens[1], "bob")
        clients = [A, B]
        check(A.has("menuBtnSocial"), "the v10 main page is up (#menuBtnSocial)")
        # v10 chat lines while on the menu: the unread summary of sign-in (bob's "see you in game"),
        # then a live DM from bob ([DM from <name>]: text, the _49706 listener; the page is the menu)
        check(A.chat_has("[DM] 1 new message from %s" % names[1], 15), "sign-in: \"[DM] 1 new message from <bob>\" in #chatList")
        dm_session(names[1], tokens[1], a, "dm while on the menu")
        check(A.chat_has("[DM from %s]: dm while on the menu" % names[1], 15), "a DM on the menu: \"[DM from <bob>]: text\" in #chatList")
        A.click("menuBtnSocial")
        check(A.has("v10sTab1"), "the Social hub popup opened (sidebar tabs)")
        check(A.has("v10sFeed0"), "the Social tab's feeds are mounted")
        A.shot("hub-social")
        A.click("v10sTab1")  # Friends
        check(A.has("v10sFriend%d" % b), "the Friends tab lists the friend (event bus friends:list)")
        A.click("v10sFriend%d" % b)
        check(A.has("v10sMsgInput"), "the conversation opened")
        A.type("v10sMsgInput", "hello from the e2e")
        A.click("v10sSend")
        sent, end = [], time.time() + 10.0  # polled: the row lands later than a second under load
        while time.time() < end:
            sent = q("SELECT body FROM chat_messages WHERE sender_id = ? AND receiver_id = ? ORDER BY id DESC", a, b)
            if sent and sent[0][0] == "hello from the e2e":
                break
            time.sleep(0.2)
        check(sent and sent[0][0] == "hello from the e2e", "Send reached kn_brain (chat_messages)")
        A.shot("hub-friends")
        A.click("v10sTab2")
        A.type("v10sSearchQ", names[1][:6])
        time.sleep(1.5)
        check(A.has("v10sResult0"), "the Search tab shows a result (gapi /players/search)")
        A.shot("hub-search")
        A.click("v10sResult0")
        check(A.has("v10sProfile"), "clicking a result opens the profile view")
        A.shot("profile")
        A.ui("v10s('back')")
        # friend request: Follow on carol's profile; carol follows back (accept) -> friends + presence
        A.ui("openPlayerProfile('%s')" % names[2])
        check(A.has("v10sProfileFollow"), "carol's profile shows Follow")
        A.click("v10sProfileFollow")
        time.sleep(1.0)
        check(q("SELECT 1 FROM follows WHERE follower_id = ? AND followee_id = ?", a, c), "Follow on the profile: the follows row (friend request)")
        # Report carol: v10's Report view sends the reason's index (0 = Hacking); the brain files it (rptr)
        A.click("v10sProfileReport")
        check(A.has("v10sReportSend"), "Report opens the report view")
        A.type("v10sReportText", "carol is using an aimbot in the lobby")
        A.click("v10sReportSend")
        time.sleep(1.2)
        check(q("SELECT reason, details FROM reports WHERE reporter_id = ? AND target_id = ?", a, c)
              == [("Hacking", "carol is using an aimbot in the lobby")], "Report: filed as v10's reason Hacking (rptr true)")
        A.ui("v10s('back')")  # the report view's Done: the profile again
        A.click("v10sPtab7")  # Reports (moderators): gapi GET /players/<name>/reports
        check(A.has("v10sReport0"), "the Reports tab lists the report")
        rp = A.textof("v10sReport0") or ""
        check(rp.startswith("from %s" % names[0]) and "Hacking" in rp and "carol is using an aimbot" in rp
              and re.search(r"(just now|\d+m ago)", rp) is not None,
              "a report row: \"from <alice>\", its age, the reason, the text (deob-10.0.7.js:112018): %r" % rp)
        A.shot("profile-reports")
        # the staff actions (deob-10.0.7.js:111865-111924, 112551-112585): alice moderates (canChatBan, canGlobalFlag),
        # no developer (no Verify)
        check((A.textof("v10sProfileChatBan") or "").endswith(" Chat Ban"), "a moderator sees Chat Ban (getChatBan: none)")
        check(A.count("#v10sProfileVerify") == 0, "Verify is a developer's only (canVerify)")
        A.click("v10sProfileChatBan")
        time.sleep(1.0)
        check(q("SELECT count(*) FROM feature_bans WHERE account_id = ? AND feature = 'chat' AND revoked_at IS NULL", c)[0][0] == 1
              and (A.textof("v10sProfileChatBan") or "").endswith("Chat Unban"),
              "Chat Ban: POST /moderation/chat bans carol's chat, the button reads Chat Unban")
        A.click("v10sProfileChatBan")
        time.sleep(1.0)
        check(q("SELECT count(*) FROM feature_bans WHERE account_id = ? AND feature = 'chat' AND revoked_at IS NULL", c)[0][0] == 0
              and (A.textof("v10sProfileChatBan") or "").endswith("Chat Ban"), "Chat Unban lifts it")
        A.click("v10sProfileFlag")
        check(A.has("v10sFlagConfirm", 6.0) and A.textof("v10sFlagText") == "Are you sure you want to flag %s as a Hacker?" % names[2]
              and A.textof("v10sFlagConfirm") == "Flag", "Flag asks first: \"Are you sure you want to flag <carol> as a Hacker?\", Flag")
        A.click("v10sFlagConfirm")
        time.sleep(1.0)
        check(q("SELECT player_hack FROM players WHERE player_id = ?", c)[0][0] == 1 and "BANNED" in (A.textof("v10sProfile") or "")
              and (A.textof("v10sProfileFlag") or "").endswith("Unflag"), "Flag: POST /moderation/flag sets player_hack, BANNED shows, Unflag")
        A.click("v10sProfileFlag")
        A.has("v10sFlagConfirm", 6.0)
        A.click("v10sFlagConfirm")
        time.sleep(1.0)
        check(q("SELECT player_hack FROM players WHERE player_id = ?", c)[0][0] == 0, "Unflag clears it")
        # Match History (deob-10.0.7.js:113205-113221): GET /players' match_history in v10's MatchHistory, carol's line
        A.click("v10sPtab6")
        time.sleep(0.8)
        mh = A.textof("v10sProfile") or ""
        check(A.count(".match-entry.loss") == 1 and "LOSS" in mh and "1 - 3" in mh and "KDA: 12/20/2" in mh and "Score: 1800" in mh
              and "-18 Rating" in mh, "Match History: carol's match, LOSS 1 - 3 (her team first), her KDA, score and rating: %r" % mh[-260:])
        check(A.count(".match-region") == 0 and A.count(".pagination .page-info") == 1 and "Page 1 of 1" in mh,
              "the profile's component: no region, pagination (data, hasPagination)")
        A.ui("v10sMhMatch(0)")
        rows = A.textof("v10sProfile") or ""
        check(A.count(".match-details .table-row.current-player") == 1 and rows.find(names[0]) < rows.rfind(names[2]),
              "a match opens into its scoreboard, score descending (alice 3300 above carol 1800)")
        A.ui("v10sMhSort(0,2)")  # Kills: a new column sorts ascending (deob-10.0.7.js:110416)
        rows = A.textof("v10sProfile") or ""
        check(rows.rfind(names[2]) < rows.rfind(names[0]) and "Kills \u2191" in rows,
              "Kills sorts ascending first (carol 12 above alice 25), its arrow up")
        A.shot("profile-match-history")
        A.ui("v10s('back')")
        # openPlayerProfile(name, {autoGift: 1, assetName}) (deob-10.0.7.js:112032-112038, 112331-112334): the gift view
        # opens over the profile with "I like your <mod> mod"
        A.ui("openPlayerProfile('%s',1,'Neon')" % names[2])
        check(A.has("v10sGiftWin", 10.0) and A.value("v10sGiftMsg") == "I like your Neon mod",
              "autoGift 1: the gift view's message is \"I like your Neon mod\": %r" % A.value("v10sGiftMsg"))
        A.ui("v10s('back')")
        A.ui("v10s('back')")
        A.ui("v10s('back')")
        A.click("v10sTab1")
        peer = peer_session(names[2], tokens[2], names[0], 8.0)
        check(A.has("v10sFriend%d" % c, 10.0), "carol follows back (accept): the friends list gains carol at once (pushed friends:list)")
        A.shot("friends-accepted")
        peer.join(15)
        # the profile window's Friends tab (showWindow(5), tab 1) lists the same friends
        A.ui("clearPops()")
        A.ui("showWindow(5)")
        A.ui("windows.4.switchTab(1)")
        check(A.has("friendList"), "showWindow(5) Friends tab: #friendList")
        m = A.mark()
        A.cmd("rect friendList")
        A.wait_line("#friendList rect", m, 3.0)
        rect = re.findall(r"#friendList rect [\d.]+ [\d.]+ [\d.]+ ([\d.]+)", A.text()[m:])
        check(rect and float(rect[-1]) > 30, "the Friends tab has friend rows (not the empty text)")
        A.shot("window5-friends")
        A.ui("closWind()")
        A.click("menuBtnSocial")
        A.ui("openClanView('V10C')")
        check(A.has("v10sClanView"), "openClanView renders the clan view")
        A.shot("clan")
        A.ui("v10s('back')")
        A.click("v10sTab0")
        A.click("v10sCompose")
        check(A.has("v10sComposeText"), "New post opens the compose popup")
        A.type("v10sComposeText", "hello from the native v10 hub")
        A.click("v10sComposePost")
        time.sleep(1.5)
        posted = q("SELECT body FROM feed_posts WHERE author_id = ? ORDER BY id DESC", a)
        check(posted and posted[0][0] == "hello from the native v10 hub", "Post: the post is in feed_posts")
        A.click("v10sCompose")
        A.type("v10sComposeText", "giveaway!")
        A.click("v10sGiveaway")
        A.type("giveaway-kr", "1000")
        A.click("v10sComposePost")
        time.sleep(1.5)
        g = q("SELECT type, funds FROM feed_posts WHERE author_id = ? ORDER BY id DESC", a)
        check(g and g[0][0] == 9 and g[0][1] == 1000, "giveaway post: type 9, funds 1000")
        check(q("SELECT player_funds FROM players WHERE player_id = ?", a)[0][0] == 3900, "the 10 % fee left the author 3,900 KR")
        gid = q("SELECT id FROM feed_posts WHERE author_id = ? AND type = 9 ORDER BY id DESC", a)[0][0]
        B.click("menuBtnSocial")
        B.click("v10sSubgiveaways")
        check(B.has("v10sPost%d" % gid), "a second client sees the giveaway in the Giveaways tab (its own gapi feed call)")
        B.click("v10sVote%d" % gid)
        time.sleep(1.0)
        check(q("SELECT count(*) FROM feed_likes WHERE post_id = ? AND account_id = ?", gid, b)[0][0] == 1,
              "Like on the giveaway reached kn_brain (the entry)")
        B.shot("hub-giveaways")
        # the Leaderboards popup is a new component on every openLeaderboards (deob-10.0.7.js:140313-140316): the
        # Players tab picked last time (Wins) is back on Level
        B.ui("clearPops()")
        B.ui("openLeaderboards('players')")
        first, third = ".tabs-container > .tab:first-child.tab-active", ".tabs-container > .tab:nth-child(3).tab-active"
        check(B.count(first) == 1, "openLeaderboards('players'): the Level tab is active")
        B.ui("knLbTab(2)")
        check(B.count(third) == 1 and B.count(first) == 0, "knLbTab(2): Wins is active")
        B.ui("clearPops()")
        B.ui("openLeaderboards('players')")
        check(B.count(first) == 1 and B.count(third) == 0, "reopened: Level is active again (the tabs reset)")
    except Exception as e:
        import traceback; traceback.print_exc(); check(False, "exception: %r" % e)
    finally:
        for c in clients: c.close()
        brain.terminate()
        try: brain.wait(10)
        except subprocess.TimeoutExpired: brain.kill()
        log.close()
    print("v10 social e2e: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"): shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)

main()

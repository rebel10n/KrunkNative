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
        cid = q("INSERT INTO clans (clan_name, clan_playerid, clan_score, clan_discord) VALUES ('V10C', ?, 777777, 'abc') RETURNING clan_id", a)[0][0]
        q("INSERT INTO clan_members (account_id, clan_id, rank, score) VALUES (?, ?, 3, 5000)", a, cid)
        q("INSERT INTO follows (follower_id, followee_id) VALUES (?, ?), (?, ?)", a, b, b, a)
        q("INSERT INTO chat_messages (sender_id, receiver_id, body) VALUES (?, ?, 'see you in game')", b, a)
        A, B = Client(names[0], tokens[0], "alice"), Client(names[1], tokens[1], "bob")
        clients = [A, B]
        check(A.has("menuBtnSocial"), "the v10 main page is up (#menuBtnSocial)")
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
        time.sleep(1.2)
        sent = q("SELECT body FROM chat_messages WHERE sender_id = ? AND receiver_id = ? ORDER BY id DESC", a, b)
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

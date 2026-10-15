#!/usr/bin/env python3
"""v10 Studios end to end through the native client, headless (client/src/ui/v10_studios.c,
services/brain/studios.c).

  python3 -I client/tests/v10_studios_e2e.py <native build dir> <repo root>

kn_brain on sqlite + two headless clients on the v10 page, driven by KRUNKNATIVE_UI_CMDS. The
owner opens Games > My Games > Studios Manage, creates a studio (the studio view opens), invites the
second player, assigns a published map, creates a currency, applies for monetization in the dialog
and looks at the analytics; the second player accepts the invite from the Studios window and sees
the studio; the owner deletes it through the confirm prompt. Outcomes are checked in the database.
77 = skip (no client_bin / kn_brain / websockets).
"""
import asyncio, json, os, re, shutil, sqlite3, subprocess, sys, tempfile, time
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
WORK = tempfile.mkdtemp(prefix="kn-v10-studios.", dir=os.path.expanduser("~/.cache"))
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


def wait_q(sql, args, pred, timeout=10.0):
    end = time.time() + timeout
    rows = []
    while time.time() < end:
        rows = q(sql, *args)
        if pred(rows): return rows
        time.sleep(0.3)
    return rows


async def register(name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT) as ws:
        await ws.send(json.dumps(["a", 0, [name, "studio_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


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
            time.sleep(0.2)
        return False

    def textof(self, sel):
        m = self.mark()
        self.cmd("textof " + sel)
        if not self.wait_line("[ui_cmds] textof %s = " % sel, m, 5.0): return ""
        found = re.findall(r"\[ui_cmds\] textof %s = '(.*)'" % re.escape(sel), self.text()[m:])
        return found[-1] if found else ""

    def value(self, elem_id):
        m = self.mark()
        self.cmd("value " + elem_id)
        if not self.wait_line("[ui_cmds] value %s = " % elem_id, m, 5.0): return None
        found = re.findall(r"\[ui_cmds\] value %s = '(.*)'" % re.escape(elem_id), self.text()[m:])
        return found[-1] if found else None

    def click(self, elem_id):
        self.cmd("clickid " + elem_id); time.sleep(0.5)

    def ui(self, calls):
        self.cmd("ui " + calls); time.sleep(0.6)

    def type(self, elem_id, chars):
        self.click(elem_id); self.cmd("focus " + elem_id); self.cmd("text " + chars); time.sleep(0.5)

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
               "brain": {"host": "127.0.0.1", "port": PORT, "federation": False, "internal_secret": "v10studios-%d" % os.getpid()},
               "accounts": {"signup_rewards": {"enabled": False}, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0,
                            "password_iterations": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, open(cfg, "w"))
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    clients = []
    try:
        for _ in range(200):
            try:
                if re.search(r"listening on http://127\.0\.0\.1:%d" % PORT, open(os.path.join(WORK, "brain.log")).read()): break
            except OSError: pass
            time.sleep(0.1)
        stamp = str(int(time.time()) % 100000)
        names = ["stuA%s" % stamp, "stuB%s" % stamp]
        (a, ta), (b, tb) = asyncio.run(register(names[0])), asyncio.run(register(names[1]))
        # the owner meets every monetization requirement once a game is assigned
        q("UPDATE accounts SET created_at = strftime('%Y-%m-%d %H:%M:%f', 'now', '-120 days'), twofa_secret = 'JBSWY3DPEHPK3PXP' WHERE id = ?", a)
        q("UPDATE players SET player_score = 50000000 WHERE player_id = ?", a)
        mid = q("INSERT INTO published_maps (map_name, creator_id) VALUES (?, ?) RETURNING map_id", "StuGame" + stamp, a)[0][0]
        studio = "E2E_" + stamp

        A = Client(names[0], ta, "owner")
        clients.append(A)
        check(A.has("menuBtnSocial", 40.0), "the v10 main page is up")
        A.ui("showWindow(10)")
        A.ui("windows.9.switchTab(5)")
        check(A.has("knStudiosOpen"), "Games > My Games shows the Studios row with Manage")
        check(A.textof("#studioInviteCount") == "0", "My Games: no pending invites")
        A.shot("my-games")
        A.click("knStudiosOpen")
        check(A.has("knStRoot"), "Manage opens the Studios window (openStudios)")
        check("You aren't in any studio yet." in A.textof("#knStRoot"), "the Studios window: no studio yet")
        A.type("knStCreateName", studio)
        A.click("knStCreate")
        check(A.has("knStStudio"), "Create opens the new studio's view")
        rows = wait_q("SELECT id, owner_id FROM studios WHERE name = ?", (studio,), lambda r: r)
        check(rows and rows[0][1] == a, "the studio is in kn_brain, owned by the creator")
        sid = rows[0][0] if rows else 0
        check(studio in A.textof("#knStStudio"), "the studio view's header shows its name")
        A.shot("studio-members")

        A.type("knStInviteName", names[1])
        A.click("knStInvite")
        inv = wait_q("SELECT id FROM studio_invites WHERE studio_id = ? AND account_id = ?", (sid, b), lambda r: r)
        check(bool(inv), "Invite: the invite is in kn_brain")
        time.sleep(0.8)
        t = A.textof("#knStStudio")
        check("Pending Invites" in t and names[1] in t and ("Invite sent to %s." % names[1]) in t, "the members tab lists the pending invite")

        A.click("knStTab1")
        A.ui("knStudios('input','map','%d')" % mid)
        A.click("knStAssignMap")
        check(bool(wait_q("SELECT 1 FROM studio_maps WHERE studio_id = ? AND map_id = ?", (sid, mid), lambda r: r)), "Assign Game: the map is in the studio")
        time.sleep(0.8)
        check("StuGame" + stamp in A.textof("#knStStudio"), "the Games tab lists the assigned game")

        A.click("knStTab3")
        A.type("knStCurName", "Gold\\sCoins")
        A.type("knStCurSymbol", "gld")
        check(A.value("knStCurSymbol") == "GLD", "the symbol field upper-cases what is typed")
        A.click("knStCurCreate")
        check(wait_q("SELECT name, symbol FROM studio_currencies WHERE studio_id = ?", (sid,), lambda r: r) == [("Gold Coins", "GLD")],
              "Create currency: Gold Coins / GLD in kn_brain")
        A.shot("studio-currencies")

        A.click("knStTab1")
        check(A.has("knStMonApply"), "the Games tab offers Apply for monetization")
        A.click("knStMonApply")
        check(A.has("knStMonDialog"), "Apply opens the monetization dialog")
        A.shot("monetization-dialog")
        A.type("knStMonDesc", "A\\scosmetic\\sshop\\sin\\sour\\sparkour\\smap.")
        # the games and the checkbox sit below the dialog's scrolled body: their handlers directly
        A.ui("knStudios('monmap',%d)" % mid)
        A.ui("knStudios('input','agree','1')")
        A.click("knStMonSubmit")
        rows = wait_q("SELECT status, description, map_ids FROM studio_monetization WHERE studio_id = ?", (sid,), lambda r: r)
        check(rows and rows[0][0] == "pending" and rows[0][1] == "A cosmetic shop in our parkour map." and json.loads(rows[0][2]) == [mid],
              "Submit: a pending application with the text and the example game")
        time.sleep(0.8)
        t = A.textof("#knStStudio")
        check("Under review" in t and "Application sent." in t, "the Games tab shows Under review")

        A.click("knStTab4")
        check(A.has("knStAnalytics"), "the Analytics tab loads")
        A.shot("studio-analytics")

        B = Client(names[1], tb, "member")
        clients.append(B)
        check(B.has("menuBtnSocial", 40.0), "the second client's page is up")
        B.ui("showWindow(10)")
        B.ui("windows.9.switchTab(5)")
        check(B.has("knStudiosOpen"), "the member's My Games shows Studios")
        time.sleep(1.5)
        check(B.textof("#studioInviteCount") == "1", "My Games: one pending invite (setStudioInviteCount)")
        B.click("knStudiosOpen")
        iid = inv[0][0] if inv else 0
        check(B.has("knStAccept%d" % iid), "the Studios window lists the invite with Accept")
        B.shot("studios-invite")
        B.click("knStAccept%d" % iid)
        check(bool(wait_q("SELECT 1 FROM studio_members WHERE studio_id = ? AND account_id = ? AND role = 2", (sid, b), lambda r: r)),
              "Accept: the member is in the studio")
        check(B.has("knStRow%d" % sid), "the studio is in the member's list")
        B.click("knStRow%d" % sid)
        check(B.has("knStLeave"), "the member's studio view offers Leave, not owner actions")
        B.shot("studio-member")

        A.ui("knStudios('tab',0)")
        A.click("knStDelete")
        check(A.has("knStConfirm"), "Delete asks first (the confirm prompt)")
        A.shot("delete-confirm")
        A.click("knStConfirm")
        check(bool(wait_q("SELECT count(*) FROM studios WHERE id = ?", (sid,), lambda r: r and r[0][0] == 0)), "Delete: the studio is gone")
        check(A.has("knStRoot"), "after Delete the Studios window is back")
        time.sleep(1.0)
        check(studio not in A.textof("#knStRoot"), "the deleted studio left the list")
    except Exception as e:
        import traceback; traceback.print_exc(); check(False, "exception: %r" % e)
    finally:
        for c in clients: c.close()
        brain.terminate()
        try: brain.wait(10)
        except subprocess.TimeoutExpired: brain.kill()
        log.close()
    print("v10 studios e2e: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"): shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

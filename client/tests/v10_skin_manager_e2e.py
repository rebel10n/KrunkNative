#!/usr/bin/env python3
"""v10.0.7's Skin Manager end to end through the native client, headless (client/src/ui/v10_skin_manager.c,
services/brain/skins.c).

  python3 -I client/tests/v10_skin_manager_e2e.py <native build dir> <repo root>

kn_brain on sqlite + a signed-in headless client on the sandbox page (--offline --sandbox: connectToSandbox's menu),
driven by KRUNKNATIVE_UI_CMDS. The side nav's Skin Manager (shown only in the sandbox, $skinManagerEnabled) opens the
window; Add new skin creates "New Skin" at once (the editor shows its id), a rename and a rarity are saved, Back
lists it, Duplicate makes "<name> (Copy)", the search narrows the table, Delete asks first and removes it, the close
button closes the window. A second client on the live offline menu has no Skin Manager item. Outcomes are checked
in the database. 77 = skip (no client_bin / kn_brain / websockets).
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
WORK = tempfile.mkdtemp(prefix="kn-v10-skins.", dir=os.path.expanduser("~/.cache"))
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
        await ws.send(json.dumps(["a", 0, [name, "skins_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


class Client:
    def __init__(self, name, token, tag, args):
        self.tag, self.cmds = tag, os.path.join(WORK, tag + ".cmds")
        self.log = os.path.join(WORK, tag + ".log")
        cfg = os.path.join(WORK, tag + "-cfg")
        os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
        if name:
            open(os.path.join(cfg, "krunknative", "krunker_username.json"), "w").write(name)
            open(os.path.join(cfg, "krunknative", "krunker_token.json"), "w").write(token)
        open(self.cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                    "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UI_CMDS": self.cmds, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                    "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % PORT})
        self.out = open(self.log, "w")
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT] + args, stdout=self.out, stderr=subprocess.STDOUT,
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

    def gone(self, elem_id, timeout=8.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.mark()
            self.cmd("rect " + elem_id)
            if self.wait_line("#%s: no box" % elem_id, m, 2.0): return True
            time.sleep(0.2)
        return False

    def textof(self, sel):
        m = self.mark()
        self.cmd("textof " + sel)
        if not self.wait_line("[ui_cmds] textof %s = " % sel, m, 5.0): return ""
        found = re.findall(r"\[ui_cmds\] textof %s = '(.*)'" % re.escape(sel), self.text()[m:])
        return found[-1] if found else ""

    def wait_text(self, sel, needle, timeout=8.0):
        end = time.time() + timeout
        t = ""
        while time.time() < end:
            t = self.textof(sel)
            if needle in t: return t
            time.sleep(0.3)
        return t

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
               "brain": {"host": "127.0.0.1", "port": PORT, "federation": False, "internal_secret": "v10skins-%d" % os.getpid()},
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
        name = "skM%s" % stamp
        aid, tok = asyncio.run(register(name))

        A = Client(name, tok, "sandbox", ["--offline", "--sandbox"])
        clients.append(A)
        check(A.has("menuBtnSocial", 40.0), "the sandbox page is up")
        check(A.has("menuBtnSkinManager"), "the sandbox menu shows the Skin Manager item ($skinManagerEnabled)")
        A.click("menuBtnSkinManager")
        check(A.has("knSmWindow"), "Skin Manager opens its window (showSkinManager)")
        check("No data available" in A.wait_text("#knSmWindow", "No data available"), "the table: no skin yet")
        A.shot("empty")

        A.click("knSmAdd")
        rows = wait_q("SELECT id, name, type, weapon, rarity FROM custom_skins WHERE owner_id = ?", (aid,), lambda r: r)
        check(len(rows) == 1 and rows[0][1:] == ("New Skin", 0, 1, 0), "Add new skin: \"New Skin\" (weapon 1) is in kn_brain at once")
        sid = rows[0][0] if rows else 0
        check(str(sid) in A.wait_text("#skin-id", str(sid)), "the editor shows the new skin's id")
        A.shot("editor-general")

        A.ui("knSkins('input','name','Neon Sniper')")
        A.ui("knSkins('set','rarity','3')")
        A.click("knSmSave")
        check(bool(wait_q("SELECT 1 FROM custom_skins WHERE id = ? AND name = 'Neon Sniper' AND rarity = 3", (sid,), lambda r: r)),
              "Save: the new name and rarity are in kn_brain")
        A.click("knSmTab1")
        check(A.has("copy-id-select"), "the Appearance tab's Same attributes as")
        A.click("knSmTab2")
        check(A.has("knSmAutoAttrs"), "the Advanced tab's Automatic Attributes")
        A.shot("editor-advanced")
        A.click("knSmBack")
        t = A.wait_text("#knSmRows", "Neon Sniper #%d" % sid)
        check("Neon Sniper #%d" % sid in t, "Back: the table lists \"Neon Sniper #id\"")

        A.click("knSmDuplicate%d" % sid)
        dup = wait_q("SELECT id FROM custom_skins WHERE owner_id = ? AND name = 'Neon Sniper (Copy)'", (aid,), lambda r: r)
        check(bool(dup), "Duplicate: \"Neon Sniper (Copy)\" is in kn_brain")
        did = dup[0][0] if dup else 0
        check(A.has("knSmRow%d" % did), "the copy is in the table")
        A.shot("list")

        A.type("knSmSearch", "copy")
        time.sleep(1.0)
        check(A.gone("knSmRow%d" % sid) and A.has("knSmRow%d" % did), "the search narrows the table to the copy")
        A.click("knSmSearchClear")
        check(A.has("knSmRow%d" % sid), "clearing the search lists both again")

        A.click("knSmDelete%d" % did)
        check(A.has("knSmConfirm"), "Delete asks first (Delete Skin)")
        A.shot("delete-confirm")
        A.click("knSmConfirm")
        check(bool(wait_q("SELECT count(*) FROM custom_skins WHERE id = ?", (did,), lambda r: r and r[0][0] == 0)), "Delete: the copy is gone")
        check(A.gone("knSmRow%d" % did) and A.has("knSmRow%d" % sid), "the table no longer lists it")

        A.click("knSmClose")
        check(A.gone("knSmWindow"), "the close button closes the window")

        B = Client(None, None, "live", ["--offline"])
        clients.append(B)
        check(B.has("menuBtnSocial", 40.0), "the live offline page is up")
        check(B.gone("menuBtnSkinManager"), "the live menu has no Skin Manager item")
    except Exception as e:
        import traceback; traceback.print_exc(); check(False, "exception: %r" % e)
    finally:
        for c in clients: c.close()
        brain.terminate()
        try: brain.wait(10)
        except subprocess.TimeoutExpired: brain.kill()
        log.close()
    print("v10 skin manager e2e: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"): shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

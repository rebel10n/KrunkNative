#!/usr/bin/env python3
"""The account window's Settings tab against v10.0.7 through the native client, headless (client/src/ui/account_ui.c).

  python3 -I services/brain/tests/account_settings_e2e.py <native build dir> <repo root>

kn_brain on sqlite + a headless client on the v10 page, driven by KRUNKNATIVE_UI_CMDS (as v10_social_e2e.py):
  - Account Security has v10.0.7's New / Repeat password fields only (no current password); Submit sends
    ["upp", new, repeat, the session token] (changePass, deob-10.0.7.js:216987-216993) and the password changes
  - Enable 2FA is v10.0.7's io-popup form (act2FA, :217005-217091): POST /account/2fa/enable/start, the QR code and
    six code boxes; a wrong code asks again, the right one enables 2FA (the button turns Disable)
  - Disable 2FA (dact2FA, :217093-217146): the code form, POST /account/2fa/disable, the button turns Enable
  - Request Deletion (requestDeletion / requestConfirm, :216941-216985): the YES / NO confPop, rpc("rqd", 1), logged
    out and the "Account Deletion Requested" modal; the next sign-in of the account cancels it by itself (:216885:
    `acc.reqDel && requestConfirm(0)`) and shows "Account Deletion Cancelled"
Outcomes are checked in the database. 77 = skip (no client_bin / kn_brain / websockets).
"""
import asyncio, base64, hashlib, hmac, json, os, re, shutil, sqlite3, struct, subprocess, sys, tempfile, time
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
WORK = tempfile.mkdtemp(prefix="kn-account-settings.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
PORT = free_ports(1)[0]
PASS1, PASS2 = "settings_pass_1", "settings_pass_2"


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond: FAILS.append(what)


def q(sql, *args):
    con = sqlite3.connect(DB, timeout=30)
    try:
        rows = con.execute(sql, args).fetchall(); con.commit(); return rows
    finally:
        con.close()


def totp(secret_b32, step):
    """RFC 6238, SHA-1, 6 digits: the code of one 30 s step (the brain's kn_totp_code)"""
    key = base64.b32decode(secret_b32.upper() + "=" * (-len(secret_b32) % 8))
    h = hmac.new(key, struct.pack(">Q", step), hashlib.sha1).digest()
    o = h[-1] & 0x0F
    return "%06d" % ((struct.unpack(">I", h[o:o + 4])[0] & 0x7FFFFFFF) % 1000000)


async def social_auth(kind, name, password):
    """["a", 0 register | 1 login, [name, password, null]] -> (account id, session token)"""
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT) as ws:
        await ws.send(json.dumps(["a", kind, [name, password, None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


def wait_until(fn, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        if fn(): return True
        time.sleep(0.2)
    return False


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

    def count(self, sel, timeout=4.0):
        m = self.mark()
        self.cmd("count " + sel)
        if not self.wait_line("count %s = " % sel, m, timeout): return -1
        found = re.search(r"count %s = (\d+)" % re.escape(sel), self.text()[m:])
        return int(found.group(1)) if found else -1

    def count_is(self, sel, n, timeout=10.0):
        return wait_until(lambda: self.count(sel) == n, timeout)

    def ui(self, calls):
        self.cmd("ui " + calls); time.sleep(0.6)

    def type(self, elem_id, chars):
        self.click(elem_id); self.cmd("text " + chars); time.sleep(0.5)

    def code(self, digits):
        """the six code boxes: the first focused, a digit each (each box's input moves the focus to the next)"""
        self.click("knTfaCode0")
        for d in digits:
            self.cmd("text " + d); time.sleep(0.3)
        time.sleep(0.6)

    def shot(self, name):
        path = os.path.join(WORK, "%s-%s.png" % (self.tag, name))
        self.cmd("shot " + path); time.sleep(0.8); return path

    def close(self):
        self.cmd("quit")
        try: self.proc.wait(15)
        except subprocess.TimeoutExpired:
            self.proc.kill(); self.proc.wait(5)
        self.out.close()


def settings(c):
    c.ui("showWindow(5)")
    c.ui("windows[4].switchTab(2)")
    return c.has("newPass")


def main():
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
               "brain": {"host": "127.0.0.1", "port": PORT, "federation": False, "internal_secret": "acctsettings-%d" % os.getpid()},
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
        name = "acset%d" % (int(time.time()) % 100000)
        aid, token = asyncio.run(social_auth(0, name, PASS1))
        q("UPDATE players SET player_score = 2000000 WHERE player_id = ?", aid)
        A = Client(name, token, "a"); clients.append(A)
        check(A.has("menuBtnSocial", 40), "the v10 main page is up with the stored session")

        # ---- Account Security: v10.0.7's two password fields, changePass -> ["upp", new, repeat, token]
        check(settings(A), "Settings tab: Account Security's New Password field")
        check(A.count("#oldPass") == 0, "no Current Password field (v10.0.7 has New / Repeat only)")
        check(A.count("#repNPass") == 1, "the Repeat Password field")
        A.type("newPass", PASS2)
        A.type("repNPass", PASS2)
        A.ui("changePass()")
        check(wait_until(lambda: "Password updated" in (A.textof("newPRes") or ""), 15),
              "Submit: #newPRes says the password was updated (%r)" % A.textof("newPRes"))
        check(q("SELECT password_changed_at IS NOT NULL FROM accounts WHERE id = ?", aid) == [(1,)], "the password changed in the database")

        # ---- Enable 2FA: the start request, the QR code form, a wrong code asks again, the right one enables it
        A.ui("act2FA()")
        check(A.has("knTfaQr"), "Enable: the form shows the QR code (enable2FAStart's qr_code_url)")
        check(A.count(".code-input-container input") == 6, "six code boxes (codeLength 6)")
        check(A.count("#knTfaSubmit[disabled]") == 1, "Submit is disabled until the code is complete")
        pending = q("SELECT twofa_pending FROM accounts WHERE id = ?", aid)
        check(pending and pending[0][0], "enable/start stored a pending secret")
        A.code("000000")
        check(wait_until(lambda: A.count("#knTfaQr") == 1 and "Invalid code. Please try again." in (A.textof(".io-card") or ""), 10),
              "a wrong code: the form asks again (\"Invalid code. Please try again.\")")
        A.shot("twofa-invalid")
        check(q("SELECT twofa_secret IS NULL FROM accounts WHERE id = ?", aid) == [(1,)], "a wrong code enables nothing")
        secret = pending[0][0] if pending else "AAAA"
        A.code(totp(secret, int(time.time()) // 30))
        check(wait_until(lambda: q("SELECT twofa_secret IS NOT NULL FROM accounts WHERE id = ?", aid) == [(1,)], 10),
              "the right code enables 2FA (enable/verify)")
        check(A.count_is("#knTfaCode0", 0), "the form closed")
        check(A.count_is(".subPassChg[onclick='dact2FA()']", 1), "the window redraws: the 2FA button says Disable")

        # ---- Disable 2FA: the code form, disable2FA
        A.ui("dact2FA()")
        check(A.has("knTfaCode0") and A.count("#knTfaQr") == 0, "Disable: the code form, no QR code")
        A.code(totp(secret, int(time.time()) // 30 + 1))  # the next step's code (the last one used is spent)
        check(wait_until(lambda: q("SELECT twofa_secret IS NULL FROM accounts WHERE id = ?", aid) == [(1,)], 10),
              "the code disables 2FA (/account/2fa/disable)")
        check(A.count_is(".subPassChg[onclick='act2FA()']", 1), "the 2FA button says Enable again")

        # ---- Account Privacy: requestDeletion(1) asks, requestConfirm(1) requests, logs out, the modal
        check(settings(A), "Settings tab again")
        check(A.count(".menuLink[onclick='requestDeletion(1)']") == 1, "Delete Account: Request Deletion")
        A.ui("requestDeletion(1)")
        check(A.has("confirmBtn") and A.count("#genericPop.confPop #declineBtn") == 1, "the YES / NO confPop")
        check("request an account deletion" in (A.textof("genericPop") or ""), "its text is account.delete.request")
        A.click("confirmBtn")
        check(wait_until(lambda: q("SELECT delete_requested_at IS NOT NULL FROM accounts WHERE id = ?", aid) == [(1,)], 10),
              "YES: rpc(\"rqd\", 1) stored the request")
        check(A.count_is(".success-popup-container", 1), "the \"Account Deletion Requested\" modal")
        check(A.count(".popup.svelte-1wz7ymp[style*='700px']") == 1, "the modal is 700 px wide")
        check(A.count_is("#knV10Login", 0), "the logout's login modal closed with the view stack (iííîîiï)")
        check(wait_until(lambda: q("SELECT count(*) FROM sessions WHERE account_id = ?", aid) == [(0,)], 10),
              "the account's sessions ended (the client logged out)")
        A.shot("deletion-requested")
        A.close(); clients.remove(A)

        # ---- the next sign-in cancels the request by itself
        _, token2 = asyncio.run(social_auth(1, name, PASS2))
        check(bool(token2), "the new password signs in")
        B = Client(name, token2, "b"); clients.append(B)
        check(B.has("menuBtnSocial", 40), "the client signs in again")
        check(wait_until(lambda: q("SELECT delete_requested_at IS NULL FROM accounts WHERE id = ?", aid) == [(1,)], 15),
              "acc.reqDel at sign-in: requestConfirm(0) cancelled the deletion")
        check(B.count_is(".success-popup-container", 1), "the \"Account Deletion Cancelled\" modal")
        B.shot("deletion-cancelled")
        check(settings(B) and B.count(".menuLink[onclick='requestDeletion(1)']") == 1, "Account Privacy: Request Deletion again")
    except Exception as e:
        import traceback; traceback.print_exc(); check(False, "exception: %r" % e)
    finally:
        for c in clients: c.close()
        brain.terminate()
        try: brain.wait(10)
        except subprocess.TimeoutExpired: brain.kill()
        log.close()
    print("account settings e2e: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"): shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

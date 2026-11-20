#!/usr/bin/env python3
"""Krunker v10.0.7's email preference through the native client (client/src/ui/account_ui.c), headless:
kn_brain on sqlite (GET / POST /email/preferences, v10_auth.c, migration 0391) + one client_bin on the v10 menu at a
time, signed in from seeded storage and driven through KRUNKNATIVE_UI_CMDS. Outcomes are checked in the database.

  email_prefs_e2e.py <native build dir> <repo root>

  1. nothing chosen: the opt-in popup (deob-10.0.7.js:216865-216873, 116848-116905) opens ~3 s after the sign-in;
     a click on its overlay closes it with nothing stored (dismissable)
  2. the next run opens it again; Accept stores {chosen, promotional: true, source: popup} and closes it
  3. Settings > Account's Promotional Emails toggle (togglePromoEmails, 217694-217698) stores source "settings"
  4. a run with a choice stored opens no popup
  5. another account's Decline stores promotional false
  6. a brain that does not ask (no smtp, accounts.email.promotional_optin unset) opens no popup
Screenshots go to the work dir (~/.cache/kn-email-prefs.*, kept on failure or with KN_KEEP=1).
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
import urllib.request

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
WORK = tempfile.mkdtemp(prefix="kn-email-prefs.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")


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


def pref(aid):
    """(chosen, promotional, source) of the account"""
    r = q("SELECT email_pref_at IS NOT NULL, email_promotional, email_pref_source FROM accounts WHERE id = ?", aid)
    return (bool(r[0][0]), bool(r[0][1]), r[0][2]) if r else None


def wait(fn, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return fn()


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, "email_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f and f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


class Client:
    """One headless client_bin on the v10 menu, signed in as `name` from its saved session."""

    def __init__(self, port, name, token, tag):
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
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_UI_DIR")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state"),
                    "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UI_CMDS": self.cmds,
                    "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port})
        self.out = open(self.log, "w")
        self.proc = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT, "--offline"], stdout=self.out, stderr=subprocess.STDOUT,
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
        mark = len(self.text())
        self.cmd(line)
        end = time.time() + timeout
        while time.time() < end:
            m = re.search(pattern, self.text()[mark:])
            if m:
                return m
            time.sleep(0.1)
        return None

    def count(self, sel):
        m = self.ask("count " + sel, r"count %s = (\d+)" % re.escape(sel))
        return int(m.group(1)) if m else -1

    def has(self, elem_id, timeout=20.0):
        end = time.time() + timeout
        while time.time() < end:
            m = self.ask("rect " + elem_id, r"#%s (rect|: no box)" % re.escape(elem_id), 1.5)
            if m and m.group(1) == "rect":
                return True
            time.sleep(0.3)
        return False

    def click(self, elem_id):
        self.cmd("clickid " + elem_id)
        time.sleep(0.6)

    def ui(self, calls):
        self.cmd("ui " + calls)
        time.sleep(0.6)

    def shot(self, name):
        path = os.path.join(WORK, "%s-%s.png" % (self.tag, name))
        self.cmd("shot " + path)
        time.sleep(0.8)
        return path

    def close(self):
        self.cmd("quit")
        try:
            self.proc.wait(15)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(5)
        self.out.close()


def popup_count(c):
    return c.count("#knOptIn")


def main():
    port = free_ports(1)[0]
    cfg = os.path.join(WORK, "brain.json")
    with open(cfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "emailprefs-%d" % os.getpid()},
                   "accounts": {"signup_rewards": {"enabled": False}, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0,
                                "email": {"promotional_optin": True},  # no smtp here: ask anyway (v10_auth.c)
                                "password_iterations": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    log = open(os.path.join(WORK, "brain.log"), "w")
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    clients = []

    def run(name, token, tag):
        c = Client(port, name, token, tag)
        clients.append(c)
        return c

    def done(c):
        c.close()
        clients.remove(c)

    try:
        up = wait(lambda: re.search(r"listening on http://127\.0\.0\.1:%d" % port, open(os.path.join(WORK, "brain.log")).read()), 30)
        check(bool(up), "kn_brain is up")
        stamp = str(int(time.time()) % 100000)
        a_name, b_name = "epa%s" % stamp, "epb%s" % stamp
        a, a_tok = asyncio.run(register(port, a_name))
        b, b_tok = asyncio.run(register(port, b_name))
        check(pref(a) == (False, False, None), "a new account has no email preference")

        # 1. the popup opens ~3 s after the sign-in; the overlay dismisses it, nothing stored
        A = run(a_name, a_tok, "a1")
        check(A.has("menuBtnSocial", 40), "the v10 menu is up")
        check(A.has("knOptInYes", 20), "nothing chosen: the opt-in popup opens after the sign-in")
        A.shot("optin")
        A.click("knOptIn")  # the host's centre is the popup itself: a click there is not the overlay's
        check(popup_count(A) == 1, "a click inside the popup keeps it open")
        A.ui("knOptInClose()")  # the overlay's onclick
        check(wait(lambda: popup_count(A) == 0, 5), "a click on the overlay closes it")
        time.sleep(1.0)
        check(pref(a) == (False, False, None), "dismissed: nothing stored")
        done(A)

        # 2. the next run asks again; Accept stores the choice
        A = run(a_name, a_tok, "a2")
        check(A.has("knOptInYes", 40), "the next run opens the popup again (still not chosen)")
        A.click("knOptInYes")
        check(wait(lambda: pref(a) == (True, True, "popup"), 10), "Accept: chosen, promotional true, source popup (%r)" % (pref(a),))
        check(wait(lambda: popup_count(A) == 0, 5), "the popup closes after the answer")

        # 3. the Account settings toggle (togglePromoEmails(this.checked) -> setPreference(on, "settings"))
        A.ui("togglePromoEmails(0)")
        check(wait(lambda: pref(a) == (True, False, "settings"), 10), "togglePromoEmails(0): promotional false, source settings (%r)" % (pref(a),))
        A.ui("togglePromoEmails(1)")
        check(wait(lambda: pref(a) == (True, True, "settings"), 10), "togglePromoEmails(1): promotional true again (%r)" % (pref(a),))
        done(A)

        # 4. a choice is stored: no popup
        A = run(a_name, a_tok, "a3")
        check(A.has("menuBtnSocial", 40), "the menu is up again")
        time.sleep(6.0)  # past the 3 s timer and its read
        check(popup_count(A) == 0, "a stored choice: no opt-in popup")
        done(A)

        # 5. another account's Decline
        B = run(b_name, b_tok, "b1")
        check(B.has("knOptInNo", 40), "the second account gets the popup")
        B.click("knOptInNo")
        check(wait(lambda: pref(b) == (True, False, "popup"), 10), "Decline: chosen, promotional false, source popup (%r)" % (pref(b),))
        done(B)

        # 6. the same brain without accounts.email.promotional_optin and without smtp: nothing to ask
        brain.terminate()
        brain.wait(10)
        with open(cfg) as f:
            conf = json.load(f)
        del conf["accounts"]["email"]
        with open(cfg, "w") as f:
            json.dump(conf, f)
        log.write("---- restart without promotional_optin\n")
        log.flush()
        mark = len(open(os.path.join(WORK, "brain.log")).read())
        brain = subprocess.Popen([BRAIN, "--config", cfg], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        check(bool(wait(lambda: re.search(r"listening on http://127\.0\.0\.1:%d" % port, open(os.path.join(WORK, "brain.log")).read()[mark:]), 30)),
              "kn_brain is up again")
        c_name = "epc%s" % stamp
        c, c_tok = asyncio.run(register(port, c_name))
        req = urllib.request.Request("http://127.0.0.1:%d/email/preferences" % port, headers={"Authorization": "Bearer " + c_tok})
        with urllib.request.urlopen(req, timeout=15) as resp:
            got = json.loads(resp.read())
        check((got.get("data") or {}).get("chosen") is True, "no mailer and no promotional_optin: GET answers chosen (no popup): %r" % (got,))
    except Exception as e:  # pragma: no cover
        check(False, "exception: %r" % e)
    finally:
        for c in list(clients):
            c.close()
        brain.terminate()
        try:
            brain.wait(10)
        except subprocess.TimeoutExpired:
            brain.kill()
        log.close()
    if FAILS or os.environ.get("KN_KEEP"):
        print("work dir kept: %s" % WORK)
    else:
        shutil.rmtree(WORK, ignore_errors=True)
    print("%d failure(s)" % len(FAILS))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

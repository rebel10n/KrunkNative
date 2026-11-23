#!/usr/bin/env python3
"""Krunker v10.0.7's Premium popups, the display name's Clear and the disconnect report through the native client,
headless: kn_brain on sqlite + one client_bin on the v10 menu at a time, signed in from seeded storage and driven
through KRUNKNATIVE_UI_CMDS. Outcomes are checked in the page, the client's log, the clipboard and the database.

  premium_dc_e2e.py <native build dir> <repo root>

  1. Premium (deob-10.0.7.js:223527-223606) on a Box Office account: premiumPopup(1) draws "30 days of Premium" with
     Buy - 4.99 USD and GIFT; Buy (selectPremium), the gift form (selectPremiumGift -> giftPremium) and confPremium's
     YES (premiumConfirmed) each end on the store's no-payments answer: Premium has only USD prices and Box Office no
     payment provider (doc/v10/checkout-*.md). No account and no premium change.
  2. clearName (223613-223620): a Premium account's display name is cleared (players.player_alias NULL, a name_changes
     row); an account without Premium gets "Premium required" and keeps its name.
  3. The disconnect report (229972-229997): disconnectPopup(code, reason) draws the Report popup (genericPop
     "disconnectPop"); Copy Data puts v10's JSON blob (JSON.stringify(.., null, 2): ua, savedRegion, connectedRegion,
     connectedHost, ping, v, steam, client, mobile, browser, code, reason, time) on the clipboard, the button reads
     "Copied", then its text again after 1250 ms.
Screenshots go to the work dir (~/.cache/kn-premium-dc.*, kept on failure or with KN_KEEP=1).
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
WORK = tempfile.mkdtemp(prefix="kn-premium-dc.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
NO_PAY = "Purchases are not available in Krunker Box Office."
DC_KEYS = ["ua", "savedRegion", "connectedRegion", "connectedHost", "ping", "v", "steam", "client", "mobile", "browser", "code",
           "reason", "time"]


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
    return r[0][0] if r else None


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
        await ws.send(json.dumps(["a", 0, [name, "premium_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f and f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


def unescape_clip(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            out.append("\n" if s[i + 1] == "n" else s[i + 1])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


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

    def textof(self, sel):
        m = self.ask("textof " + sel, r"textof %s = '(.*)'" % re.escape(sel))
        return m.group(1) if m else None

    def text_has(self, sel, needle, timeout=10.0):
        return wait(lambda: needle in (self.textof(sel) or ""), timeout)

    def clip(self):
        m = self.ask("clip", r"\[ui_cmds\] clip = '(.*)'\n")
        return unescape_clip(m.group(1)) if m else None

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

    def type(self, elem_id, chars):
        self.cmd("clickid " + elem_id)
        time.sleep(0.4)
        self.cmd("text " + chars)
        time.sleep(0.5)

    def alerts(self, text):
        return self.text().count("[ui log] alert: " + text)

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


def premium(c, aid):
    funds = one("SELECT player_funds FROM players WHERE player_id = ?", aid)
    prem = one("SELECT player_premium FROM players WHERE player_id = ?", aid)
    c.ui("clearPops();premiumPopup(1)")
    check(c.has("premiumPop"), "premiumPopup(1) opens #premiumPop")
    pop = c.textof("#premiumPop") or ""
    check("30 days of Premium" in pop and "Buy - 4.99 USD" in pop and "GIFT" in pop,
          "the popup: 30 days of Premium, Buy - 4.99 USD, GIFT: %r" % pop[:160])
    check("Golden Badge on Profile" in pop and "& more" in pop, "and v10's benefit list")
    c.shot("premium-popup")
    n = c.alerts(NO_PAY)
    c.ui("selectPremium(1)")
    check(wait(lambda: c.alerts(NO_PAY) == n + 1, 10), "Buy (selectPremium): the store's no-payments answer")
    c.click("confirmBtn")  # the alert's OK
    c.ui("clearPops();selectPremiumGift(this, 2)")
    check(c.has("knPromptInput"), "Gift (selectPremiumGift): the gift form asks for a player name")
    check(c.text_has("#genericPop", "60 days of Premium"), "naming '60 days of Premium': %r" % (c.textof("#genericPop") or "")[:160])
    c.type("knPromptInput", "somebody")
    c.click("confirmBtn")
    check(wait(lambda: c.alerts(NO_PAY) == n + 2, 10), "a named gift (giftPremium): the no-payments answer")
    c.click("confirmBtn")
    c.ui("clearPops();confPremium(3)")
    check(c.text_has("#genericPop", "Are you sure you want to purchase 90 days of Premium?"),
          "confPremium(3): the Confirmation popup: %r" % (c.textof("#genericPop") or "")[:160])
    check(c.text_has("#genericPop", "Confirmation") and c.count("#genericPop #declineBtn") == 1, "with YES / NO")
    c.shot("conf-premium")
    c.click("confirmBtn")
    check(wait(lambda: c.alerts(NO_PAY) == n + 3, 10), "YES (premiumConfirmed): the no-payments answer")
    c.click("confirmBtn")
    check(one("SELECT player_funds FROM players WHERE player_id = ?", aid) == funds and
          one("SELECT player_premium FROM players WHERE player_id = ?", aid) == prem, "nothing was charged or granted")


def clear_name(c, aid, alias):
    check(one("SELECT player_alias FROM players WHERE player_id = ?", aid) == alias, "the Premium account has a display name")
    c.ui("clearPops();clearName()")
    check(wait(lambda: one("SELECT player_alias FROM players WHERE player_id = ?", aid) is None, 10),
          "clearName: players.player_alias is NULL")
    check(one("SELECT count(*) FROM name_changes WHERE account_id = ? AND kind = 'alias' AND old_name = ? AND new_name = ''", aid, alias) == 1,
          "a name_changes row records the clear")
    check(c.text_has("#popupContent", "Success"), "the SUCCESS popup: %r" % c.textof("#popupContent"))


def dc_report(c):
    c.ui("clearPops();disconnectPopup('1006', 'Connection lost')")
    check(c.text_has("#genericPop", "Copy Paste this error data and send it to support."), "disconnectPopup: the Report popup")
    check(c.count(".disconnectPop") == 1 and c.count("#genericPop .mapFndB") == 1, "genericPop 'disconnectPop' with Copy Data")
    check((c.textof("#genericPop .imageListHeader") or "") == "Report", "headed Report")
    c.shot("dc-popup")
    c.cmd("clicksel 0 #genericPop .mapFndB")
    check(c.text_has("#genericPop .mapFndB", "Copied", 5), "Copy Data: the button reads Copied")
    clip = c.clip() or ""
    try:
        data = json.loads(clip, object_pairs_hook=lambda kv: kv)
    except ValueError:
        data = None
    check(data is not None, "the clipboard holds JSON: %r" % clip[:200])
    keys = [k for k, _ in data] if data else []
    d = dict(data or [])
    check(keys == DC_KEYS, "v10's keys in v10's order: %r" % keys)
    check(clip.startswith('{\n  "ua": "') and clip.endswith('"\n}'), "JSON.stringify(.., null, 2) layout")
    check(d.get("code") == "1006" and d.get("reason") == "Connection lost", "code and reason: %r %r" % (d.get("code"), d.get("reason")))
    check(d.get("v") == "10.0.7r1", "v = clientVersion: %r" % d.get("v"))
    check(d.get("steam") is False and d.get("client") is False and d.get("mobile") is False, "steam / client / mobile false")
    check(isinstance(d.get("ping"), (int, float)), "ping is a number: %r" % d.get("ping"))
    ua = d.get("ua") or ""
    check(ua.startswith("Mozilla/5.0 (") and "KrunkNative/" in ua, "ua: %r" % ua)
    check(d.get("browser") == ua[ua.rfind(") ") + 2:] and d["browser"].startswith("KrunkNative/"),
          "browser = ua.slice(ua.lastIndexOf(') ') + 2): %r" % d.get("browser"))
    check(re.match(r"^\w{3} \w{3} \d\d \d{4} \d\d:\d\d:\d\d GMT[+-]\d{4} \(.+\)$", d.get("time") or "") is not None,
          "time = new Date().toString(): %r" % d.get("time"))
    check("[dc] report copied" in c.text(), "the client logged the copy")
    check(c.text_has("#genericPop .mapFndB", "Copy Data", 5), "after 1250 ms the button reads Copy Data again")


def main():
    port = free_ports(1)[0]
    cfg = os.path.join(WORK, "brain.json")
    with open(cfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "premiumdc-%d" % os.getpid()},
                   "accounts": {"signup_rewards": {"enabled": False}, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0,
                                "password_iterations": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    log = open(os.path.join(WORK, "brain.log"), "w")
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    clients = []
    try:
        up = wait(lambda: re.search(r"listening on http://127\.0\.0\.1:%d" % port, open(os.path.join(WORK, "brain.log")).read()), 30)
        check(bool(up), "kn_brain is up")
        stamp = str(int(time.time()) % 100000)
        a_name, b_name = "pda%s" % stamp, "pdb%s" % stamp
        a, a_tok = asyncio.run(register(port, a_name))
        b, b_tok = asyncio.run(register(port, b_name))
        a_alias, b_alias = "Alias%s" % stamp, "Blias%s" % stamp
        q("UPDATE players SET player_premium = ?, player_alias = ? WHERE player_id = ?", 30 * 86400000, a_alias, a)
        q("UPDATE players SET player_alias = ? WHERE player_id = ?", b_alias, b)

        A = Client(port, a_name, a_tok, "a")
        clients.append(A)
        check(A.has("menuBtnSocial", 60), "the v10 menu is up")
        premium(A, a)
        clear_name(A, a, a_alias)
        dc_report(A)
        A.close()
        clients.remove(A)

        B = Client(port, b_name, b_tok, "b")
        clients.append(B)
        check(B.has("menuBtnSocial", 60), "the second account's menu is up")
        B.ui("clearPops();clearName()")
        check(B.text_has("#popupContent", "Premium required"), "clearName without Premium: Premium required: %r" % B.textof("#popupContent"))
        time.sleep(1.0)
        check(one("SELECT player_alias FROM players WHERE player_id = ?", b) == b_alias, "and the display name stays")
    except Exception as e:  # pragma: no cover
        import traceback
        traceback.print_exc()
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
    print("premium / disconnect report e2e (native): " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""The economy through the native client's hub (client/src/ui/social_ui.c), headless:
kn_brain on sqlite + one client_bin at a time, logged in from seeded storage and driven through
KRUNKNATIVE_UI_CMDS (real clicks on the page's buttons, typing into its inputs, and the page's
inline handlers where a button has no id). Every outcome is checked in the database.

  economy_hub_e2e.py <native build dir> <repo root>

  1. alice opens bob's profile: "Send Gift" -> types 150 -> the fee line -> Gift KR
  2. alice: "Trade" -> both inventories -> adds an item each side + 25 KR -> Submit -> YES
  3. bob: Market > Trading -> the incoming offer -> Accept -> YES: items and KR move
  4. bob: Inventory -> the item he got -> List Item 300 -> YES (10 % fee)
  5. alice: Market -> the listing -> Purchase; the KR History popup lists every movement
Screenshots go to the work dir (~/.cache/kn-econ-hub.*, kept on failure or with KN_KEEP=1).
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
WORK = tempfile.mkdtemp(prefix="kn-econ-hub.", dir=os.path.expanduser("~/.cache"))
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


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, "hub_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


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
        self.shots = 0

    def text(self):
        try:
            with open(self.log, errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def cmd(self, line):
        with open(self.cmds, "a") as f:
            f.write(line + "\n")

    def has(self, elem_id, timeout=20.0):
        """Wait until #elem_id has a box (rect printed by the client)."""
        end = time.time() + timeout
        while time.time() < end:
            mark = len(self.text())
            self.cmd("rect " + elem_id)
            t0 = time.time()
            while time.time() - t0 < 1.5:
                tail = self.text()[mark:]
                if "#%s rect" % elem_id in tail:
                    return True
                if "#%s: no box" % elem_id in tail:
                    break
                time.sleep(0.1)
            time.sleep(0.3)
        return False

    def ui(self, calls):
        self.cmd("ui " + calls)
        time.sleep(0.4)

    def click(self, elem_id):
        self.cmd("clickid " + elem_id)
        time.sleep(0.4)

    def type(self, elem_id, chars):
        self.click(elem_id)
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


def scenario(port, ids, tokens, names):
    A, B = ids
    # 1 + 2: alice on bob's profile
    c = Client(port, names[0], tokens[0], "p=profile&q=" + names[1], "alice1")
    try:
        check(c.has("profileKR", 60) and c.has("giftBtn", 30), "alice is logged in on bob's profile")
        c.click("giftBtn")
        check(c.has("giftIn"), "Send Gift opens v7's gift popup")
        c.type("giftIn", "150")
        check(c.has("giftAmntFee"), "fee line")
        c.shot("gift")
        before_a, before_b = one("SELECT player_funds FROM players WHERE player_id = ?", A), one("SELECT player_funds FROM players WHERE player_id = ?", B)
        c.click("postSaleBtn")
        ok = wait(lambda: one("SELECT player_funds FROM players WHERE player_id = ?", B) == before_b + 150, 15)
        check(ok and one("SELECT player_funds FROM players WHERE player_id = ?", A) == before_a - 165,
              "gift: bob +150, alice -165 (ceil 10 %% fee)")
        mail = q("SELECT subject FROM mail WHERE account_id = ? AND subject = 'KR Gift'", B)
        check(len(mail) == 1, "bob got the KR Gift mail")
        c.shot("gift-done")

        c.ui("clearPops()")
        c.click("tradeBtn")
        check(c.has("invList0") and c.has("trd_inv_0_4") and c.has("trd_inv_1_3"), "Trade opens both inventories")
        c.click("trd_inv_0_4")
        c.click("trd_inv_1_3")
        check(c.has("trd_0_4") and c.has("trd_1_3"), "items added to both trade lists")
        c.type("tradeKR0", "25")
        c.shot("trade")
        c.click("trdSubBt")
        check(c.has("confirmBtn"), "Submit Offer asks for confirmation")
        c.shot("trade-confirm")
        c.click("confirmBtn")
        trid = wait(lambda: one("SELECT id FROM trades WHERE sender_id = ? AND status = 'open'", A), 15)
        check(trid, "the offer reached the server")
        row = q("SELECT sender_kr, escrow FROM trades WHERE id = ?", trid)
        check(row and row[0] == (25, 1) and one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = 4", A) == 1,
              "offer escrowed: 25 KR + the item left alice's account: %r" % row)
        check(one("SELECT player_funds FROM players WHERE player_id = ?", A) == before_a - 165 - 25 - 18,
              "the trade fee (rarity fees 10 + 5, gift fee 3 on the 25 KR) was charged")
    finally:
        c.close()

    # 3 + 4: bob accepts, then lists what he got
    c = Client(port, names[1], tokens[1], "p=market&q=trades", "bob")
    try:
        check(c.has("o_in", 60) and c.has("of_%d" % trid), "bob sees the incoming offer")
        c.click("of_%d" % trid)
        check(c.has("tMsg") or c.has("genericPop"), "viewOffer opens the offer")
        c.shot("offer")
        c.ui("confOffer(%d, false)" % trid)
        check(c.has("confirmBtn"), "Accept asks for confirmation")
        bf = one("SELECT player_funds FROM players WHERE player_id = ?", B)
        c.click("confirmBtn")
        ok = wait(lambda: one("SELECT count(*) FROM trades WHERE id = ? AND status = 'accepted'", trid), 15)
        check(ok and one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = 4", B) == 1 and
              one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = 3", A) == 1 and
              one("SELECT player_funds FROM players WHERE player_id = ?", B) == bf + 25, "accepted: items and KR moved")
        c.ui("clearPops();updateWindow('market','inventory')")
        check(c.has("marketList", 30), "bob's inventory")
        time.sleep(1.5)
        c.ui("showPopup('inventory', 4, 0)")
        check(c.has("sellPriceIn"), "List popup for the item")
        c.type("sellPriceIn", "300")
        c.shot("list")
        c.click("postSaleBtn")
        check(c.has("confirmBtn"), "List Item asks for confirmation")
        bf = one("SELECT player_funds FROM players WHERE player_id = ?", B)
        c.click("confirmBtn")
        lid = wait(lambda: one("SELECT id FROM market_listings WHERE seller_id = ? AND sold_at IS NULL AND cancelled_at IS NULL", B), 15)
        check(lid and one("SELECT player_funds FROM players WHERE player_id = ?", B) == bf - 30, "listed for 300 with the 30 KR fee")
    finally:
        c.close()

    # 5: alice buys it back; KR History
    c = Client(port, names[0], tokens[0], "p=market", "alice2")
    try:
        check(c.has("marketList", 60), "alice on the market")
        time.sleep(1.5)
        c.ui("showPopup('market', 4, 0)")
        check(c.has("popupContent"), "the listing's popup")
        c.shot("buy")
        af, bf = one("SELECT player_funds FROM players WHERE player_id = ?", A), one("SELECT player_funds FROM players WHERE player_id = ?", B)
        c.ui("buyItem(%d, 0)" % lid)
        ok = wait(lambda: one("SELECT count(*) FROM market_listings WHERE id = ? AND buyer_id = ?", lid, A), 15)
        check(ok and one("SELECT player_funds FROM players WHERE player_id = ?", A) == af - 300 and
              one("SELECT player_funds FROM players WHERE player_id = ?", B) == bf + 300 and
              one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = 4", A) == 2, "bought: KR and the item moved")
        check(wait(lambda: q("SELECT 1 FROM mail WHERE account_id = ? AND subject = 'Item sold'", B), 10), "bob got v7's Item sold mail")
        c.ui("clearPops()")
        c.click("profileKR")
        check(c.has("genericPop"), "KR History popup")
        time.sleep(1.0)
        c.shot("kr-history")
    finally:
        c.close()

    for aid in ids:
        f = one("SELECT player_funds FROM players WHERE player_id = ?", aid)
        s = one("SELECT coalesce(sum(amount), 0) FROM kr_transactions WHERE account_id = ?", aid)
        check(f == s, "ledger of %d matches its balance (%d / %d)" % (aid, f, s))


def main():
    (port,) = free_ports(1)
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")  # its own config: never the developer's config/config.json
    with open(cfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "hub-e2e-%d" % os.getpid()},
                   "accounts": {"signup_rewards": {"enabled": False},  # v10 signup perks off: these count inventories
                    "sale_grace_s": 0, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0,
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
        names = ["hubal%s" % stamp, "hubbo%s" % stamp]
        ids, tokens = [], []
        for n in names:
            i, t = asyncio.run(register(port, n))
            ids.append(i)
            tokens.append(t)
        for aid in ids:
            q("UPDATE players SET player_score = 2000000, player_premium = 0, player_funds = 5000 WHERE player_id = ?", aid)
            q("INSERT INTO kr_transactions (account_id, amount, source) VALUES (?, 5000, 'admin')", aid)
        q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 4, 2)", ids[0])  # Moon Dragon, legendary
        q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 3, 1)", ids[1])  # Digital Hunt, uncommon
        scenario(port, ids, tokens, names)
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
    print("economy hub e2e (native): " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

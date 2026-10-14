#!/usr/bin/env python3
"""Krunker v10's Market & Trading popup (client/src/ui/v10_market.c) end to end against a local
kn_brain (sqlite, temp dir) and one headless client_bin at a time, from the v10 page (ui/v10, KRUNKNATIVE_UI_DIR):

  1. alice opens #menuBtnMarket: the exchange popup, the Market tab, an empty Browse
  2. alice's inventory (My Inventory): the item's card, List -> 300 KR -> confirm: the listing is in
     the database and the KR fee was charged
  3. bob's Market: the listing on the Browse card -> Purchase -> confirm: KR and the item move
  4. bob's inventory: Quick sell -> Get KR -> confirm: KR credited, the stack drops
  5. alice's Trading tab: Start a Trade -> bob -> Create Trade: add her item, add his, Submit ->
     confirm (his offer side included); then bob's Trading tab -> the incoming offer -> Accept
  6. both live balances (the header KR) and the ledger match

  python3 -I client/tests/v10_market_e2e.py BUILD_DIR      (repository root)
Headless client_bin (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs) driven through
KRUNKNATIVE_UI_CMDS (showWindow / clickid / text), ports from kn_proto.free_ports().
77 = skip (no kn_brain / client_bin / websockets).
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
WORK = tempfile.mkdtemp(prefix="kn-v10-market.", dir=os.path.expanduser("~/.cache"))
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


def wait(fn, timeout=25.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return fn()


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, "market_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


class Client:
    """One headless client_bin on the v10 page, logged in as `name`."""

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
                    "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UI_CMDS": self.cmds,
                    "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_UI_DIR": os.path.join(ROOT, "ui", "v10"),
                    "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port})
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

    def ui(self, calls):
        self.cmd("ui " + calls)
        time.sleep(0.5)

    def click(self, elem_id, timeout=25.0):
        """clickid #id once it has a box (the exchange re-renders)."""
        end = time.time() + timeout
        while time.time() < end:
            mark = len(self.text())
            self.cmd("clickid " + elem_id)
            t0 = time.time()
            while time.time() - t0 < 1.5:
                tail = self.text()[mark:]
                if "#%s rect" % elem_id in tail:
                    time.sleep(0.5)
                    return True
                if "#%s: no box" % elem_id in tail:
                    break
                time.sleep(0.1)
            time.sleep(0.3)
        return False

    def has(self, elem_id, timeout=20.0):
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

    def type(self, elem_id, chars):
        self.click(elem_id)
        self.cmd("text " + chars)
        time.sleep(0.6)

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


def open_market(c, tab_id):
    """#menuBtnMarket -> the exchange popup -> the sidebar tab."""
    check(c.has("menuBtnMarket", 60), "%s: the v10 page's Market & Trading item" % c.tag)
    c.click("menuBtnMarket")
    ok = c.has("v10mkSide%d" % tab_id, 25)
    check(ok, "%s: openExchange shows the exchange popup (%s)" % (c.tag, "tab %d" % tab_id))
    if not ok:
        raise RuntimeError("%s: no exchange popup (log %s)" % (c.tag, c.log))
    c.click("v10mkSide%d" % tab_id)
    return ok


def scenario(port, ids, tokens, names):
    A, B = ids
    # ---- 1 + 2: alice lists her item from My Inventory
    c = Client(port, names[0], tokens[0], "alice")
    try:
        open_market(c, 0)
        check(c.has("v10mkTab0", 20), "alice: the Market tab strip (Browse ... Stats)")
        c.click("v10mkTab1")  # My Inventory
        check(c.has("v10mkList4", 25), "alice: My Inventory shows her item with the List button")
        c.shot("inventory")
        c.click("v10mkList4")
        check(c.has("v10mkPrice"), "List opens the price modal")
        c.type("v10mkPrice", "300")
        check(c.has("v10mkModalOk", 15), "the fee line + the List button")
        a_before = one("SELECT player_funds FROM players WHERE player_id = ?", A)
        c.shot("list")
        c.click("v10mkModalOk")
        lid = wait(lambda: one("SELECT id FROM market_listings WHERE seller_id = ? AND sold_at IS NULL AND cancelled_at IS NULL", A))
        fee = one("SELECT player_funds FROM players WHERE player_id = ?", A)
        check(lid, "the listing reached the server")
        check(a_before - fee == 30, "the 10%% list fee was charged (%d -> %d)" % (a_before, fee))
        check(one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = 4", A) == 1, "escrowed: one left in the inventory")
        check(wait(lambda: one("SELECT count(*) FROM market_listings WHERE id = ? AND price = 300", lid), 5), "listed at 300 KR")
    finally:
        c.close()

    # ---- 3: bob buys it from Browse
    c = Client(port, names[1], tokens[1], "bob")
    try:
        open_market(c, 0)
        check(c.has("v10mkBuy%d" % lid, 30), "bob: the listing's Buy button on Browse")
        c.shot("browse")
        b_before = one("SELECT player_funds FROM players WHERE player_id = ?", B)
        c.click("v10mkBuy%d" % lid)
        check(c.has("v10mkModalOk", 15), "Confirm Purchase")
        c.click("v10mkModalOk")
        ok = wait(lambda: one("SELECT count(*) FROM market_listings WHERE id = ? AND buyer_id = ?", lid, B))
        b_after = one("SELECT player_funds FROM players WHERE player_id = ?", B)
        check(ok, "bob bought the listing")
        check(b_before - b_after == 300, "bob paid 300 KR (%d -> %d)" % (b_before, b_after))
        check(one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = 4", B) == 1, "the item moved to bob")
        check(wait(lambda: q("SELECT 1 FROM mail WHERE account_id = ? AND subject = 'Item sold'", A), 10), "alice got the Item sold mail")

        # ---- 4: bob quick sells it
        c.click("v10mkTab1")
        check(c.has("v10mkQS4", 25), "bob: his inventory's quick-sell button")
        b_before = one("SELECT player_funds FROM players WHERE player_id = ?", B)
        c.click("v10mkQS4")
        check(c.has("v10mkQsGo", 15), "the Quick Sell window")
        c.shot("quicksell")
        c.click("v10mkQsGo")
        ok = wait(lambda: one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = 4", B) == 0, 15)
        b_after = one("SELECT player_funds FROM players WHERE player_id = ?", B)
        check(ok, "the stack is gone")
        check(b_after - b_before == 100, "quick sell paid the Legendary value 100 KR (%d -> %d)" % (b_before, b_after))
    finally:
        c.close()

    # ---- 5: alice -> bob, both sides of the offer
    c = Client(port, names[0], tokens[0], "alice2")
    try:
        open_market(c, 1)
        check(c.has("v10mkCreate", 25), "alice: the Trading tab's + Create Trade")
        c.click("v10mkCreate")
        check(c.has("v10mkStartName", 15), "Start a Trade asks for a player name")
        c.type("v10mkStartName", names[1])
        c.click("v10mkStartGo")
        check(c.has("v10mkCtSubmit", 30), "the Create Trade window with both inventories")
        c.shot("create-trade")
        # alice offers her remaining Moon Dragon (4) for bob's Digital Hunt (3): click both inventory cards
        check(c.click("v10mkCtI0_4") and c.has("v10mkCtO0_4"), "alice's Moon Dragon moves to her trade list")
        check(c.click("v10mkCtI1_3") and c.has("v10mkCtO1_3"), "bob's Digital Hunt moves to his side")
        c.shot("create-trade-added")
        c.click("v10mkCtSubmit")
        check(c.has("v10mkModalOk", 15), "Submit asks for confirmation")
        c.click("v10mkModalOk")
        trid = wait(lambda: one("SELECT id FROM trades WHERE sender_id = ? AND status = 'open'", A))
        check(trid, "the offer reached the server")
        if trid:
            check(one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = 4", A) == 0, "alice's offered item left her inventory (escrow)")
            check(one("SELECT escrow FROM trades WHERE id = ?", trid) == 1, "the offer is escrowed")
    finally:
        c.close()

    # ---- 5b: bob accepts it
    c = Client(port, names[1], tokens[1], "bob2")
    try:
        open_market(c, 1)
        trid = one("SELECT id FROM trades WHERE sender_id = ? AND status = 'open'", A)
        if not trid:
            check(False, "no open offer for bob to accept")
        else:
            check(c.has("v10mkAccept%d" % trid, 30), "bob: the incoming offer's Accept button")
            c.shot("offers")
            b_before = one("SELECT player_funds FROM players WHERE player_id = ?", B)
            c.click("v10mkAccept%d" % trid)
            ok = wait(lambda: one("SELECT count(*) FROM trades WHERE id = ? AND status = 'accepted'", trid), 20)
            check(ok, "bob accepted the offer")
            check(one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = 4", B) == 1, "bob got alice's item")
            check(one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = 3", A) == 1, "alice got bob's item")
            check(b_before >= 0, "bob's balance is sane (%d)" % b_before)
    finally:
        c.close()

    # ---- 6: the ledger matches every balance
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
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "market-e2e-%d" % os.getpid()},
                   "accounts": {"signup_rewards": {"enabled": False}, "sale_grace_s": 0, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0,
                                "market_min_level": 0, "trade_min_level": 0, "gift_min_level": 0,
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
        names = ["mktal%s" % stamp, "mktbo%s" % stamp]
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
    print("v10 market e2e (native): " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

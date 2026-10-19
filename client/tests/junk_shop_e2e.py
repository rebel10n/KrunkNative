#!/usr/bin/env python3
"""The Junk-priced shop and the lottery end to end (services/brain/shop.c, lottery.c; client
client/src/ui/junk_shop_ui.c, v10_misc_popups.c): kn_brain on a scratch sqlite database and a headless
client_bin (EGL, no window) signed in through it, driven with KRUNKNATIVE_UI_CMDS.

  python3 -I client/tests/junk_shop_e2e.py <build dir> [out dir]      (from the repository root)

The brain holds two Junk products on the "limited" display and the account has 10.00 Junk and 2 skins
of the lottery's ticket (7782). The client shows the Store's Eterno Shop with the products drawn as
cards (the screenshot), BUYs one (the Junk spent, the skins granted, the purchase row), then opens the
Anniversary raffle, which is open for the test (active), enters one bundle of the ticket (the entry row,
the ticket spent). Exit 77 when the binaries or python websockets are missing.
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

ROOT = os.getcwd()
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
OUT = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/.cache/kn-junk-shop-e2e"))
os.makedirs(OUT, exist_ok=True)
FAILS = []
JUNK = 1000  # 10.00 Junk
TICKET = 7782  # the Anniversary raffle's bundle (lottery 3, one entry)


def check(cond, what):
    print("%s  %s" % ("ok  " if cond else "FAIL", what), flush=True)
    if not cond:
        FAILS.append(what)


def wait_for(path, pattern, secs=20.0):
    end = time.time() + secs
    while time.time() < end:
        with open(path, errors="replace") as f:
            m = re.search(pattern, f.read())
        if m:
            return m
        time.sleep(0.2)
    return None


async def ws_call(port, frames):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        out = []
        for fr, label in frames:
            await ws.send(json.dumps(fr))
            while True:
                f = json.loads(await asyncio.wait_for(ws.recv(), 10))
                if f[0] == label:
                    out.append(f)
                    break
        return out


async def internal_ops(port, secret, ops):
    """The game servers' "acct" ops on /mm/internal/ws (as the brain tests do): [(op, args)] -> [(err, data)]."""
    async with websockets.connect("ws://127.0.0.1:%d/mm/internal/ws" % port,
                                  additional_headers={"Authorization": "Bearer " + secret}) as ws:
        await ws.send(json.dumps({"type": "hello", "nodeId": "JS-E2E", "region": "local", "publicHost": "127.0.0.1",
                                  "publicPort": 1, "version": "test"}))
        while json.loads(await asyncio.wait_for(ws.recv(), 10)).get("type") != "welcome":
            pass
        out = []
        for rid, (op, args) in enumerate(ops, 1):
            await ws.send(json.dumps({"type": "acct", "rid": rid, "op": op, "args": args}))
            while True:
                msg = json.loads(await asyncio.wait_for(ws.recv(), 15))
                if msg.get("type") == "acct-r" and msg.get("rid") == rid:
                    out.append((msg.get("err"), msg.get("data")))
                    break
        return out


def main():
    work = tempfile.mkdtemp(prefix="kn-junk-shop-e2e-", dir=os.path.expanduser("~/.cache"))
    db = os.path.join(work, "brain.db")
    cfg = {"database": {"driver": "sqlite", "path": db, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
           "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": "js-e2e-%d" % os.getpid(), "federation": False},
           "accounts": {"password_iterations": 10000, "starting_kr": 500, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}
    with open(os.path.join(work, "config.json"), "w") as f:
        json.dump(cfg, f)
    blog = os.path.join(OUT, "brain.log")
    brain = subprocess.Popen([os.path.join(BUILD, "services/brain/kn_brain"), "--config", os.path.join(work, "config.json"), "--migrate"],
                             cwd=ROOT, stdout=open(blog, "w"), stderr=subprocess.STDOUT)
    client = None
    try:
        port = int(wait_for(blog, r"listening on http://127\.0\.0\.1:(\d+) \(ws").group(1))
        name = "js%d" % (int(time.time()) % 100000)
        r = asyncio.run(ws_call(port, [(["a", 0, [name, "junkpass1", None], None], "a")]))[0]
        acc_id, token = r[2], r[5]

        con = sqlite3.connect(db)
        con.execute("INSERT INTO shop_products (display_tag, name, description, image_url, price_currency_id, price_amount,"
                    " purchase_limit, active, sort_order) VALUES ('limited', 'Junk Crate', 'Two skins for 2.50 Junk', '', 2, 250, NULL, 1, 1)")
        crate = con.execute("SELECT id FROM shop_products WHERE name = 'Junk Crate'").fetchone()[0]
        con.execute("INSERT INTO shop_product_contents (product_id, ord, content_type, skin_id, quantity) VALUES (?, 0, 'skin', 7438, 2)",
                    (crate,))
        con.execute("UPDATE lotteries SET active = 1 WHERE lottery_id = 3")
        con.commit()
        con.close()
        secret = cfg["brain"]["internal_secret"]
        granted = asyncio.run(internal_ops(port, secret, [("junk_credit", {"id": acc_id, "amount": JUNK, "reason": "trigger"}),
                                                          ("give_skin", {"id": acc_id, "skin": TICKET, "source": "test"}),
                                                          ("give_skin", {"id": acc_id, "skin": TICKET, "source": "test"})]))
        check(all(err is None for err, _ in granted), "the test grants 10.00 Junk and two tickets through the game ops: %s" % granted)

        xdg = os.path.join(work, "cfg")
        os.makedirs(os.path.join(xdg, "krunknative"), exist_ok=True)
        for k, v in (("krunker_username", name), ("krunker_token", token), ("krunker_id", str(acc_id))):
            with open(os.path.join(xdg, "krunknative", k + ".json"), "w") as f:
                f.write(v)
        cmds = os.path.join(work, "cmds.txt")
        open(cmds, "w").close()
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": xdg, "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port,
                    "KRUNKNATIVE_UI_CMDS": cmds, "KRUNKNATIVE_AUDIO_BACKEND": "null"})
        clog = os.path.join(OUT, "client.log")
        client = subprocess.Popen([os.path.join(BUILD, "client_bin"), "--offline"], cwd=ROOT, env=env,
                                  stdout=open(clog, "w"), stderr=subprocess.STDOUT)

        def cmd(line, wait=0.5):
            with open(cmds, "a") as f:
                f.write(line + "\n")
            time.sleep(wait)

        time.sleep(3)
        cmd("ui showWindow(14);windows[13].switchTab(5)", 3)  # the Store's Eterno Shop tab
        cmd("shot %s/1-eterno.png" % OUT, 1)
        cmd("ui knKioShopBuy(%d,%d)" % (1000000 + crate, crate), 1)  # BUY on the Junk Crate card
        m = wait_for(clog, r"\[junkshop\] bought product %d: junk (\S+)" % crate, 15)
        check(m is not None and m.group(1) == "7.50", "client: BUY on the Junk Crate charges 2.50 Junk (balance %r)" % (m and m.group(1)))
        cmd("shot %s/2-bought.png" % OUT, 1)
        check(not wait_for(clog, r"\[junkshop\] purchase of product", 0.1), "client: the purchase was not refused")

        cmd("ui openLottery(3)", 3)  # the raffle popup (v10_misc_popups.c)
        cmd("shot %s/3-lottery.png" % OUT, 1)
        cmd("ui knMiscLotQty(0,1)", 1)
        cmd("ui knMiscLotEnter()", 3)
        cmd("shot %s/4-entered.png" % OUT, 1)
        cmd("quit", 3)
        client.wait(timeout=30)

        con = sqlite3.connect(db)
        junk = con.execute("SELECT player_junk FROM players WHERE player_id = ?", (acc_id,)).fetchone()[0]
        granted = con.execute("SELECT COALESCE(SUM(delta), 0) FROM skin_transactions WHERE account_id = ? AND skin_id = 7438"
                              " AND source = 'shop'", (acc_id,)).fetchone()[0]
        purchases = con.execute("SELECT COUNT(*) FROM shop_purchases WHERE account_id = ? AND product_id = ?", (acc_id, crate)).fetchone()[0]
        entries = con.execute("SELECT COALESCE(SUM(e.entries), 0) FROM lottery_entries e JOIN lottery_draws d ON d.id = e.draw_id"
                              " WHERE e.account_id = ? AND d.lottery_id = 3", (acc_id,)).fetchone()[0]
        ticket_left = con.execute("SELECT COALESCE(SUM(delta), 0) FROM skin_transactions WHERE account_id = ? AND skin_id = ?",
                                  (acc_id, TICKET)).fetchone()[0]
        con.close()
        check(junk == JUNK - 250, "brain: the Junk spent (balance %r, expected 7.50)" % junk)
        check(granted == 2 and purchases == 1, "brain: two skins granted and one purchase row (%r, %r)" % (granted, purchases))
        check(entries == 1 and ticket_left == 1, "brain: the raffle entry written and the ticket spent (entries %r, tickets %r)" % (entries, ticket_left))
        for shot in ("1-eterno", "2-bought", "3-lottery", "4-entered"):
            p = os.path.join(OUT, shot + ".png")
            check(os.path.exists(p) and os.path.getsize(p) > 10000, shot + ".png written")
        check(not re.search(r"\[brain\].*(failed|ERROR)", open(blog, errors="replace").read()), "brain log: no statement errors")
    finally:
        if client and client.poll() is None:
            client.kill()
        brain.terminate()
        brain.wait(timeout=10)
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    if not os.path.exists(os.path.join(BUILD, "client_bin")) or not os.path.exists(os.path.join(BUILD, "services/brain/kn_brain")):
        print("junk_shop_e2e: client_bin or kn_brain not built: skipped")
        sys.exit(77)
    main()
    print("junk_shop_e2e: %s (screenshots in %s)" % ("FAILED: " + "; ".join(FAILS) if FAILS else "ok", OUT))
    sys.exit(1 if FAILS else 0)

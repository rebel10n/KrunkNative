#!/usr/bin/env python3
"""The economy through the web client's hub (the Emscripten build of client/src/ui/social_ui.c):
headless Chromium on dist/web against kn_brain (sqlite). Same flow as economy_hub_e2e.py, with
real DOM clicks and input events:

  1. alice on bob's profile: Send Gift 150 (fee 15) -> Trade: one item each side + 25 KR -> Submit
  2. bob: Market > Trading -> the offer -> Accept -> Inventory -> List Item 300
  3. alice: Market -> Purchase the listing -> KR History

  economy_web_e2e.py <native build dir> <repo root> [<web dist dir>]
Web dist: argument, else $KN_WEB_DIST. 77 = skip (no web build, Chromium, websockets or kn_brain).
Ports from kn_proto.free_ports(); the profile, database and screenshots live in ~/.cache.
"""
import asyncio
import base64
import functools
import http.server
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
DIST = os.path.abspath(sys.argv[3] if len(sys.argv) > 3 else os.environ.get("KN_WEB_DIST", ""))
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
CHROME = os.environ.get("KN_CHROME") or shutil.which("chromium-browser") or shutil.which("chromium") or shutil.which("google-chrome")
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
if not (DIST and os.path.exists(os.path.join(DIST, "client.wasm"))) or not CHROME or not os.path.exists(BRAIN):
    print("needs a web build (KN_WEB_DIST), Chromium and kn_brain: skip")
    sys.exit(77)
from kn_proto import free_ports  # noqa: E402

FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-econ-web.", dir=os.path.expanduser("~/.cache"))
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


def funds(aid):
    return one("SELECT player_funds FROM players WHERE player_id = ?", aid)


async def poll(fn, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        v = fn()
        if v:
            return v
        await asyncio.sleep(0.25)
    return fn()


class Cdp:
    def __init__(self, ws):
        self.ws, self.id, self.pending = ws, 0, {}

    @classmethod
    async def connect(cls, port):
        pages = []
        for _ in range(120):
            try:
                with urllib.request.urlopen("http://127.0.0.1:%d/json/list" % port, timeout=2) as r:
                    pages = [t for t in json.load(r) if t.get("type") == "page"]
                if pages:
                    break
            except OSError:
                pass
            await asyncio.sleep(0.5)
        c = cls(await websockets.connect(pages[0]["webSocketDebuggerUrl"], max_size=256 << 20))
        asyncio.get_event_loop().create_task(c.reader())
        return c

    async def reader(self):
        async for m in self.ws:
            msg = json.loads(m)
            if "id" in msg and msg["id"] in self.pending:
                self.pending.pop(msg["id"]).set_result(msg)

    async def send(self, method, **params):
        self.id += 1
        fut = asyncio.get_event_loop().create_future()
        self.pending[self.id] = fut
        await self.ws.send(json.dumps({"id": self.id, "method": method, "params": params}))
        msg = await asyncio.wait_for(fut, 60)
        if "error" in msg:
            raise RuntimeError("%s: %s" % (method, msg["error"]))
        return msg["result"]

    async def eval(self, expr):
        r = await self.send("Runtime.evaluate", expression=expr, returnByValue=True, awaitPromise=True, userGesture=True)
        if "exceptionDetails" in r:
            raise RuntimeError("eval %s: %s" % (expr[:80], r["exceptionDetails"]))
        return r["result"].get("value")

    async def wait(self, expr, timeout=30.0):
        end = time.time() + timeout
        while time.time() < end:
            try:
                v = await self.eval(expr)
                if v:
                    return v
            except RuntimeError:
                pass
            await asyncio.sleep(0.25)
        return None

    async def visible(self, sel, timeout=30.0):
        return await self.wait("(() => { const e = document.querySelector(%s); return !!e && e.getClientRects().length > 0; })()"
                               % json.dumps(sel), timeout)

    async def click(self, sel):
        await self.eval("document.querySelector(%s).click()" % json.dumps(sel))
        await asyncio.sleep(0.4)

    async def type(self, sel, value, event="input"):
        await self.eval("""(() => { const e = document.querySelector(%s); e.focus(); e.value = %s;
            e.dispatchEvent(new Event(%s, {bubbles: true})); })()""" % (json.dumps(sel), json.dumps(value), json.dumps(event)))
        await asyncio.sleep(0.4)

    async def shot(self, name):
        r = await self.send("Page.captureScreenshot", format="png")
        with open(os.path.join(WORK, name + ".png"), "wb") as f:
            f.write(base64.b64decode(r["data"]))


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port) as ws:
        await ws.send(json.dumps(["a", 0, [name, "web_pass_1", None]]))
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), 20))
            if f[0] == "a":
                assert f[1] is None, f
                return f[2], f[5]


async def open_hub(c, http_port, brain_port, name, token, query):
    """Log in as `name` (the saved session, as the menu leaves it) and open the hub at `query`."""
    await c.send("Page.navigate", url="http://127.0.0.1:%d/favicon.png" % http_port)
    await asyncio.sleep(1.0)
    await c.eval("localStorage.setItem('krunker_username', %s); localStorage.setItem('krunker_token', %s)" % (json.dumps(name), json.dumps(token)))
    await c.send("Page.navigate", url="http://127.0.0.1:%d/social.html?%s&brain=http://127.0.0.1:%d" % (http_port, query, brain_port))
    return await c.wait("(() => { const e = document.getElementById('profileKR'); return !!e && e.textContent.includes('KR'); })()", 180)


async def scenario(http_port, cdp_port, brain_port, ids, tokens, names):
    A, B = ids
    c = await Cdp.connect(cdp_port)
    await c.send("Runtime.enable")
    await c.send("Page.enable")
    await c.send("Emulation.setDeviceMetricsOverride", width=1280, height=720, deviceScaleFactor=1, mobile=False)

    # 1. alice: gift + offer
    check(await open_hub(c, http_port, brain_port, names[0], tokens[0], "p=profile&q=" + names[1]), "alice logged in on bob's profile")
    check(await c.visible("#giftBtn"), "Send Gift button")
    await asyncio.sleep(1.0)
    await c.click("#giftBtn")
    check(await c.visible("#giftIn"), "the gift popup")
    await c.type("#giftIn", "150")
    fee = await c.eval("document.getElementById('giftAmntFee').textContent")
    check(fee and "15 KR" in fee, "calcGiftFee: %r" % fee)
    await c.shot("web-gift")
    a0, b0 = funds(A), funds(B)
    await c.click("#postSaleBtn")
    ok = await poll(lambda: funds(B) == b0 + 150, 15)
    check(ok and funds(A) == a0 - 165, "gift: bob +150, alice -165")
    await c.wait("(() => { const p = document.getElementById('genericPop'); return p && p.textContent.includes('Success'); })()", 10)
    await c.click("#tradeBtn")
    check(await c.visible("#trd_inv_0_4") and await c.visible("#trd_inv_1_3"), "Trade: both inventories")
    await c.click("#trd_inv_0_4")
    await c.click("#trd_inv_1_3")
    check(await c.visible("#trd_0_4") and await c.visible("#trd_1_3"), "items on both trade lists")
    await c.type("#tradeKR0", "25")
    await c.shot("web-trade")
    await c.click("#trdSubBt")
    check(await c.visible("#confirmBtn"), "Submit Offer: confirmation")
    await c.click("#confirmBtn")
    trid = await poll(lambda: one("SELECT id FROM trades WHERE sender_id = ? AND status = 'open'", A), 15)
    check(trid and one("SELECT sender_kr FROM trades WHERE id = ?", trid) == 25 and funds(A) == a0 - 165 - 25 - 18,
          "offer sent: 25 KR escrowed, 18 KR fee (15 + the gift fee on 25 KR)")

    # 2. bob: accept, then list the item he got
    check(await open_hub(c, http_port, brain_port, names[1], tokens[1], "p=market&q=trades"), "bob logged in on Trading")
    check(await c.visible("#of_%d" % trid), "the incoming offer")
    await c.click("#of_%d" % trid)
    check(await c.visible(".tOfferBtn.accept"), "the offer popup")
    await c.shot("web-offer")
    await c.click(".tOfferBtn.accept")
    check(await c.visible("#confirmBtn"), "Accept: confirmation")
    b1 = funds(B)
    await c.click("#confirmBtn")
    ok = await poll(lambda: one("SELECT count(*) FROM trades WHERE id = ? AND status = 'accepted'", trid), 15)
    check(ok and funds(B) == b1 + 25 and one("SELECT count FROM inventory WHERE account_id = ? AND skin_id = 4", B) == 1, "accepted")
    await c.eval("document.getElementById('m_inventory') && document.getElementById('m_inventory').click()")
    check(await c.visible("#marketList .marketCard"), "bob's inventory")
    await c.eval("[...document.querySelectorAll('#marketList .cardAction')].find(a => a.textContent === 'List').click()")
    check(await c.visible("#sellPriceIn"), "List popup")
    await c.type("#sellPriceIn", "300")
    fee = await c.eval("document.getElementById('sellPricFee').textContent")
    check(fee and "30 KR" in fee, "calcFee: %r" % fee)
    await c.click("#postSaleBtn")
    check(await c.visible("#confirmBtn"), "List: confirmation")
    b2 = funds(B)
    await c.click("#confirmBtn")
    lid = await poll(lambda: one("SELECT id FROM market_listings WHERE seller_id = ? AND sold_at IS NULL AND cancelled_at IS NULL", B), 15)
    check(lid and funds(B) == b2 - 30, "listed for 300, fee 30")

    # 3. alice buys it (sale_grace_s 0 in the test), then KR History
    check(await open_hub(c, http_port, brain_port, names[0], tokens[0], "p=market"), "alice on the market")
    check(await c.visible("#marketList .marketCard"), "the listing's card")
    await c.eval("[...document.querySelectorAll('#marketList .cardAction')].find(a => a.textContent === 'Purchase').click()")
    check(await c.visible("#popupContent .pItemButton"), "Purchase popup")
    await c.shot("web-buy")
    a1, b3 = funds(A), funds(B)
    await c.click("#popupContent .pItemButton")
    ok = await poll(lambda: one("SELECT count(*) FROM market_listings WHERE id = ? AND buyer_id = ?", lid, A), 15)
    check(ok and funds(A) == a1 - 300 and funds(B) == b3 + 300, "bought")
    await c.wait("(() => { const p = document.getElementById('genericPop'); return p && p.textContent.includes('Success'); })()", 10)
    await c.click("#profileKR")
    check(await c.wait("document.getElementById('genericPop').textContent.includes('KR History') && "
                       "document.getElementById('genericPop').textContent.includes('Market purchase')", 15), "KR History popup")
    await c.shot("web-kr-history")
    await c.ws.close()
    for aid in ids:
        check(funds(aid) == one("SELECT coalesce(sum(amount), 0) FROM kr_transactions WHERE account_id = ?", aid), "ledger of %d" % aid)


def main():
    http_port, cdp_port, brain_port = free_ports(3)

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *a):
            pass
    httpd = http.server.ThreadingHTTPServer(("127.0.0.1", http_port), functools.partial(Quiet, directory=DIST))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    cfg = os.path.join(WORK, "brain.json")
    with open(cfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": DB, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
                   "brain": {"host": "127.0.0.1", "port": brain_port, "federation": False, "internal_secret": "web-e2e-%d" % os.getpid()},
                   "accounts": {"signup_rewards": {"enabled": False},  # v10 signup perks off: these count inventories
                    "sale_grace_s": 0, "free_premium": False, "starting_kr": 0, "login_throttle_s": 0,
                                "password_iterations": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    log = open(os.path.join(WORK, "brain.log"), "w")
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    chrome = subprocess.Popen([CHROME, "--headless=new", "--no-sandbox", "--use-gl=angle", "--use-angle=swiftshader",
                               "--enable-unsafe-swiftshader", "--mute-audio", "--window-size=1280,720",
                               "--remote-debugging-port=%d" % cdp_port, "--user-data-dir=" + os.path.join(WORK, "chrome"), "about:blank"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        for _ in range(200):
            with open(os.path.join(WORK, "brain.log")) as f:
                if re.search(r"listening on http://127\.0\.0\.1:%d" % brain_port, f.read()):
                    break
            time.sleep(0.1)
        stamp = str(int(time.time()) % 100000)
        names = ["webal%s" % stamp, "webbo%s" % stamp]
        ids, tokens = [], []
        for n in names:
            i, t = asyncio.run(register(brain_port, n))
            ids.append(i)
            tokens.append(t)
        for aid in ids:
            q("UPDATE players SET player_score = 2000000, player_premium = 0, player_funds = 5000 WHERE player_id = ?", aid)
            q("INSERT INTO kr_transactions (account_id, amount, source) VALUES (?, 5000, 'admin')", aid)
        q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 4, 2)", ids[0])
        q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 3, 1)", ids[1])
        asyncio.run(asyncio.wait_for(scenario(http_port, cdp_port, brain_port, ids, tokens, names), 500))
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        check(False, "exception: %r" % e)
    finally:
        for p in (chrome, brain):
            p.terminate()
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        httpd.shutdown()
        log.close()
    print("economy web e2e: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

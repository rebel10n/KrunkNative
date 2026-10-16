#!/usr/bin/env python3
"""Krunker v10.0.2 Wild Offers on kn_brain (services/brain/wild_offer.c): the social socket's
["wo"] (shop.getWildOffer), ["wob", id] (purchaseWildOffer) and ["wor", id] (refreshWildOffer) on a
scratch sqlite database: the day's offer is v10's shape and stable, a refresh costs refresh_cost KR
and brings the next one, a purchase charges the price and grants the skin once (then no offer
until the next day or refresh), a stale id and too little KR are refused.

  wild_offer_test.py <kn_brain> <repo root>
77 = skip (python websockets missing).
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

BRAIN, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
FAILS = []
DAY_MS = 86400000


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def wait_port(log, proc):
    for _ in range(300):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log)
        m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", open(log, errors="replace").read())
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


class Sock:
    def __init__(self, port):
        self.port = port

    async def __aenter__(self):
        self.ws = await websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % self.port)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def call(self, frame, label):
        await self.ws.send(json.dumps(frame))
        while True:
            f = json.loads(await asyncio.wait_for(self.ws.recv(), 10))
            if f[0] == label:
                return f


async def run(port, db):
    def funds_of(acc):
        con = sqlite3.connect(db)
        try:
            return con.execute("SELECT player_funds FROM players WHERE player_id = ?", (acc,)).fetchone()[0]
        finally:
            con.close()

    def set_funds(acc, v):
        con = sqlite3.connect(db)
        con.execute("UPDATE players SET player_funds = ? WHERE player_id = ?", (v, acc))
        con.commit()
        con.close()

    def owned(acc, skin):
        con = sqlite3.connect(db)
        try:
            return con.execute("SELECT coalesce(sum(delta), 0) FROM skin_transactions WHERE account_id = ? AND skin_id = ?", (acc, skin)).fetchone()[0]
        finally:
            con.close()

    async with Sock(port) as s:
        name = "wo%d" % (int(time.time()) % 100000)
        a = await s.call(["a", 0, [name, "wildpass1", None], None], "a")
        acc = a[2]
        check(isinstance(acc, int) and acc > 0, "account registered (%r)" % acc)
        o = (await s.call(["wo"], "wo"))[1]
        check(isinstance(o, dict), "[\"wo\"]: an offer (%r)" % (o and o.get("id")))
        day = int(time.time() * 1000) // DAY_MS
        check(o["id"] == day * 100, "offer id: the UTC day's first (%r)" % o["id"])
        check(o["status"] is None and o["price_type"] == "virtual", "status null, price_type virtual")
        check(re.fullmatch(r"\d{4}-\d\d-\d\d 00:00:00", o["expires_at"]) is not None, "expires_at at the day's end, UTC (%r)" % o["expires_at"])
        p, op = o["price"]["virtual"], o["original_price"]["virtual"]
        check(p["currency"] == "KR" and op["currency"] == "KR", "KR prices")
        check(o["discount_pct"] in range(25, 61, 5) and p["amount"] == (op["amount"] * (100 - o["discount_pct"]) + 50) // 100,
              "price = original - discount_pct %% (%r of %r, -%r%%)" % (p["amount"], op["amount"], o["discount_pct"]))
        check(op["amount"] in (1500, 4000, 10000, 25000), "original price by rarity (%r)" % op["amount"])
        c = o["contents"]
        check(len(c) == 1 and c[0]["type"] == "skin" and c[0]["ref"].isdigit(), "contents: one skin (%r)" % c)
        check(o["refresh_cost"] == 300, "refresh_cost 300")
        o2 = (await s.call(["wo"], "wo"))[1]
        check(o2 == o, "the same offer when asked again")

        # refresh: a stale id is refused, the current one costs 300 KR and brings the next offer
        f0 = funds_of(acc)
        r = await s.call(["wor", o["id"] + 1], "wor")
        check(r[1] is None and "expired" in (r[2] or ""), "refresh of another id refused (%r)" % r[2:])
        r = await s.call(["wor", o["id"]], "wor")
        check(isinstance(r[1], dict) and r[1]["success"] and r[1]["refresh_cost"] == 300, "refresh ok, cost 300")
        n = r[1]["offer"]
        check(n["id"] == o["id"] + 1, "the next offer (%r)" % n["id"])
        check(r[1]["funds"] == f0 - 300 and funds_of(acc) == f0 - 300, "300 KR charged (%r -> %r)" % (f0, funds_of(acc)))
        check((await s.call(["wo"], "wo"))[1] == n, "[\"wo\"] now returns the refreshed offer")
        r = await s.call(["wob", o["id"]], "wob")
        check(r[1] is None, "the old offer can no longer be bought")

        # purchase: too little KR is refused (nothing granted), then bought once
        skin = int(n["contents"][0]["ref"])
        price = n["price"]["virtual"]["amount"]
        set_funds(acc, price - 1)
        r = await s.call(["wob", n["id"]], "wob")
        check(r[1] is None and "Not enough KR" in (r[2] or ""), "too little KR refused (%r)" % r[2:])
        check(owned(acc, skin) == 0 and funds_of(acc) == price - 1, "nothing charged or granted")
        set_funds(acc, price + 5)
        r = await s.call(["wob", n["id"]], "wob")
        check(isinstance(r[1], dict) and r[1]["success"] and r[1]["rewards"] == [{"type": "skin", "skin_id": skin}],
              "bought: rewards [{type skin, skin_id %d}] (%r)" % (skin, r[1]))
        check(r[1]["funds"] == 5 and funds_of(acc) == 5, "the price charged (funds %r)" % funds_of(acc))
        check(owned(acc, skin) == 1, "the skin granted")
        check((await s.call(["wo"], "wo"))[1] is None, "no offer after the purchase (until the next day or refresh)")
        r = await s.call(["wob", n["id"]], "wob")
        check(r[1] is None and owned(acc, skin) == 1, "a second purchase refused")
        r = await s.call(["wor", n["id"]], "wor")
        check(r[1] is None, "a bought offer cannot be refreshed")
        r = await s.call(["wob", "x"], "wob")
        check(r[1] is None and r[2] == "Invalid wild offer", "a non-numeric id refused")


def main():
    work = tempfile.mkdtemp(prefix="kn-wild-offer-test-", dir=os.path.expanduser("~/.cache"))
    db = os.path.join(work, "brain.db")
    cfg = {"database": {"driver": "sqlite", "path": db, "migrations_dir": os.path.join(ROOT, "services", "db", "migrations")},
           "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": "wo-%d" % os.getpid(), "federation": False},
           "accounts": {"password_iterations": 10000, "starting_kr": 1000, "skins_file": os.path.join(ROOT, "assets", "skins.json")}}
    with open(os.path.join(work, "config.json"), "w") as f:
        json.dump(cfg, f)
    log = os.path.join(work, "brain.log")
    brain = subprocess.Popen([BRAIN, "--config", os.path.join(work, "config.json"), "--migrate"], cwd=ROOT,
                             stdout=open(log, "w"), stderr=subprocess.STDOUT)
    try:
        asyncio.run(run(wait_port(log, brain), db))
    finally:
        brain.terminate()
        try:
            brain.wait(timeout=10)
        except subprocess.TimeoutExpired:
            brain.kill()
    if FAILS:
        print("wild_offer_test: FAILED (%d): %s (log %s)" % (len(FAILS), "; ".join(FAILS), log))
        sys.exit(1)
    shutil.rmtree(work, ignore_errors=True)
    print("wild_offer_test: ok")


if __name__ == "__main__":
    main()

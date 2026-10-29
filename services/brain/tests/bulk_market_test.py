#!/usr/bin/env python3
"""v10.0.4's bulk market actions and item locks for a Box Office account, on the social socket (services/brain/economy.c
"qsb" / "lib" / "ilk" / "ils", db_economy.c run_quick_sell_bulk / run_market_list_bulk / run_item_locks / run_item_lock,
migration 0364_item_locks), as krunker.io's market/quick-sell/bulk, market/list/bulk and items/locks
(deob-10.0.4.js:146851-146866, :131978-132019, :108837-108862).

  bulk_market_test.py <kn_brain> <repo root>
env: KN_TEST_DB_DRIVER as economy_test.py (its harness: the scratch database, the socket, the setup helpers). 77 = skip.

Covers: the bulk quick sell for KR / Junk / destroy (a stack sells all its unpainted units, a painted copy one unit, a unit
the action does not take or that is not owned is skipped), one transaction and one key per frame (a replayed frame moves
nothing twice), the bulk list (the fee per listing, escrow, a painted copy keeps its paint, a row's own error does not stop
the others, the listing limit), the item locks (get / set, a locked unit is skipped by the bulk sell, refused by the single
quick sell, the list and a trade offer), and the KR ledger.
"""
import asyncio
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location("economy_test", os.path.join(HERE, "economy_test.py"))
E = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(E)  # its argv is ours: <kn_brain> <repo root>

check, give, funds, junk, count, set_player, op = E.check, E.give, E.funds, E.junk, E.count, E.set_player, E.op
BRAIN, ROOT, DRIVER = E.BRAIN, E.ROOT, E.DRIVER
NAME = "kn_test_bulk_%d" % os.getpid()
STAMP = E.STAMP
COPY_BASE = 1000000
KR10 = 10       # Acid Breath, rarity 3: 100 KR each, no scrap
JUNK114 = 114   # Flame Tamer, contraband: 2500 KR or 5.00 Junk each
DESTROY = 1905  # Rune Lord: noQuickSell, destroy only
LIST4 = 4       # Moon Dragon, rarity 3


def add_copy(db, aid, skin, copy_id, colour):
    give(db, aid, skin, 1)
    # a fixed copy_id: PostgreSQL's copy_id is GENERATED ALWAYS AS IDENTITY, which takes one only with OVERRIDING SYSTEM VALUE
    over = " OVERRIDING SYSTEM VALUE" if DRIVER == "postgres" else ""
    db.q("INSERT INTO skin_copies (copy_id, account_id, skin_id, paint_color)%s VALUES ($1, $2, $3, $4)" % over,
         copy_id, aid, skin, colour)


async def scenario(url, db):
    names = {k: "%s%s" % (k, STAMP) for k in ("ada", "ben")}
    ids = {k: await E.register(url, n) for k, n in names.items()}
    A, B = ids["ada"], ids["ben"]
    for aid in ids.values():
        set_player(db, aid, funds=10000, level=40, premium=0)
    a = await E.user(url, names["ada"])
    try:
        # ---- bulk quick sell for KR: a stack's unpainted units, a painted copy as one unit, a Junk-only item skipped
        give(db, A, KR10, 3)
        add_copy(db, A, KR10, COPY_BASE + 101, "#ff0000")
        give(db, A, JUNK114, 1)
        f0 = funds(db, A)
        key = op()
        r = await a.call("qsbr", "qsb", 0, [[KR10, KR10], [KR10, COPY_BASE + 101], [9999, 9999]], 7, key)
        res = r[1] or {}
        check(r[2] == 7 and sorted(res.get("sold", [])) == sorted([KR10, COPY_BASE + 101]) and res.get("skipped") == [9999],
              "bulk sell for KR: the stack and the copy sold, an unowned unit skipped: %r" % (r,))
        check(res.get("funds") == f0 + 400 and funds(db, A) == f0 + 400 and count(db, A, KR10) == 0
              and db.one("SELECT count(*) FROM skin_copies WHERE copy_id = $1", COPY_BASE + 101) == 0,
              "3 stack units + 1 copy at 100 KR, the copy row gone: %r" % (res,))
        r = await a.call("qsbr", "qsb", 0, [[KR10, KR10]], 8, key)
        check(r[1] and r[1].get("funds") == f0 + 400 and funds(db, A) == f0 + 400, "a replayed frame moves nothing twice: %r" % (r,))
        # ---- for Junk; then destroy (only noQuickSell items)
        j0 = junk(db, A)
        r = await a.call("qsbr", "qsb", 1, [[JUNK114, JUNK114]], 9, op())
        check(r[1] and r[1].get("sold") == [JUNK114] and junk(db, A) == j0 + 500 and abs(r[1]["junk"] - (j0 + 500) / 100.0) < 1e-9
              and funds(db, A) == f0 + 400, "bulk sell for Junk: 5.00 Junk, no KR: %r" % (r,))
        give(db, A, DESTROY, 2)
        give(db, A, LIST4, 1)
        f1 = funds(db, A)
        r = await a.call("qsbr", "qsb", 2, [[DESTROY, DESTROY], [LIST4, LIST4]], 10, op())
        check(r[1] and r[1].get("sold") == [DESTROY] and r[1].get("skipped") == [LIST4] and count(db, A, DESTROY) == 0
              and count(db, A, LIST4) == 1 and funds(db, A) == f1, "destroy takes the noQuickSell item only, pays nothing: %r" % (r,))
        give(db, A, KR10, 5)  # the Keep options' per-item count: a stack keeps 2 of its 5 units
        f_keep = funds(db, A)
        r = await a.call("qsbr", "qsb", 0, [[KR10, KR10, 2]], 12, op())
        check(r[1] and r[1].get("sold") == [KR10] and count(db, A, KR10) == 2 and funds(db, A) == f_keep + 300,
              "a stack's keep count stays (5 units, keep 2: 3 sold for 300 KR): %r" % (r,))
        r = await a.call("qsbr", "qsb", 0, [[KR10, KR10, 2]], 13, op())
        check(r[1] and r[1].get("skipped") == [KR10] and count(db, A, KR10) == 2, "nothing above the keep count: skipped: %r" % (r,))
        db.q("UPDATE inventory SET count = 0 WHERE account_id = $1 AND skin_id = $2", A, KR10)
        r = await a.call("qsbr", "qsb", 5, [[LIST4, LIST4]], 11, op())
        check(r[1] is None and r[3] == "Invalid request", "an unknown action is refused: %r" % (r,))

        # ---- item locks: set, read, and every path refuses the locked unit
        give(db, A, KR10, 2)
        add_copy(db, A, LIST4, COPY_BASE + 202, "#00ff00")
        r = await a.call("ilsr", "ils", KR10, 0, 1, 21)
        check(r[1] == {"locked": True} and r[2] == 21, "lock a stack: %r" % (r,))
        r = await a.call("ilsr", "ils", LIST4, COPY_BASE + 202, 1, 22)
        check(r[1] == {"locked": True}, "lock a copy: %r" % (r,))
        r = await a.call("ilsr", "ils", 4242, 0, 1, 23)
        check(r[1] is None and r[3] == "Item not owned", "an item the account lacks can't be locked: %r" % (r,))
        r = await a.call("ilkr", "ilk")
        locks = sorted((x["skin_id"], x["item_id"], x["stack"]) for x in (r[1] or {}).get("locks", []))
        check(locks == sorted([(KR10, KR10, True), (COPY_BASE + 202, LIST4, False)]), "the locks as krunker.io's rows: %r" % (r,))
        f2 = funds(db, A)
        r = await a.call("qsbr", "qsb", 0, [[KR10, KR10], [LIST4, COPY_BASE + 202]], 24, op())
        check(r[1] and r[1].get("sold") == [] and sorted(r[1].get("skipped")) == sorted([KR10, COPY_BASE + 202])
              and count(db, A, KR10) == 2 and funds(db, A) == f2, "the bulk sell skips locked units: %r" % (r,))
        r = await a.call(("qsr", "po"), "qsi", 0, KR10, 25, 0, op())
        check(r[0] == "qsr" and r[1] is None and count(db, A, KR10) == 2, "the single quick sell refuses a locked stack (qsr null): %r" % (r,))
        r = await a.call(("lir", "po"), "li", LIST4, COPY_BASE + 202, 26, 500, op())
        check(E.po_text(r) == "That item is locked.", "the single list refuses a locked copy: %r" % (r,))
        r = await a.call(("trR", "po"), "ctr", B, [{"i": KR10, "c": 1}], [], "x", op())
        check(E.po_text(r) == "That item is locked." and count(db, A, KR10) == 2, "a trade offer refuses a locked stack: %r" % (r,))
        r = await a.call("ilsr", "ils", KR10, 0, 0, 27)
        check(r[1] == {"locked": False}, "unlock the stack: %r" % (r,))
        r = await a.call("ilkr", "ilk")
        check([x["skin_id"] for x in r[1]["locks"]] == [COPY_BASE + 202], "one lock left: %r" % (r,))

        # ---- bulk list: per-row errors, the fee per listing (10%, floor), escrow, a copy keeps its paint
        give(db, A, LIST4, 3)  # 4 unpainted units (1 from earlier) + the locked copy
        add_copy(db, A, LIST4, COPY_BASE + 203, "#0000ff")
        f3 = funds(db, A)
        rows = [[LIST4, LIST4, 155, 2], [LIST4, COPY_BASE + 203, 300, 1], [LIST4, COPY_BASE + 202, 300, 1], [KR10, KR10, 0, 1],
                [7777, 7777, 100, 1]]
        r = await a.call("libr", "lib", rows, 31, op())
        res = r[1] or {}
        errs = [x.get("error") for x in res.get("results", [])]
        check(r[2] == 31 and res.get("listed") == 3 and errs == [None, None, "That item is locked.", "Invalid price", "Item not owned"],
              "bulk list: 2 stack units + 1 copy listed, a locked copy / bad price / unowned row refused alone: %r" % (r,))
        check(res.get("funds") == f3 - 15 - 15 - 30 and funds(db, A) == f3 - 60, "a fee per listing, floor(10%%): %r" % (res,))
        check(db.q("SELECT paint_color FROM market_listings WHERE seller_id = $1 AND paint_color IS NOT NULL", A) == [("#0000ff",)]
              and db.one("SELECT count(*) FROM skin_copies WHERE copy_id = $1", COPY_BASE + 203) == 0,
              "the copy's listing keeps its paint; the copy is in escrow")
        bad = E.ledger_ok(db, [A, B])
        check(not bad, "the KR ledger sums to the balances: %r" % (bad,))
        r = await a.call("libr", "lib", [[LIST4, LIST4, 100, 101]], 32, op())
        check(r[1] is None and r[3] == "Invalid request", "more than 100 units a frame is refused: %r" % (r,))
    finally:
        await a.__aexit__()


async def limit(url, db):
    """The listing limit (max_listings 5 in this config): the rows after it say Max Items on Sale."""
    name = "lim%s" % STAMP
    aid = await E.register(url, name)
    set_player(db, aid, funds=10000, level=40, premium=0)
    give(db, aid, LIST4, 8)
    s = await E.user(url, name)
    try:
        r = await s.call("libr", "lib", [[LIST4, LIST4, 100, 4], [LIST4, LIST4, 100, 3]], 41, op())
        res = r[1] or {}
        check(res.get("listed") == 5 and [x.get("error") for x in res.get("results", [])] == [None, "Max Items on Sale"]
              and count(db, aid, LIST4) == 3, "the listing limit stops the second row after one unit: %r" % (r,))
    finally:
        await s.__aexit__()


def main():
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("%s unavailable (%s): skip" % (DRIVER, r.stderr.decode(errors="replace").strip()))
        return r.returncode
    dbcfg = json.loads(r.stdout)
    tmp = tempfile.mkdtemp(prefix="kn-bulk-test-", dir=os.path.expanduser("~/.cache"))
    cfg = {
        "database": dict(dbcfg, pool_size=8, migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
        "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": "bulk-%d" % os.getpid(), "federation": False},
        "accounts": {"signup_rewards": {"enabled": False}, "password_iterations": 1000, "login_throttle_s": 0, "starting_kr": 0,
                     "free_premium": False, "skins_file": os.path.join(ROOT, "assets", "skins.json"), "max_listings": 5,
                     "economy_rate_qsell": 1000, "economy_rate_list": 1000, "economy_rate_trade": 1000},
    }
    with open(os.path.join(tmp, "config.json"), "w") as fp:
        json.dump(cfg, fp)
    log_path = os.path.join(tmp, "brain.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([BRAIN, "--config", os.path.join(tmp, "config.json"), "--migrate"], stdout=log, stderr=subprocess.STDOUT,
                            cwd=ROOT, start_new_session=True)
    rc = 1
    try:
        port = None
        for _ in range(1200):
            if proc.poll() is not None:
                raise RuntimeError("kn_brain exited early")
            with open(log_path) as fp:
                m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read())
            if m:
                port = int(m.group(1))
                break
            time.sleep(0.05)
        if not port:
            raise RuntimeError("kn_brain did not start listening")
        url = "ws://127.0.0.1:%d/social/ws" % port
        db = E.Db(dbcfg)

        async def run():
            await scenario(url, db)
            await limit(url, db)

        asyncio.run(asyncio.wait_for(run(), 300))
        rc = 1 if E.FAILS else 0
    except Exception as e:  # noqa: BLE001
        import traceback
        traceback.print_exc()
        print("FAIL exception: %r" % (e,))
        rc = 1
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        with open(log_path) as fp:
            text = fp.read()
        errs = [l for l in text.splitlines() if " ERROR " in l or "deadlock" in l.lower()]
        if errs:
            print("brain log errors:\n" + "\n".join(errs[:20]))
        if rc:
            print("---- brain log tail (%s) ----\n%s" % (log_path, text[-4000:]))
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        if not rc:
            for f in os.listdir(tmp):
                os.unlink(os.path.join(tmp, f))
            os.rmdir(tmp)
    print("bulk market test (%s): %s" % (DRIVER, "OK" if not rc else "%d failure(s)" % len(E.FAILS)))
    return rc


if __name__ == "__main__":
    sys.exit(main())

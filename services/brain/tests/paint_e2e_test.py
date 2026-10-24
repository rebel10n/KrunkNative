#!/usr/bin/env python3
"""The per-copy paint, end to end through kn_brain's HTTP routes (doc/v10/paint-copy.md; services/brain/v10_market.c,
db_economy.c run_item_paint, db_common.c db_owned_skins).

  paint_e2e_test.py <kn_brain | build dir> [repo root]
  (CTest passes the kn_brain binary and the source root.)

A scratch database (db_scratch.py; sqlite only: the postgres and mysql seeding is not written here), kn_brain
--migrate-only (0352_skin_copies), then kn_brain on an ephemeral port. A stub account registers over
POST /auth/register and is given two units of one skin and one paint, through sqlite (the owned units; the
route does the rest). Then, over HTTP only:

  1. POST /items/{id}/paint on an unpainted unit: the reply names a copy (id >= 1000000) painted with the paint.
  2. GET /market/inventory: the painted copy is its own row (sid = the copy id, m = {paint}, no cnt); the second
     unit of the skin is the stack row with cnt 1 and no paint.
  3. GET /player/me: the owned list (data[10], the loadout's source) has the painted copy with its id and m, and
     the second unit as an unpainted {ind, cnt} row.
  4. The database agrees: one skin_copies row with the colour, inventory.count still 2, the paint consumed.
  5. A quick sell "all" of the skin sells the stack's unpainted unit only (the official stack row counts its unpainted
     units: game.js 147652-147722); the painted copy is not sold.
  6. The paint's last unit is gone: a second paint of the same kind is refused (400).

Exit 0 when every check passes, 1 otherwise, 77 when kn_brain or the sqlite scratch database is unavailable.
"""
import json
import os
import re
import shutil
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ARG = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BRAIN = os.path.join(ARG, "services", "brain", "kn_brain") if os.path.isdir(ARG) else ARG
NAME = "kn_test_paint_e2e_%d" % os.getpid()
SECRET = "kn-paint-e2e-secret-%d" % os.getpid()
PASSWORD = "paintpass1"
OWNER = "paint_owner"  # at most 14 characters: a new name's cap (v10.0.3 maxNameLength, deob-10.0.3.js:22725)
SKIN = 0            # a tradable skin in assets/skins.json (the market test's ITEM)
PAINT = 7776        # a colour paint, #dc3c3c (v10_paint.c)
PAINT_HEX = "#dc3c3c"
COPY_BASE = 1000000  # V10_COPY_ID_BASE
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(bool(ok))
    print(("PASS  " if ok else "FAIL  ") + name + ("" if ok or not detail else "  -- " + detail), flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.1)
    return None


def http(port, method, path, body=None, token=None):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data is not None else {}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            status, raw = r.status, r.read()
    except urllib.error.HTTPError as e:
        status, raw = e.code, e.read()
    try:
        return status, json.loads(raw or b"null")
    except ValueError:
        return status, None


def err_of(reply):
    return reply.get("error") if isinstance(reply, dict) and isinstance(reply.get("error"), dict) else {}


def data_of(reply):
    return reply.get("data") if isinstance(reply, dict) else None


def query(path, sql, args=()):
    con = sqlite3.connect(path, timeout=30)
    try:
        return con.execute(sql, args).fetchall()
    finally:
        con.close()


def seed(path, stmts):
    con = sqlite3.connect(path, timeout=30)
    try:
        for sql, args in stmts:
            con.execute(sql, args)
        con.commit()
    finally:
        con.close()


def market_rows(port, token):
    status, reply = http(port, "GET", "/market/inventory", None, token)
    rows = data_of(reply)
    return status, rows if isinstance(rows, list) else []


def owned_list(port, token):
    status, reply = http(port, "GET", "/player/me", None, token)
    acc = data_of(reply) or {}
    data = acc.get("data") if isinstance(acc, dict) else None
    skins = data[10] if isinstance(data, list) and len(data) > 10 else None
    return status, skins if isinstance(skins, list) else []


def stop(proc):
    if proc.poll() is None:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except OSError:
            proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()


def scenario(port, db):
    status, reply = http(port, "POST", "/auth/register", {"username": OWNER, "password": PASSWORD})
    token = reply.get("token") if isinstance(reply, dict) else None
    check("POST /auth/register: the stub account", status == 200 and bool(token), "HTTP %d" % status)
    if not token:
        return
    acc = query(db, "SELECT id FROM accounts WHERE username = ?", (OWNER,))
    acc_id = acc[0][0] if acc else 0
    # the stub's holdings: two units of the skin and one paint unit (the owned units; no copy yet)
    seed(db, [
        ("UPDATE players SET player_funds = 1000 WHERE player_id = ?", (acc_id,)),
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 2)", (acc_id, SKIN)),
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", (acc_id, PAINT)),
    ])

    st, rows = market_rows(port, token)
    skin_rows = [x for x in rows if x.get("i") == SKIN]
    check("before the paint: the two units are one unpainted stack (cnt 2, sid = store index)",
          st == 200 and len(skin_rows) == 1 and skin_rows[0].get("cnt") == 2 and skin_rows[0].get("sid") == SKIN
          and "m" not in skin_rows[0],
          "HTTP %d %s" % (st, json.dumps(rows)[:200]))

    # 1. paint one unpainted unit: it becomes a copy
    st, reply = http(port, "POST", "/items/%d/paint" % SKIN, {"item_id": SKIN, "paint_item_id": PAINT}, token)
    d = data_of(reply) or {}
    copy_id = d.get("copy_id")
    check("POST /items/{id}/paint: an unpainted unit becomes a copy with the paint (m, paints left 0)",
          st == 200 and isinstance(copy_id, int) and copy_id >= COPY_BASE and d.get("m") == {"paint": PAINT_HEX}
          and d.get("count") == 0 and d.get("item_id") == SKIN, "HTTP %d %s" % (st, json.dumps(reply)[:200]))
    if not isinstance(copy_id, int):
        return

    # 2. the market inventory: the painted copy is its own row; the second unit stays unpainted
    st, rows = market_rows(port, token)
    copy_rows = [x for x in rows if x.get("sid") == copy_id]
    stack_rows = [x for x in rows if x.get("i") == SKIN and "m" not in x and x.get("sid") == SKIN]
    check("GET /market/inventory: the painted copy row is i = skin, sid = copy id, m = {paint}, no cnt",
          st == 200 and len(copy_rows) == 1 and copy_rows[0].get("i") == SKIN and copy_rows[0].get("m") == {"paint": PAINT_HEX}
          and "cnt" not in copy_rows[0], json.dumps(rows)[:300])
    check("GET /market/inventory: the second unit is the unpainted stack (cnt 1, no m)",
          len(stack_rows) == 1 and stack_rows[0].get("cnt") == 1, json.dumps(rows)[:300])

    # 3. the owned list (the loadout's source, data[10]): the copy with its id and m, the second unit unpainted
    st, owned = owned_list(port, token)
    painted = [x for x in owned if x.get("id") == copy_id]
    plain = [x for x in owned if x.get("ind") == SKIN and "id" not in x]
    check("GET /player/me data[10]: the painted copy is {ind, cnt 1, id, m: {paint}}",
          st == 200 and len(painted) == 1 and painted[0].get("ind") == SKIN and painted[0].get("cnt") == 1
          and painted[0].get("m") == {"paint": PAINT_HEX}, "HTTP %d %s" % (st, json.dumps(owned)[:300]))
    check("GET /player/me data[10]: the second unit is an unpainted {ind, cnt 1} row",
          len(plain) == 1 and plain[0].get("cnt") == 1 and "m" not in plain[0], json.dumps(owned)[:300])

    # 4. the database: one painted copy row, the units and the paint as the route left them
    copies = query(db, "SELECT copy_id, paint_color, paint_shader FROM skin_copies WHERE account_id = ? AND skin_id = ?",
                   (acc_id, SKIN))
    check("skin_copies: one row for the copy, colour set, shader NULL",
          len(copies) == 1 and copies[0][0] == copy_id and copies[0][1] == PAINT_HEX and copies[0][2] is None,
          repr(copies))
    count = query(db, "SELECT count FROM inventory WHERE account_id = ? AND skin_id = ?", (acc_id, SKIN))
    paint_left = query(db, "SELECT count FROM inventory WHERE account_id = ? AND skin_id = ?", (acc_id, PAINT))
    check("inventory: the skin still counts both units; the paint is consumed (0 left)",
          count and count[0][0] == 2 and paint_left and paint_left[0][0] == 0, "skin %r paint %r" % (count, paint_left))

    # 5. a sale "all" takes the stack's unpainted unit only: the painted copy is not sold
    st, reply = http(port, "POST", "/market/quick-sell", {"item_id": SKIN, "action": 2, "amount": 2}, token)
    count = query(db, "SELECT count FROM inventory WHERE account_id = ? AND skin_id = ?", (acc_id, SKIN))
    check("quick sell of all sells the unpainted unit only; the painted copy stays",
          st == 200 and (reply.get("data") or {}).get("sold") == 1 and count and count[0][0] == 1
          and len(query(db, "SELECT copy_id FROM skin_copies WHERE copy_id = ?", (copy_id,))) == 1,
          "HTTP %d %s" % (st, json.dumps(reply)[:200]))

    # 6. no paint left: another paint of the same kind is refused
    st, reply = http(port, "POST", "/items/%d/paint" % SKIN, {"item_id": SKIN, "paint_item_id": PAINT}, token)
    check("a second paint with none left is refused (400)",
          st == 400 and "paint" in str(err_of(reply).get("message", "")), "HTTP %d %s" % (st, json.dumps(reply)[:200]))

SHADER_PAINT = 7795  # Spectrum (paintShader 0; v10_paint.c)
BUYER = "paint_buyer"  # at most 14 characters: a new name's cap (v10.0.3 maxNameLength, deob-10.0.3.js:22725)


def scenario_shader(port, db):
    """A shader paint (paintShader 0, no colour): the copy, its listing, the purchase and the buyer's copy keep m."""
    st, reply = http(port, "POST", "/auth/register", {"username": "paint_shader", "password": PASSWORD})
    token = reply.get("token") if isinstance(reply, dict) else None
    check("shader: the seller registers", st == 200 and bool(token), "HTTP %d" % st)
    if not token:
        return
    acc = query(db, "SELECT id FROM accounts WHERE username = ?", ("paint_shader",))
    acc_id = acc[0][0] if acc else 0
    seed(db, [
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", (acc_id, SKIN)),
        ("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, ?, 1)", (acc_id, SHADER_PAINT)),
        ("UPDATE players SET player_score = 200000, player_funds = 1000 WHERE player_id = ?", (acc_id,)),  # level 13 (the market needs 10), a listing fee
    ])
    st, reply = http(port, "POST", "/items/%d/paint" % SKIN, {"item_id": SKIN, "paint_item_id": SHADER_PAINT}, token)
    d = data_of(reply) or {}
    copy_id = d.get("copy_id")
    check("shader: POST /items/{id}/paint makes a copy with m = {paintShader: 0}",
          st == 200 and isinstance(copy_id, int) and copy_id >= COPY_BASE and d.get("m") == {"paintShader": 0},
          "HTTP %d %s" % (st, json.dumps(reply)[:200]))
    if not isinstance(copy_id, int):
        return
    row = query(db, "SELECT paint_color, paint_shader FROM skin_copies WHERE copy_id = ?", (copy_id,))
    check("shader: skin_copies keeps the shader (paint_shader 0, colour NULL)", row == [(None, 0)], repr(row))
    st, owned = owned_list(port, token)
    mine = [x for x in owned if x.get("id") == copy_id]
    check("shader: the owned list (data[10]) carries m = {paintShader: 0}",
          len(mine) == 1 and mine[0].get("m") == {"paintShader": 0}, json.dumps(owned)[:200])

    # list the painted copy (skin_id = its copy id, the quick-sell convention) and buy it with another account
    st, reply = http(port, "POST", "/market/list", {"item_id": SKIN, "skin_id": copy_id, "price": 10}, token)
    check("shader: the painted copy lists on the market (HTTP 200)", st == 200, "HTTP %d %s" % (st, json.dumps(reply)[:200]))
    st, sales = http(port, "GET", "/market/sales", None, token)
    listing = [x for x in (data_of(sales) or []) if isinstance(x, dict) and x.get("skinindex") == SKIN and "m" in x]
    check("shader: the listing keeps the copy's m = {paintShader: 0}",
          st == 200 and len(listing) == 1 and listing[0].get("m") == {"paintShader": 0}, json.dumps(sales)[:300])
    if not listing:
        return
    market_id = listing[0].get("marketid")
    st, reg = http(port, "POST", "/auth/register", {"username": BUYER, "password": PASSWORD})
    btoken = reg.get("token") if isinstance(reg, dict) else None
    bacc = query(db, "SELECT id FROM accounts WHERE username = ?", (BUYER,))
    if bacc:
        seed(db, [("UPDATE players SET player_funds = 1000, player_score = 200000 WHERE player_id = ?", (bacc[0][0],))])
    seed(db, [("UPDATE market_listings SET created_at = 0 WHERE id = ?", (market_id,))])  # a listing is bought once it is not fresh
    st, reply = http(port, "POST", "/market/purchase", {"market_id": market_id}, btoken)
    bought = data_of(reply) or {}
    check("shader: the buyer receives the copy with m = {paintShader: 0}",
          st == 200 and bought.get("m") == {"paintShader": 0} and bought.get("i") == SKIN, "HTTP %d %s" % (st, json.dumps(reply)[:300]))
    if bacc:
        rows = query(db, "SELECT copy_id, paint_shader FROM skin_copies WHERE account_id = ? AND skin_id = ?", (bacc[0][0], SKIN))
        check("shader: the buyer's skin_copies row keeps paint_shader 0", len(rows) == 1 and rows[0][1] == 0, repr(rows))
        check("shader: the purchase names the buyer's copy id as sid (not the store index)",
              len(rows) == 1 and bought.get("sid") == rows[0][0] and bought.get("sid") >= COPY_BASE, "sid %r rows %r" % (bought.get("sid"), rows))


def main():
    if not os.access(BRAIN, os.X_OK):
        print("paint_e2e_test: no kn_brain at %s: skipped" % BRAIN)
        return 77
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "create", "sqlite", NAME],
                       capture_output=True)
    if r.returncode:
        print("paint_e2e_test: no sqlite scratch database: skipped")
        return 77
    db = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET,
               **{"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()})
    work = tempfile.mkdtemp(prefix="kn-paint-e2e.")
    log = os.path.join(work, "brain.log")
    procs = []
    try:
        mig = subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, capture_output=True)
        check("the migrations apply (0352_skin_copies on sqlite)", mig.returncode == 0,
              mig.stderr.decode(errors="replace")[-200:])
        if mig.returncode:
            return 1
        brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=paint-e2e-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(log)), 20)
        check("kn_brain starts on an ephemeral port", m is not None, read(log)[-300:])
        if m is None:
            return 1
        try:
            scenario(int(m.group(1)), db["path"])
            scenario_shader(int(m.group(1)), db["path"])
        except Exception as e:  # noqa: BLE001 - a crashed probe is a failed run, not a crash of the test
            check("the probes complete", False, repr(e)[:200])
    finally:
        for p in procs:
            stop(p)
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "drop", "sqlite", NAME],
                       capture_output=True)
    ok = bool(RESULTS) and all(RESULTS)
    print("paint_e2e_test: %d PASS, %d FAIL" % (sum(RESULTS), len(RESULTS) - sum(RESULTS)))
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("logs kept in " + work)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Krunker v10.0.2 clan window backend (client/src/ui/clan_ui.c): the social socket's clan ops and
the settings tab's gapi routes (services/brain/clan_v10.c, social_data.c, db_social.c,
0113_clan_v10), on a kn_brain with its own sqlite database (never config/config.json).

  clan_v10_test.py <kn_brain> <repo root>
  KN_TEST_DB_DRIVER=postgres: a scratch schema in krunknative_svcacc (config/config.json credentials,
  or the file KN_TEST_CONFIG names)

Covers: create clan, join request + accept, the owner making a Captain, a Captain's rights (role
change but no Captains, Decline All, no Discord), the Discord link (cln 9), s7, MOTD set / get /
delete (members read, only role >= 2 writes), banner colour from a seeded paint (consumed; none
left = refused), ownership transfer initiate / cancel / respond, the loot config read, the Captain (5)
and Soldier (25) role limits, and the clan logo (clan_logo.c): the owner or a Captain only, the profile
picture's refusals (UnsupportedType, ImageTooSmall / ImageTooLarge, FileTooLarge) with no charge, one
pending logo at a time, 20,000 KR taken from the clan bank exactly once and only by an approval (ledger
row), a rejection and a failed review leave the funds and the ledger unchanged, an approval the clan
cannot pay for is not set, approval sets clan_logo and serves the WebP, DELETE removes the logo.
The moderation review goes to a STUBBED gateway (KN_AI_UPSTREAM_URL), never the live one.
77 = skip (no kn_brain / websockets).
"""
import asyncio
import base64
import itertools
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
import urllib.error
import urllib.request
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
if not os.path.exists(BRAIN):
    print("no kn_brain: skip")
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-clan-v10.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
RID = itertools.count(1)


def check(cond, what, detail=""):
    print(("  ok   " if cond else "  FAIL ") + what + (("  -- " + str(detail)) if detail and not cond else ""), flush=True)
    if not cond:
        FAILS.append(what)


DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
PG = {}


def lit(v):
    return str(int(v)) if isinstance(v, (int, bool)) else "'" + str(v).replace("'", "''") + "'"


def q(sql, *args):
    parts = sql.split("?")
    text = "".join(p + (lit(args[i]) if i < len(args) else "") for i, p in enumerate(parts))
    if DRIVER != "postgres":
        con = sqlite3.connect(DB, timeout=30)
        try:
            rows = con.execute(text).fetchall()
            con.commit()
            return rows
        finally:
            con.close()
    out = subprocess.run(PG["psql"] + ["-c", "SET search_path TO %s; %s" % (PG["schema"], text)], env=PG["env"],
                         capture_output=True, text=True, check=True).stdout
    rows = []
    for line in out.splitlines():
        if not line or line == "SET":
            continue
        rows.append(tuple(None if c == "" else int(c) if re.fullmatch(r"-?\d+", c) else c for c in line.split("|")))
    return rows


NOW_PLUS_1S = "now() + interval '1 second'" if DRIVER == "postgres" else "strftime('%Y-%m-%d %H:%M:%f', 'now', '+1 second')"

# ---- the moderation gateway (STUB): OpenAI-compatible /cpa/v1/chat/completions; mode per test step
MOD_KEY = "sk-clan-logo-test-%d" % os.getpid()


class Gateway:
    def __init__(self):
        self.mode = "approve"  # approve | reject | fail | slow
        self.calls = 0
        gw = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def _send(self, code, obj):
                data = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_POST(self):
                length = int(self.headers.get("Content-Length") or 0)
                body = json.loads(self.rfile.read(length) or b"{}")
                if self.path != "/cpa/v1/chat/completions":
                    return self._send(404, {"error": {"message": "no route"}})
                if self.headers.get("Authorization") != "Bearer " + MOD_KEY:
                    return self._send(401, {"error": {"message": "bad key"}})
                msgs = body.get("messages", [])
                parts = msgs[-1].get("content") if msgs else None
                if not (isinstance(parts, list) and any(p.get("type") == "image_url" for p in parts)):
                    return self._send(200, {"choices": [{"index": 0, "finish_reason": "stop", "message": {"role": "assistant", "content": "Hello"}}]})
                gw.calls += 1
                nonce = re.search(r'"nonce":"([^"]+)"', msgs[0].get("content", ""))
                nonce = nonce.group(1) if nonce else ""
                mode = gw.mode
                if mode == "fail":
                    return self._send(500, {"error": {"message": "stub failure"}})
                if mode == "slow":
                    time.sleep(3)
                    mode = "approve"
                if mode == "reject":
                    answer = {"nonce": nonce, "allowed": False, "categories": ["gore"], "reason": "Shows blood."}
                else:
                    answer = {"nonce": nonce, "allowed": True, "categories": [], "reason": "Looks fine."}
                self._send(200, {"choices": [{"index": 0, "finish_reason": "stop",
                                              "message": {"role": "assistant", "content": json.dumps(answer)}}],
                                 "usage": {"prompt_tokens": 7, "completion_tokens": 2, "total_tokens": 9}})

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.server.daemon_threads = True
        self.url = "http://127.0.0.1:%d/cpa/v1" % self.server.server_address[1]

    def start(self):
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        return self

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


GW = None


def png_rgba(w, h, raw=None):
    """A w x h RGBA PNG: left half red, right half blue unless raw (w*h*4 bytes of pixels) is given."""
    rows = []
    for y in range(h):
        if raw is None:
            row = bytearray(b"\x00")
            for x in range(w):
                row += bytes((255, 0, 0, 255)) if x < w // 2 else bytes((0, 0, 255, 255))
        else:
            row = b"\x00" + raw[y * w * 4:(y + 1) * w * 4]
        rows.append(bytes(row))

    def chunk(kind, data):
        return len(data).to_bytes(4, "big") + kind + data + (zlib.crc32(kind + data) & 0xFFFFFFFF).to_bytes(4, "big")

    ihdr = w.to_bytes(4, "big") + h.to_bytes(4, "big") + bytes((8, 6, 0, 0, 0))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(b"".join(rows))) +
            chunk(b"IEND", b""))


def b64(data):
    return base64.b64encode(data).decode()


class Sock:
    def __init__(self, ws):
        self.ws = ws
        self.buf = []

    @classmethod
    async def open(cls, port):
        return cls(await websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port))

    async def send(self, *frame):
        await self.ws.send(json.dumps(list(frame)))

    async def wait(self, pred, timeout=10.0):
        for i, f in enumerate(self.buf):
            if pred(f):
                return self.buf.pop(i)
        end = time.time() + timeout
        while time.time() < end:
            try:
                f = json.loads(await asyncio.wait_for(self.ws.recv(), max(0.05, end - time.time())))
            except asyncio.TimeoutError:
                break
            if pred(f):
                return f
            self.buf.append(f)
        return None

    async def auth(self, name):
        await self.send("a", 0, [name, "clan_pass_1", None])
        f = await self.wait(lambda f: f[0] == "a")
        assert f and f[1] is None, f
        self.id = f[2]
        return f[2]

    async def cln(self, action, value=1, extra=None):
        frame = ["cln", action, value] + ([extra] if extra is not None else [])
        await self.send(*frame)
        return await self.wait(lambda f: f[0] == "cln" and len(f) >= 5)

    async def gapi(self, method, path, body=None):
        rid = next(RID)
        await self.send("gapi", rid, method, path, body)
        f = await self.wait(lambda f: f[0] == "gapi" and f[1] == rid)
        return (f[2], (f[3] or {}).get("data"), (f[3] or {}).get("error")) if f else (0, None, None)

    async def close(self):
        await self.ws.close()


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    a, b, c, d = [await Sock.open(port) for _ in range(4)]
    names = ["clA" + stamp, "clB" + stamp, "clC" + stamp, "clD" + stamp]
    for s, n in zip((a, b, c, d), names):
        await s.auth(n)
    tag = "T" + stamp[-3:]

    f = await a.cln(1, tag)
    check(f and f[3] == 1 and f[2] and f[2]["clan_name"] == tag, "create clan", f)
    clan = f[2]["clan_id"] if f and f[2] else 0
    check(f and f[2] and "clan_link4" in f[2] and "clan_banner_color" in f[2], "clanData has the v10 fields", f and f[2])

    for s in (b, c):
        f = await s.cln(2, tag)
        check(f and f[1] == "Request Sent", "join request", f)
    await a.send("clnR", b.id, 1)
    f = await a.wait(lambda f: f[0] == "cln" and len(f) >= 5)
    check(f and f[3] == 1 and any(m["i"] == b.id for m in f[2]["members"]), "owner accepts the request", f)
    await a.send("clnR", c.id, 1)
    await a.wait(lambda f: f[0] == "cln" and len(f) >= 5)

    f = await a.cln(6, b.id, {"r": 2})
    check(f and f[3] == 1 and f[2] == {"i": b.id, "r": 2}, "owner makes a Captain", f)
    f = await a.cln(6, c.id, {"r": 3})
    check(f and f[3] == 0, "nobody is made Commander", f)
    f = await b.cln(6, c.id, {"r": 1})
    check(f and f[3] == 1 and f[2]["r"] == 1, "Captain changes a Soldier's role", f)
    f = await b.cln(6, c.id, {"r": 2})
    check(f and f[3] == 0 and f[1] == "No Permission", "Captain cannot make Captains", f)
    f = await c.cln(6, b.id, {"r": 0})
    check(f and f[3] == 0, "a Soldier manages nobody", f)

    f = await d.cln(2, tag)
    check(f and f[1] == "Request Sent", "a third request", f)
    await b.send("clnRA")
    await asyncio.sleep(0.5)
    check(q("SELECT count(*) FROM clan_requests WHERE clan_id = ?", clan)[0][0] == 0, "Captain declines all requests")

    f = await b.cln(9, "https://discord.gg/abcDEF")
    check(f and f[3] == 0, "a Captain cannot set the Discord link", f)
    f = await a.cln(9, "not a link")
    check(f and f[3] == 0 and f[1] == "Invalid Discord", "invalid Discord link refused", f)
    f = await a.cln(9, "https://discord.com/invite/abcDEF-12")
    check(f and f[3] == 1 and f[2]["clan_link4"] == "abcDEF-12", "owner sets the Discord link", f)

    q("INSERT INTO match_results (match_id, account_id, score, at) VALUES ('m1', ?, 1234, " + NOW_PLUS_1S + ")", b.id)
    f = await a.cln(3)
    sb = [m for m in (f[2]["members"] if f and f[2] else []) if m["i"] == b.id]
    check(sb and sb[0]["s7"] == 1234, "s7 = the member's score of the last 7 days", sb)

    st, data, err = await a.gapi("PUT", "/clans/%d/motd" % clan, {"message": "  Hello clan  "})
    check(st == 200 and data and data["message"] == "Hello clan" and data["set_by_name"] == names[0], "owner sets the MOTD", (st, data, err))
    st, data, err = await c.gapi("GET", "/clans/%d/motd" % clan)
    check(st == 200 and data and data["message"] == "Hello clan" and data["set_at"], "a member reads the MOTD", (st, data, err))
    st, data, err = await c.gapi("PUT", "/clans/%d/motd" % clan, {"message": "nope"})
    check(st != 200 and err, "a Soldier cannot set the MOTD", (st, data, err))
    st, data, err = await d.gapi("GET", "/clans/%d/motd" % clan)
    check(st != 200, "an outsider cannot read the MOTD", (st, data, err))
    st, data, err = await b.gapi("DELETE", "/clans/%d/motd" % clan)
    check(st == 200 and data and data["message"] is None, "a Captain removes the MOTD", (st, data, err))

    q("INSERT INTO inventory (account_id, skin_id, count) VALUES (?, 7776, 1)", a.id)
    st, data, err = await a.gapi("POST", "/clans/%d/banner-color" % clan, {"paint_item_id": 7776})
    check(st == 200 and data and data["clan_banner_color"] == "#dc3c3c", "banner colour from a paint", (st, data, err))
    check(q("SELECT count FROM inventory WHERE account_id = ? AND skin_id = 7776", a.id)[0][0] == 0, "the paint is consumed")
    st, data, err = await a.gapi("POST", "/clans/%d/banner-color" % clan, {"paint_item_id": 7776})
    check(st != 200 and err and "paint" in err.get("message", ""), "no paint left: refused", (st, data, err))
    st, data, err = await a.gapi("POST", "/clans/%d/banner-color" % clan, {"paint_item_id": 5})
    check(st != 200, "a non-paint item is refused", (st, data, err))
    f = await a.cln(3)
    check(f and f[2]["clan_banner_color"] == "#dc3c3c", "clanData carries the banner colour", f and f[2])

    st, data, err = await b.gapi("POST", "/clans/%d/transfer" % clan, {"name": names[2]})
    check(st != 200, "only the owner transfers", (st, data, err))
    st, data, err = await a.gapi("POST", "/clans/%d/transfer" % clan, {"name": names[3]})
    check(st != 200, "transfer to a non-member refused", (st, data, err))
    st, data, err = await a.gapi("POST", "/clans/%d/transfer" % clan, {"name": names[2]})
    check(st == 200 and data and data["to_player_id"] == c.id and data["from_player_name"] == names[0], "owner starts a transfer", (st, data, err))
    st, data, err = await a.gapi("DELETE", "/clans/%d/transfer" % clan)
    check(st == 200, "owner cancels the transfer", (st, data, err))
    st, data, err = await c.gapi("POST", "/clans/%d/transfer/respond" % clan, {"accept": True})
    check(st != 200, "no pending transfer: respond refused", (st, data, err))
    await a.gapi("POST", "/clans/%d/transfer" % clan, {"name": names[2]})
    f = await c.cln(3)
    ot = f[2].get("ownership_transfer") if f and f[2] else None
    check(ot and ot["to_player_id"] == c.id and ot["clan_name"] == tag, "the target sees the pending transfer", f and f[2])
    st, data, err = await b.gapi("POST", "/clans/%d/transfer/respond" % clan, {"accept": True})
    check(st != 200, "someone else cannot accept", (st, data, err))
    st, data, err = await c.gapi("POST", "/clans/%d/transfer/respond" % clan, {"accept": True})
    check(st == 200 and data and data["accepted"], "the target accepts", (st, data, err))
    row = q("SELECT clan_playerid, clan_transfer_to FROM clans WHERE clan_id = ?", clan)[0]
    check(row[0] == c.id and row[1] is None, "the clan changed hands", row)
    ranks = dict(q("SELECT account_id, rank FROM clan_members WHERE clan_id = ?", clan))
    check(ranks.get(c.id) == 3 and ranks.get(a.id) == 2, "new owner Commander, old owner Captain", ranks)

    await logo_checks(a, c, d, clan)

    with urllib.request.urlopen("http://127.0.0.1:%d/api/clans/%d/loot-config" % (port, clan), timeout=10) as r:
        body = json.loads(r.read())
    data = body.get("data") or {}
    cfg = (data.get("config") or [data])[0]
    check(r.status == 200 and cfg.get("distribution_mode") == "clan_bank", "loot config read", body)

    f = await c.cln(4, 1)
    await asyncio.sleep(0.3)
    check(q("SELECT count(*) FROM clans WHERE clan_id = ?", clan)[0][0] == 0, "the owner deletes the clan")
    for s in (a, b, c, d):
        await s.close()


async def role_caps(port):
    """the role limits the clan menu shows (config.clanRoles: Soldier 25, Captain 5): a change into a
    full role is refused, a freed place can be taken again (cln 6)"""
    stamp = str(int(time.time()) % 100000)
    owner = await Sock.open(port)
    await owner.auth("rcO" + stamp)
    tag = "R" + stamp[-3:]
    f = await owner.cln(1, tag)
    check(f and f[3] == 1, "role caps: a clan for the limits", f)
    members = []
    for i in range(32):
        m = await Sock.open(port)
        await m.auth("rcM%02d%s" % (i, stamp))
        f = await m.cln(2, tag)
        assert f and f[1] == "Request Sent", f
        await owner.send("clnR", m.id, 1)
        await owner.wait(lambda f: f[0] == "cln" and len(f) >= 5)
        members.append(m)
    f = None
    for m in members[:5]:
        f = await owner.cln(6, m.id, {"r": 2})
    check(f and f[3] == 1 and f[2]["r"] == 2, "five Captains (the limit)", f)
    f = await owner.cln(6, members[5].id, {"r": 2})
    check(f and f[3] == 0 and "Captains are full" in str(f[1]), "a sixth Captain is refused", f)
    for m in members[6:31]:  # 25 Soldiers
        f = await owner.cln(6, m.id, {"r": 1})
    check(f and f[3] == 1 and f[2]["r"] == 1, "twenty-five Soldiers (the limit)", f)
    f = await owner.cln(6, members[31].id, {"r": 1})
    check(f and f[3] == 0 and "Soldiers are full" in str(f[1]), "a twenty-sixth Soldier is refused", f)
    f = await owner.cln(6, members[6].id, {"r": 0})
    check(f and f[3] == 1, "a Soldier demoted to Recruit frees a place")
    f = await owner.cln(6, members[31].id, {"r": 1})
    check(f and f[3] == 1 and f[2]["r"] == 1, "the freed place goes to the next Soldier", f)
    f = await owner.cln(6, members[0].id, {"r": 2})
    check(f and f[3] == 1, "a Captain set to Captain again is allowed at the limit", f)
    for s in [owner] + members:
        await s.close()


def wait_db(fn, timeout=20.0):
    """Polls a query (sync) until fn(rows) is true; the verdict is written asynchronously."""
    end = time.time() + timeout
    while time.time() < end:
        rows = fn()
        if rows:
            return rows
        time.sleep(0.2)
    return None


def fund(clan, kr):
    q("UPDATE clans SET clan_funds = ? WHERE clan_id = ?", kr, clan)


def clan_row(clan):
    return q("SELECT clan_funds, clan_logo, clan_logo_pending FROM clans WHERE clan_id = ?", clan)[0]


def ledger_rows(clan):
    return q("SELECT amount, source FROM clan_funds_ledger WHERE clan_id = ? ORDER BY id", clan)


def settled(clan):
    """The review's row once it is no longer pending (the verdict is written asynchronously)."""
    rows = wait_db(lambda: [r for r in q("SELECT status FROM clan_logos WHERE clan_id = ? AND status <> 'pending'", clan)])
    return rows[0][0] if rows else None


async def logo_checks(a, c, d, clan):
    """a is a Captain, c the owner (after the transfer), d is not in the clan. The charge is taken only
    when an approval sets the logo: a rejected or failed review leaves the funds and the ledger alone."""
    logo_path = "/clans/%d/logo" % clan
    png = png_rgba(512, 512)
    fund(clan, 0)
    st, data, err = await d.gapi("POST", logo_path, {"file": b64(png)})
    check(st != 200 and "Only the clan owner" in (err or {}).get("message", ""), "an outsider cannot upload a logo", (st, data, err))

    # the profile path's refusals, each before any review: nothing charged, nothing pending
    fund(clan, 30000)
    st, data, err = await a.gapi("POST", logo_path, {"file": b64(b"GIF89a\x01\x00\x01\x00\x80\x00\x00\x00\x00\xff\xff\xff!\xf9\x04\x01\x00\x00\x00\x00,\x00\x00\x00\x00\x01\x00\x01\x00\x00\x02\x02D\x01\x00;")})
    check(st == 415 and err and err.get("code") == "UnsupportedType", "a GIF is refused (UnsupportedType)", (st, data, err))
    st, data, err = await a.gapi("POST", logo_path, {"file": b64(png_rgba(500, 500))})
    check(st == 422 and err and err.get("code") == "ImageTooSmall", "a 500 x 500 PNG is refused (ImageTooSmall)", (st, data, err))
    st, data, err = await a.gapi("POST", logo_path, {"file": b64(png_rgba(600, 600))})
    check(st == 413 and err and err.get("code") == "ImageTooLarge", "a 600 x 600 PNG is refused (ImageTooLarge)", (st, data, err))
    noise = os.urandom(512 * 512 * 4)
    st, data, err = await a.gapi("POST", logo_path, {"file": b64(png_rgba(512, 512, noise))})
    check(st == 413 and err and err.get("code") == "FileTooLarge", "a 512 x 512 PNG over 256 KB is refused (FileTooLarge)", (st, data, err))
    st, data, err = await a.gapi("POST", logo_path, {})
    check(st == 400 and err, "an upload without a file is refused", (st, data, err))
    check(clan_row(clan) == (30000, 0, 0) and ledger_rows(clan) == [], "refused uploads take nothing and pend nothing", clan_row(clan))

    # the upload itself needs the price in the bank (v10's check), but charges nothing
    fund(clan, 10000)
    st, data, err = await a.gapi("POST", logo_path, {"file": b64(png)})
    check(st != 200 and err and "Insufficient clan funds" in err.get("message", ""), "an upload without 20,000 KR in the bank is refused", (st, data, err))
    check(clan_row(clan) == (10000, 0, 0), "that refusal changes nothing", clan_row(clan))

    # a valid upload: pending, nothing charged yet; a second one is refused while it is pending
    fund(clan, 30000)
    GW.mode = "slow"  # the review takes 3 s: the pending window is open for the second upload
    st, data, err = await a.gapi("POST", logo_path, {"file": b64(png)})
    check(st == 200 and data and data.get("status") == "pending" and data.get("clan_logo_pending") == 1,
          "a 512 x 512 PNG is accepted as pending (version 1)", (st, data, err))
    check(clan_row(clan) == (30000, 0, 1) and ledger_rows(clan) == [], "the upload charges nothing (funds and ledger unchanged)", clan_row(clan))
    st, data, err = await a.gapi("POST", logo_path, {"file": b64(png)})
    check(st != 200 and err and "pending" in err.get("message", ""), "a second upload while one is pending is refused", (st, data, err))
    check(clan_row(clan) == (30000, 0, 1) and ledger_rows(clan) == [], "the refused second upload takes nothing", clan_row(clan))
    check(settled(clan) == "approved", "the review approves the logo")
    check(clan_row(clan) == (10000, 1, 0), "an approval charges 20,000 KR once and sets clan_logo = 1", clan_row(clan))
    check(ledger_rows(clan) == [(-20000, "clan-logo")], "the approval writes one clan-logo ledger row", ledger_rows(clan))
    f = await a.cln(3)
    check(f and f[2] and f[2].get("clan_logo") == 1 and f[2].get("clan_logo_pending") == 0, "clanData carries the approved logo", f and f[2])
    with urllib.request.urlopen("http://127.0.0.1:%d/api/clans/%d/logo" % (PORT, clan), timeout=10) as r:
        body = r.read()
        ctype = r.headers.get("Content-Type")
    check(ctype == "image/webp" and body[:4] == b"RIFF" and body[8:12] == b"WEBP", "the approved logo is served as WebP")

    # a rejection: funds and ledger unchanged, the pending version cleared, the approved logo stays
    fund(clan, 30000)
    before_funds, before_ledger = clan_row(clan)[0], ledger_rows(clan)
    GW.mode = "reject"
    st, data, err = await a.gapi("POST", logo_path, {"file": b64(png)})
    check(st == 200 and data and data.get("clan_logo_pending") == 2, "a second logo is accepted as pending (version 2)", (st, data, err))
    check(settled(clan) == "rejected", "the review rejects the second logo")
    check(clan_row(clan) == (before_funds, 1, 0), "a rejection clears the pending logo and keeps clan_logo 1", clan_row(clan))
    check(clan_row(clan)[0] == before_funds and ledger_rows(clan) == before_ledger,
          "a rejection charges nothing and refunds nothing (funds and ledger unchanged)", (clan_row(clan), ledger_rows(clan)))
    with urllib.request.urlopen("http://127.0.0.1:%d/api/clans/%d/logo" % (PORT, clan), timeout=10) as r:
        still = r.status
    check(still == 200, "the approved logo is still served after a rejection")

    # a failed review: also cleared, funds and ledger unchanged
    before_funds, before_ledger = clan_row(clan)[0], ledger_rows(clan)
    GW.mode = "fail"
    st, data, err = await a.gapi("POST", logo_path, {"file": b64(png)})
    check(st == 200, "a third logo is accepted as pending", (st, data, err))
    check(settled(clan) == "error", "the failed review ends in error")
    check(clan_row(clan) == (before_funds, 1, 0), "a failed review clears the pending logo", clan_row(clan))
    check(clan_row(clan)[0] == before_funds and ledger_rows(clan) == before_ledger,
          "a failed review charges nothing (funds and ledger unchanged)", (clan_row(clan), ledger_rows(clan)))
    GW.mode = "approve"

    # the bank runs short while the review is running: the approval cannot pay, so the logo is not set
    fund(clan, 25000)
    before_ledger = ledger_rows(clan)
    GW.mode = "slow"
    st, data, err = await a.gapi("POST", logo_path, {"file": b64(png)})
    check(st == 200, "a fourth logo is accepted as pending while the clan has 25,000 KR", (st, data, err))
    fund(clan, 5000)
    check(settled(clan) == "error", "an approval the clan cannot pay for is not set (error)")
    check(clan_row(clan) == (5000, 1, 0) and ledger_rows(clan) == before_ledger,
          "that approval charges nothing and leaves clan_logo 1 (funds and ledger unchanged)", (clan_row(clan), ledger_rows(clan)))
    GW.mode = "approve"

    # the owner may upload too; DELETE removes the logo and its picture
    fund(clan, 30000)
    st, data, err = await c.gapi("POST", logo_path, {"file": b64(png)})
    check(st == 200, "the owner uploads a logo (Captains and the owner)", (st, data, err))
    check(settled(clan) == "approved", "the owner's logo is approved")
    check(clan_row(clan)[0] == 10000 and len(ledger_rows(clan)) == 2, "the owner's approval charges 20,000 KR once more", clan_row(clan))
    st, data, err = await d.gapi("DELETE", logo_path)
    check(st != 200, "an outsider cannot remove the logo", (st, data, err))
    st, data, err = await a.gapi("DELETE", logo_path)
    check(st == 200, "a Captain removes the logo", (st, data, err))
    check(clan_row(clan)[1:] == (0, 0), "after the delete clan_logo and clan_logo_pending are 0", clan_row(clan))
    check(q("SELECT count(*) FROM clan_logos WHERE clan_id = ?", clan)[0][0] == 0, "the delete removes the picture row")
    try:
        urllib.request.urlopen("http://127.0.0.1:%d/api/clans/%d/logo" % (PORT, clan), timeout=10)
        gone = False
    except urllib.error.HTTPError as e:
        gone = e.code == 404
    check(gone, "after the delete the logo is not served (404)")


PORT = 0




def main():
    global GW, PORT
    (port,) = free_ports(1)
    PORT = port
    GW = Gateway().start()
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    dbcfg = {"driver": "sqlite", "path": DB}
    if DRIVER == "postgres":
        with open(os.environ.get("KN_TEST_CONFIG", os.path.join(ROOT, "config", "config.json"))) as fp:
            db = json.load(fp).get("database", {})
        dbname = os.environ.get("KN_TEST_DB_NAME", "krunknative_svcacc")
        PG["schema"] = "kn_test_clan_v10_%d" % os.getpid()
        PG["env"] = dict(os.environ, PGPASSWORD=str(db.get("password", "")))
        PG["psql"] = ["psql", "-h", str(db.get("host", "127.0.0.1")), "-p", str(db.get("port", 5432)), "-U",
                      str(db.get("user", "krunknative")), "-d", dbname, "-v", "ON_ERROR_STOP=1", "-qAt"]
        try:
            subprocess.run(PG["psql"] + ["-c", "CREATE SCHEMA %s" % PG["schema"]], env=PG["env"], check=True, capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            print("PostgreSQL unavailable (%s): skip" % e)
            sys.exit(77)
        dbcfg = dict(db, name=dbname, search_path=PG["schema"])
    with open(cfg, "w") as f:
        json.dump({"database": dict(dbcfg, migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "clan-v10-%d" % os.getpid()},
                   "accounts": {"free_premium": False, "starting_kr": 0, "login_throttle_s": 0, "password_iterations": 1000,
                                "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    # the moderation profile is the stub gateway above; no KN_AI_* from the caller's environment
    env = dict({k: v for k, v in os.environ.items() if not k.startswith("KN_AI_")},
               KN_AI_UPSTREAM_URL=GW.url, KN_AI_UPSTREAM_KEY=MOD_KEY)
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate", "--ai.health_interval_s=0"], cwd=ROOT, env=env, stdout=log,
                             stderr=subprocess.STDOUT, start_new_session=True)
    try:
        for _ in range(200):
            try:
                with open(os.path.join(WORK, "brain.log")) as f:
                    if re.search(r"listening on http://127\.0\.0\.1:%d" % port, f.read()):
                        break
            except OSError:
                pass
            if brain.poll() is not None:
                break
            time.sleep(0.1)
        asyncio.run(scenario(port))
        asyncio.run(role_caps(port))
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
        GW.stop()
        if PG:
            subprocess.run(PG["psql"] + ["-c", "DROP SCHEMA IF EXISTS %s CASCADE" % PG["schema"]], env=PG["env"], capture_output=True)
    print("clan v10 test: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

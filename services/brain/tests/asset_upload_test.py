#!/usr/bin/env python3
"""The editor's Publish Asset on a Box Office server (services/brain/asset_upload.c): uploadA -> pubR {fileId, srv},
the declared files as raw chunks to POST /upload-user-asset?fileId=&key=&offset=, then ?fileId=&done=1.

  asset_upload_test.py <build dir> <repo root>

Checks:
- a texture: pubR (srv null), chunks in order (a re-sent chunk answers 200, a gap 409, a chunk past the declared size
  400), done -> 200 {id >= 1000000000}; the file is answered by GET /api/user-assets/<id>/texture.png byte for byte with
  Cache-Control no-cache, and the asset is listed by /api/assets with data {"x":1}.
- Krunker 10.0.7 "a failed custom asset upload not being retryable": a model whose OBJ is not text fails its check
  (400 "Invalid Model") and leaves nothing published; its ticket stays open (the files can be sent again), and a new
  uploadA of the account replaces it (the old fileId answers 404) and publishes the model with its companion maps
  (data letters e, n; files model.obj, model.png, model_e.png, model_n.png).
- an update: the same name and kind again keeps the id and bumps version; another kind of the same name is a new asset.
- the editor's limits (editor-config-10.0.7.js:9197-9215): a texture over 10 MB, a free account's 300 kB thumbnail (a
  premium one's passes) and a model total over 100 MB answer upMp "File size Limit Reached"; a bad category "Invalid Category"; no required file
  "Missing File"; a guest "Must be logged in"; an incomplete upload's done "Upload incomplete".
- a glTF naming an external buffer is refused (one file), a GLB passes, a prefab's data.json is stored.
- Private: left out of the listing unless the creator's My Assets tab asks (username).

Database: sqlite (KN_TEST_DB_DRIVER=postgres: the config's PostgreSQL in a scratch schema). 77 = skip.
"""
import asyncio
import json
import os
import re
import shutil
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zlib

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD, ROOT = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
if not os.path.exists(BRAIN):
    print("needs kn_brain in %s: skip" % BUILD)
    sys.exit(77)
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
from kn_proto import free_ports  # noqa: E402

FAILS = []
WORK = tempfile.mkdtemp(prefix="kn-upload.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
UPLOADS = os.path.join(WORK, "uploads")
PORT = free_ports(1)[0]
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
PG = {}
BASE = 1000000000


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def png(w=2, h=2, pad=0):
    raw = b"".join(b"\0" + b"\x80\x40\x20\xff" * w for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    text = chunk(b"tEXt", b"c\0" + b"x" * pad) if pad else b""
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + text +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


OBJ = b"v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n"
GLTF_EXT = json.dumps({"asset": {"version": "2.0"}, "buffers": [{"uri": "model.bin", "byteLength": 4}]}).encode()


def glb():
    js = json.dumps({"asset": {"version": "2.0"}, "buffers": [{"byteLength": 4}]}).encode()
    js += b" " * (-len(js) % 4)
    bin_ = b"\0\0\0\0"
    body = struct.pack("<I4s", len(js), b"JSON") + js + struct.pack("<I4s", len(bin_), b"BIN\0") + bin_
    return struct.pack("<4sII", b"glTF", 2, 12 + len(body)) + body


def http(method, path, body=None, token=None):
    headers = {"Content-Type": "application/octet-stream"} if body is not None else {}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (PORT, path), data=body, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.status, r.read(), dict(r.headers)
    except urllib.error.HTTPError as e:
        return e.code, e.read(), dict(e.headers)


def send_file(fid, key, data, chunk=512 * 1024):
    at = 0
    while at < len(data):
        part = data[at:at + chunk]
        st, body, _ = http("POST", "/upload-user-asset?fileId=%s&key=%s&offset=%d" % (fid, key, at), part)
        if st != 200:
            return st, body
        at += len(part)
    return 200, b""


def done(fid):
    st, body, _ = http("POST", "/upload-user-asset?fileId=%s&done=1" % fid, b"")
    return st, body


class Sock:
    def __init__(self):
        self.ws = None
        self.inbox = []

    async def open(self):
        self.ws = await websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % PORT, max_size=None)
        return self

    async def send(self, frame):
        await self.ws.send(json.dumps(frame))

    async def recv_until(self, pred, timeout=10.0):
        for i, f in enumerate(self.inbox):
            if pred(f):
                return self.inbox.pop(i)
        end = time.time() + timeout
        while time.time() < end:
            try:
                f = json.loads(await asyncio.wait_for(self.ws.recv(), max(0.05, end - time.time())))
            except asyncio.TimeoutError:
                break
            if f and f[0] == "pi":
                await self.send(["po"])
                continue
            if pred(f):
                return f
            self.inbox.append(f)
        return None

    async def register(self, name):
        await self.send(["a", 0, [name, "social_pass_1", None]])
        f = await self.recv_until(lambda f: f[0] == "a")
        assert f and f[1] is None, f
        self.token = f[5]
        return f

    async def gapi(self, method, path):
        self.rid = getattr(self, "rid", 0) + 1
        await self.send(["gapi", self.rid, method, path, None])
        f = await self.recv_until(lambda f: f[0] == "gapi" and f[1] == self.rid)
        return (f[2], (f[3] or {}).get("data")) if f else (0, None)

    async def upload_a(self, name, cat, sizes, mtype=None, code=None, data="", priv=False, uvc=None):
        await self.send(["uploadA", name, "", cat, priv, data, sizes, mtype, code, uvc])
        return await self.recv_until(lambda f: f[0] in ("pubR", "upMp", "po"))

    async def close(self):
        await self.ws.close()


def listing(user="", token=None, route="/api/assets"):
    path = route + "?index=0&cat=-1" + ("&username=" + user if user else "")
    st, body, _ = http("GET", path, token=token)
    return body.decode(errors="replace")


async def scenario(names):
    sock = await Sock().open()
    await sock.register(names[0])

    print("texture:")
    tex = png(4, 4)
    r = await sock.upload_a("Brick wall", 16, {"texture": len(tex)}, None, 2)
    check(r and r[0] == "pubR" and isinstance(r[1], dict) and len(r[1].get("fileId", "")) == 32 and r[1].get("srv") is None,
          "uploadA -> pubR {fileId (32 hex), srv null}: %r" % (r,))
    fid = r[1]["fileId"]
    st, body, _ = http("POST", "/upload-user-asset?fileId=%s&key=texture&offset=0" % fid, tex[:40])
    check(st == 200 and json.loads(body).get("got") == 40, "first chunk -> 200 {got: 40}: %d %r" % (st, body))
    st, body, _ = http("POST", "/upload-user-asset?fileId=%s&key=texture&offset=0" % fid, tex[:40])
    check(st == 200, "a re-sent chunk -> 200: %d" % st)
    st, body, _ = http("POST", "/upload-user-asset?fileId=%s&key=texture&offset=60" % fid, tex[60:])
    check(st == 409, "a gap -> 409: %d %r" % (st, body))
    st, body, _ = http("POST", "/upload-user-asset?fileId=%s&key=sound&offset=0" % fid, b"x")
    check(st == 400, "an undeclared field -> 400: %d" % st)
    st, body = done(fid)
    check(st == 400 and b"incomplete" in body, "done before the whole file -> 400 Upload incomplete: %d %r" % (st, body))
    st, body, _ = http("POST", "/upload-user-asset?fileId=%s&key=texture&offset=40" % fid, tex[40:] + b"junk")
    check(st == 400, "past the declared size -> 400: %d" % st)
    st, body, _ = http("POST", "/upload-user-asset?fileId=%s&key=texture&offset=40" % fid, tex[40:])
    check(st == 200, "the rest -> 200: %d" % st)
    st, body = done(fid)
    tid = json.loads(body).get("id", 0) if st == 200 else 0
    check(st == 200 and tid >= BASE, "done -> 200 {id >= %d}: %d %r" % (BASE, st, body))
    st, got, hdr = http("GET", "/api/user-assets/%d/texture.png" % tid)
    check(st == 200 and got == tex and "no-cache" in hdr.get("Cache-Control", ""),
          "GET /api/user-assets/<id>/texture.png: the bytes, no-cache: %d %s" % (st, hdr.get("Cache-Control")))
    st, _, _ = http("GET", "/api/user-assets/%d/thumb.png" % tid)
    check(st == 404, "a file it does not have -> 404 (never Krunker's host): %d" % st)
    lst = listing()
    check(str(tid) in lst and "Brick wall" in lst, "listed by /api/assets")
    st, _ = done(fid)
    check(st == 404, "the ticket is gone after the commit: %d" % st)

    print("a failed upload, then the retry (10.0.7):")
    bad = b"\0\x01binary, not an OBJ"
    diff, emis, nrm = png(2, 2), png(2, 2, 10), png(2, 2, 20)
    sizes = {"model": len(bad), "diffuse": len(diff), "emissive": len(emis), "normal": len(nrm)}
    r = await sock.upload_a("Crate", 3, sizes, "obj")
    check(r and r[0] == "pubR", "model uploadA -> pubR: %r" % (r,))
    fid1 = r[1]["fileId"]
    for k, d in (("model", bad), ("diffuse", diff), ("emissive", emis), ("normal", nrm)):
        send_file(fid1, k, d)
    st, body = done(fid1)
    check(st == 400 and b"Invalid Model" in body, "a binary OBJ -> 400 Invalid Model: %d %r" % (st, body))
    check("Crate" not in listing(), "nothing published after the failed check")
    st, body = done(fid1)
    check(st == 400, "the ticket stays open after a failed check (done again -> 400, not 404): %d" % st)
    sizes["model"] = len(OBJ)
    r = await sock.upload_a("Crate", 3, sizes, "obj", None, "", False, {"normal": 2, "diffuse": 1, "bogus": 3})
    check(r and r[0] == "pubR" and r[1]["fileId"] != fid1, "Publish/Update again -> a new ticket: %r" % (r,))
    fid2 = r[1]["fileId"]
    st, _ = done(fid1)
    check(st == 404, "the replaced ticket answers 404: %d" % st)
    for k, d in (("model", OBJ), ("diffuse", diff), ("emissive", emis), ("normal", nrm)):
        send_file(fid2, k, d)
    st, body = done(fid2)
    mid = json.loads(body).get("id", 0) if st == 200 else 0
    check(st == 200 and mid >= BASE and mid != tid, "the retry publishes the model: %d %r" % (st, body))
    for f, d in (("model.obj", OBJ), ("model.png", diff), ("model_e.png", emis), ("model_n.png", nrm)):
        st, got, _ = http("GET", "/api/user-assets/%d/%s" % (mid, f))
        check(st == 200 and got == d, "%s stored: %d" % (f, st))
    rows = q("SELECT data, version, uvc FROM published_assets WHERE id = ?", mid)
    data = json.loads(rows[0][0]) if rows else {}
    check(set(data) == {"e", "n"}, "data: the companion map letters e, n: %r" % (data,))
    check(rows and json.loads(rows[0][2] or "{}") == {"n": 2, "c": 1}, "uvc kept by letter (c = the diffuse map): %r" % (rows,))

    print("update:")
    sizes2 = {"model": len(OBJ), "diffuse": len(diff)}
    r = await sock.upload_a("crate", 3, sizes2, "obj")
    fid3 = r[1]["fileId"]
    send_file(fid3, "model", OBJ)
    send_file(fid3, "diffuse", diff)
    st, body = done(fid3)
    j = json.loads(body) if st == 200 else {}
    check(st == 200 and j.get("id") == mid and j.get("updated") == 1, "the same name (any case) updates: %r" % (j,))
    st, _, _ = http("GET", "/api/user-assets/%d/model_e.png" % mid)
    check(st == 404, "the update replaced the files (no emissive map now): %d" % st)
    rows = q("SELECT version FROM published_assets WHERE id = ?", mid)
    check(rows and int(rows[0][0]) == 2, "version 2: %r" % (rows,))
    snd = b"ID3\x03\0\0\0\0\0\0" + b"\xff\xfb\x90\x00" + b"\0" * 64
    r = await sock.upload_a("Crate", 17, {"sound": len(snd)}, None, 3)
    fid4 = r[1]["fileId"]
    send_file(fid4, "sound", snd)
    st, body = done(fid4)
    j = json.loads(body) if st == 200 else {}
    check(st == 200 and j.get("id") not in (0, mid) and j.get("updated") == 0, "a sound of the same name is a new asset: %r" % (j,))

    print("limits:")
    r = await sock.upload_a("Big", 16, {"texture": 10 * 1000000 + 1}, None, 2)
    check(r and r[0] == "upMp" and r[1] == "File size Limit Reached", "a texture over 10 MB: %r" % (r,))
    r = await sock.upload_a("Big", 16, {"texture": 10, "thumb": 300000}, None, 2)
    check(r and r[0] == "pubR", "a premium account's 300 kB thumbnail (the signup reward's premium day): %r" % (r,))
    q("UPDATE players SET player_premium = 0")
    r = await sock.upload_a("Big", 16, {"texture": 10, "thumb": 300000}, None, 2)
    check(r and r[0] == "upMp" and r[1] == "File size Limit Reached", "a free account's 300 kB thumbnail: %r" % (r,))
    r = await sock.upload_a("Big", 3, {"model": 60 * 1000000, "diffuse": 9 * 1000000, "normal": 9 * 1000000, "emissive": 9 * 1000000,
                                       "alpha": 9 * 1000000, "bump": 9 * 1000000}, "obj")
    check(r and r[0] == "upMp" and r[1] == "File size Limit Reached", "a model over 100 MB together: %r" % (r,))
    r = await sock.upload_a("Big", 16, {"texture": 10}, None, None)
    check(r and r[0] == "upMp" and r[1] == "Invalid Category", "a model in category 16: %r" % (r,))
    r = await sock.upload_a("Big", 3, {"diffuse": 10}, "obj")
    check(r and r[0] == "upMp" and r[1] == "Missing File", "a model without its model file: %r" % (r,))
    r = await sock.upload_a("  ", 3, {"model": 10}, "obj")
    check(r and r[0] == "upMp" and r[1] == "Invalid Name", "a blank name: %r" % (r,))
    r = await sock.upload_a("Big", 3, {"model": 10}, "exe")
    check(r and r[0] == "upMp", "an unknown model type: %r" % (r,))
    guest = await Sock().open()
    r = await guest.upload_a("Big", 16, {"texture": 10}, None, 2)
    check(r and r[0] == "po", "a guest -> Must be logged in: %r" % (r,))
    await guest.close()

    print("models and prefabs:")
    r = await sock.upload_a("Ext", 3, {"model": len(GLTF_EXT)}, "gltf")
    fid5 = r[1]["fileId"]
    send_file(fid5, "model", GLTF_EXT)
    st, body = done(fid5)
    check(st == 400 and b"one file" in body, "a glTF with an external buffer -> 400: %d %r" % (st, body))
    g = glb()
    r = await sock.upload_a("Ship", 3, {"model": len(g)}, "glb")
    fid6 = r[1]["fileId"]
    send_file(fid6, "model", g)
    st, body = done(fid6)
    gid = json.loads(body).get("id", 0) if st == 200 else 0
    rows = q("SELECT data FROM published_assets WHERE id = ?", gid)
    check(st == 200 and rows and json.loads(rows[0][0]) == {"q": 1}, "a GLB passes, data {q: 1}: %d %r" % (st, rows))
    pre = json.dumps({"objects": [{"p": [0, 0, 0], "s": [10, 10, 10]}]})
    r = await sock.upload_a("Hut", 2, {"data": len(pre)}, "obj", None, pre)
    fid7 = r[1]["fileId"]
    send_file(fid7, "data", pre.encode())
    st, body = done(fid7)
    pid = json.loads(body).get("id", 0) if st == 200 else 0
    st2, got, _ = http("GET", "/api/user-assets/%d/data.json" % pid)
    check(st == 200 and st2 == 200 and got == pre.encode(), "a prefab's data.json stored: %d %d" % (st, st2))

    print("private:")
    r = await sock.upload_a("Secret", 16, {"texture": len(tex)}, None, 2, "", True)
    fid8 = r[1]["fileId"]
    send_file(fid8, "texture", tex)
    st, body = done(fid8)
    check(st == 200, "a private texture: %d" % st)
    check("Secret" not in listing(), "left out of the listing (guest)")
    check("Secret" not in listing(names[0]), "left out of the owner's username filter without the owner's session (guest)")
    check("Secret" in listing(names[0], sock.token), "listed on My Assets with the owner's session (Authorization: Bearer)")
    check("Secret" in listing(names[0], sock.token, "/webhooks/general/assets/list"), "... and on v10's webhook route")
    check("Secret" not in listing(names[0], "0" * 64), "an unknown token lists like a guest")
    other = await Sock().open()
    await other.register(names[1])
    check("Secret" not in listing(names[0], other.token), "another account's session naming the owner: not listed")
    check("Secret" not in listing("", other.token), "another account's session: not listed")
    st, got, _ = http("GET", "/api/user-assets/%d/texture.png" % json.loads(body)["id"])
    check(st == 200 and got == tex, "its files still load by id (maps that use it; Krunker loads them without credentials)")
    for who, s_, want in (("the owner", sock, True), ("another account", other, False)):
        st, d = await s_.gapi("GET", "/players/%s" % names[0])
        mine = [x.get("nm") for x in (d or {}).get("assets", [])]
        check(st == 200 and ("Secret" in mine) == want and "Brick wall" in mine,
              "profile seen by %s: Private %s: %d %r" % (who, "listed" if want else "left out", st, mine))
    guest = await Sock().open()
    st, d = await guest.gapi("GET", "/players/%s" % names[0])
    mine = [x.get("nm") for x in (d or {}).get("assets", [])]
    check(st != 200 or "Secret" not in mine, "profile seen by a guest: Private left out: %d %r" % (st, mine))
    await guest.close()
    await other.close()
    await sock.close()


def q(sql, *args):
    def lit(v):
        return "NULL" if v is None else str(int(v)) if isinstance(v, (int, bool)) else "'" + str(v).replace("'", "''") + "'"
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
    return [tuple(None if c == "" else c for c in line.split("|")) for line in out.splitlines() if line and line != "SET"]


def main():
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    dbcfg = {"driver": "sqlite", "path": DB}
    if DRIVER == "postgres":
        with open(os.environ.get("KN_TEST_CONFIG", os.path.join(ROOT, "config", "config.json"))) as fp:
            db = json.load(fp).get("database", {})
        dbname = os.environ.get("KN_TEST_DB_NAME", "krunknative_svcacc")
        PG["schema"] = "kn_test_upload_%d" % os.getpid()
        PG["env"] = dict(os.environ, PGPASSWORD=str(db.get("password", "")))
        PG["psql"] = ["psql", "-h", str(db.get("host", "127.0.0.1")), "-p", str(db.get("port", 5432)), "-U",
                      str(db.get("user", "krunknative")), "-d", dbname, "-v", "ON_ERROR_STOP=1", "-qAt"]
        try:
            subprocess.run(PG["psql"] + ["-c", "CREATE SCHEMA %s" % PG["schema"]], env=PG["env"], check=True, capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            print("PostgreSQL unavailable (%s): skip" % type(e).__name__)
            sys.exit(77)
        dbcfg = dict(db, name=dbname, search_path=PG["schema"])
    with open(cfg, "w") as f:
        json.dump({"database": dict(dbcfg, migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
                   "brain": {"host": "127.0.0.1", "port": PORT, "federation": False, "internal_secret": "upl-%d" % os.getpid()},
                   "streams": {"enabled": False},
                   "user_assets": {"upstream": "http://127.0.0.1:9/", "cache_dir": os.path.join(WORK, "ua-cache")},
                   "uploads": {"dir": UPLOADS},
                   "accounts": {"free_premium": False, "starting_kr": 0, "login_throttle_s": 0, "password_iterations": 1000,
                                "email": {"required": False},
                                "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    try:
        up = False
        for _ in range(300):
            try:
                with open(os.path.join(WORK, "brain.log")) as f:
                    if re.search(r"listening on http://127\.0\.0\.1:%d" % PORT, f.read()):
                        up = True
                        break
            except OSError:
                pass
            if brain.poll() is not None:
                break
            time.sleep(0.1)
        check(up, "kn_brain is up (%s)" % DRIVER)
        stamp = str(int(time.time()) % 100000)
        asyncio.run(scenario(["upl%s" % stamp, "upo%s" % stamp]))
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
        if PG:
            subprocess.run(PG["psql"] + ["-c", "DROP SCHEMA %s CASCADE" % PG["schema"]], env=PG["env"], capture_output=True, timeout=60)
    if FAILS:
        print("FAILED (%d): log in %s" % (len(FAILS), WORK))
        sys.exit(1)
    shutil.rmtree(WORK, ignore_errors=True)
    print("all ok")


if __name__ == "__main__":
    main()

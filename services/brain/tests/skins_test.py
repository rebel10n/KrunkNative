#!/usr/bin/env python3
"""Krunker v10.0.7 Skin Manager backend: the /skins gapi routes over the social socket and the same routes over
HTTP on the brain root (services/brain/skins.c P_CUSTOM_SKIN, v10_catalog.c, 0394_custom_skins), on a kn_brain
with its own sqlite database (never config/config.json).

  skins_test.py <kn_brain> <repo root>
  KN_TEST_DB_DRIVER=postgres: a scratch schema in krunknative_svcacc (config/config.json credentials,
  or the file KN_TEST_CONFIG names); mysql: a scratch database on KN_TEST_MYSQL_HOST/PORT/USER/PASSWORD
  (default 127.0.0.1:3306 root)

Covers what v10.0.7's Skin Manager window calls (deob-10.0.7.js:26551-26604 the client, :124350-124511 the
service): list (page, page_size, type, weapon, search by name or id, newest first, {skins, total}), get,
create (defaults, name / type / config rules, 200 per account), update (partial), duplicate (" (Copy)"),
delete, ownership (another account's skin is 404) and guests (401); uploadAsset (asset_upload.c): the gapi key then
chunks to /skins/upload, v10's own FormData POST over HTTP, each file where the game loads it (skins/<skinId>/<file>,
served by /api/user-assets/skins/...), the checks (type, category, size, content, owner), a model of another format
replacing the old one, a duplicate's copy of the files and a delete removing them; ids from 1000000000 (0395).
77 = skip (no kn_brain / websockets / database server).
"""
import asyncio
import itertools
import json
import os
import re
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import struct
import time
import urllib.error
import urllib.request
import zlib

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
WORK = tempfile.mkdtemp(prefix="kn-skins.", dir=os.path.expanduser("~/.cache"))
DB = os.path.join(WORK, "coordinator.db")
RID = itertools.count(1)
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
PG = {}
MY = {}


def check(cond, what, detail=""):
    print(("  ok   " if cond else "  FAIL ") + what + (("  -- " + str(detail)) if detail and not cond else ""), flush=True)
    if not cond:
        FAILS.append(what)


def lit(v):
    return str(int(v)) if isinstance(v, (int, bool)) else "'" + str(v).replace("'", "''") + "'"


def q(sql, *args):
    parts = sql.split("?")
    text = "".join(p + (lit(args[i]) if i < len(args) else "") for i, p in enumerate(parts))
    if DRIVER == "mysql":
        out = subprocess.run(MY["cmd"] + ["-N", "-B", "-e", text], env=MY["env"], capture_output=True, text=True, check=True).stdout
        return [tuple(None if c == "NULL" else int(c) if re.fullmatch(r"-?\d+", c) else c for c in line.split("\t"))
                for line in out.splitlines() if line]
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
        if not line or line == "SET" or line.startswith("INSERT ") or line.startswith("UPDATE ") or line.startswith("DELETE "):
            continue
        rows.append(tuple(None if c == "" else int(c) if re.fullmatch(r"-?\d+", c) else c for c in line.split("|")))
    return rows


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
        await self.send("a", 0, [name, "skins_pass_1", None])
        f = await self.wait(lambda f: f[0] == "a")
        assert f and f[1] is None, f
        self.id, self.name = f[2], name
        return f[2]

    async def gapi(self, method, path, body=None):
        rid = next(RID)
        await self.send("gapi", rid, method, path, body)
        f = await self.wait(lambda f: f[0] == "gapi" and f[1] == rid)
        return (f[2], (f[3] or {}).get("data"), (f[3] or {}).get("error")) if f else (0, None, None)

    async def close(self):
        await self.ws.close()


def png(w=2, h=2):
    raw = b"".join(b"\0" + b"\x80\x40\x20\xff" * w for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


MP3 = b"ID3\x03\0\0\0\0\0\0" + b"\xff\xfb\x90\x00" + b"\0" * 64
WAV = b"RIFF\x24\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\x44\xac\0\0\x88\x58\x01\0\x02\0\x10\0data\0\0\0\0"
NOT_VORBIS = b"OggS\0\x02" + b"\0" * 58  # an Ogg page with no Vorbis header
OBJ = b"v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n"


def glb():
    js = json.dumps({"asset": {"version": "2.0"}, "buffers": [{"byteLength": 4}]}).encode()
    js += b" " * (-len(js) % 4)
    bin_ = b"\0\0\0\0"
    body = struct.pack("<I4s", len(js), b"JSON") + js + struct.pack("<I4s", len(bin_), b"BIN\0") + bin_
    return struct.pack("<4sII", b"glTF", 2, 12 + len(body)) + body


def raw(port, method, path, data=b"", ctype="application/octet-stream", token=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method)
    req.add_header("Content-Type", ctype)
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def form(fields, file_name, data):
    b = "----knskins%d" % os.getpid()
    out = b""
    for k, v in fields.items():
        out += ("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n" % (b, k, v)).encode()
    out += ("--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\nContent-Type: application/octet-stream\r\n\r\n"
            % (b, file_name)).encode() + data + ("\r\n--%s--\r\n" % b).encode()
    return out, "multipart/form-data; boundary=" + b


async def upload(sock, port, skin, asset_type, data, category=None, file_name="file.bin", chunk=7):
    """the native client's way: the gapi key, the chunks, done -> (status, json body)"""
    body = {"asset_type": asset_type, "fileName": file_name, "size": len(data)}
    if category:
        body["category"] = category
    st, d, err = await sock.gapi("POST", "/skins/%d/upload" % skin, body)
    if st != 200:
        return st, {"error": err}
    url = d["uploadUrl"]
    for at in range(0, len(data), chunk):
        cst, _ = raw(port, "POST", "%s?fileId=%s&key=file&offset=%d" % (url, d["fileId"], at), data[at:at + chunk])
        if cst != 200:
            return cst, None
    st, out = raw(port, "POST", "%s?fileId=%s&done=1" % (url, d["fileId"]))
    try:
        return st, json.loads(out or b"null")
    except ValueError:
        return st, out


def code(err):
    return (err or {}).get("code")


def http(port, method, path, body=None, token=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method)
    req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    anon = await Sock.open(port)
    for method, path in (("GET", "/skins"), ("GET", "/skins/1"), ("POST", "/skins"), ("DELETE", "/skins/1")):
        st, data, err = await anon.gapi(method, path, {"name": "x"} if method == "POST" else None)
        check(st == 401 and code(err) == "UNAUTHORIZED", "a guest: %s %s is 401 UNAUTHORIZED" % (method, path), (st, err))
    await anon.close()

    a, b = await Sock.open(port), await Sock.open(port)
    await a.auth("skA" + stamp)
    await b.auth("skB" + stamp)

    st, data, err = await a.gapi("GET", "/skins?page=1&page_size=10")
    check(st == 200 and data == {"skins": [], "total": 0, "page": 1, "page_size": 10}, "list: empty at first {skins, total}", (st, data, err))

    # ---- create: the window's new skin is {skinId: null, name: "New Skin", type: 0, weapon: 1, rarity: 0, itemConfig: {}}
    st, data, err = await a.gapi("POST", "/skins", {"skinId": None, "name": "New Skin", "type": 0, "weapon": 1, "rarity": 0, "itemConfig": {}})
    s1 = (data or {}).get("skin") or {}
    check(st == 200 and isinstance(s1.get("skinId"), int) and s1["name"] == "New Skin" and s1["type"] == 0 and s1["weapon"] == 1
          and s1["rarity"] == 0 and s1["itemConfig"] == {} and re.match(r"\d{4}-\d\d-\d\dT", s1.get("created_at") or ""),
          "create: {skin} with skinId, the fields and created_at", (st, data, err))
    for body, want, what in (({"name": "   "}, "SKIN_NAME_INVALID", "a blank name"),
                             ({"name": "x" * 65}, "SKIN_NAME_INVALID", "a 65-character name"),
                             ({"name": "Ok", "type": 19}, "SKIN_TYPE_INVALID", "type 19"),
                             ({"name": "Ok", "rarity": 8}, "SKIN_TYPE_INVALID", "rarity 8"),
                             ({"name": "Ok", "itemConfig": [1, 2]}, "SKIN_CONFIG_INVALID", "an itemConfig that is no object"),
                             ({"name": "Ok", "itemConfig": {"k": "x" * 33000}}, "SKIN_CONFIG_INVALID", "an itemConfig over 32 KB")):
        st, data, err = await a.gapi("POST", "/skins", body)
        check(st == 400 and code(err) == want, "create: %s is refused (%s)" % (what, want), (st, err))
    cfg = {"copyId": 12, "tex": "abc", "glow": True}
    st, data, err = await a.gapi("POST", "/skins", {"name": "  Hat 50%_off  ", "type": 1, "weapon": 0, "rarity": 4, "itemConfig": cfg})
    s2 = (data or {}).get("skin") or {}
    check(st == 200 and s2.get("name") == "Hat 50%_off" and s2["type"] == 1 and s2["rarity"] == 4 and s2["itemConfig"] == cfg,
          "create: the name is trimmed, the itemConfig kept", (st, data, err))
    st, data, err = await a.gapi("POST", "/skins", {"name": "Sniper Glow", "type": 0, "weapon": 1, "rarity": 2})
    s3 = (data or {}).get("skin") or {}

    # ---- list: newest first, filters, search, pages
    st, data, err = await a.gapi("GET", "/skins")
    check(st == 200 and data["total"] == 3 and [s["skinId"] for s in data["skins"]] == [s3["skinId"], s2["skinId"], s1["skinId"]],
          "list: every skin of the account, newest first", data)
    st, data, err = await b.gapi("GET", "/skins")
    check(st == 200 and data["total"] == 0 and data["skins"] == [], "list: another account sees none of them", data)
    st, data, err = await a.gapi("GET", "/skins?type=0")
    check(st == 200 and data["total"] == 2 and all(s["type"] == 0 for s in data["skins"]), "list: type=0", data)
    st, data, err = await a.gapi("GET", "/skins?type=0&weapon=1")
    check(st == 200 and data["total"] == 2, "list: type=0&weapon=1", data)
    st, data, err = await a.gapi("GET", "/skins?weapon=2")
    check(st == 200 and data["total"] == 0, "list: weapon=2 matches none", data)
    st, data, err = await a.gapi("GET", "/skins?search=SNIPER")
    check(st == 200 and [s["skinId"] for s in data["skins"]] == [s3["skinId"]], "list: search by name in any letter case", data)
    st, data, err = await a.gapi("GET", "/skins?search=50%25_")
    check(st == 200 and [s["skinId"] for s in data["skins"]] == [s2["skinId"]], "list: % and _ in a search are literal", data)
    st, data, err = await a.gapi("GET", "/skins?search=_")
    check(st == 200 and [s["skinId"] for s in data["skins"]] == [s2["skinId"]], "list: a lone _ is no wildcard", data)
    st, data, err = await a.gapi("GET", "/skins?search=%23" + str(s1["skinId"]))
    check(st == 200 and s1["skinId"] in [s["skinId"] for s in data["skins"]], "list: search by #id (\"Search by name or ID...\")", data)
    st, data, err = await a.gapi("GET", "/skins?page=2&page_size=2")
    check(st == 200 and data["total"] == 3 and [s["skinId"] for s in data["skins"]] == [s1["skinId"]] and data["page"] == 2
          and data["page_size"] == 2, "list: page 2 of size 2", data)
    st, data, err = await a.gapi("GET", "/skins?page_size=500")
    check(st == 200 and data["page_size"] == 50, "list: page_size is at most 50", data and data.get("page_size"))

    # ---- get, ownership
    st, data, err = await a.gapi("GET", "/skins/%d" % s2["skinId"])
    check(st == 200 and data["skin"]["skinId"] == s2["skinId"] and data["skin"]["itemConfig"] == cfg, "get: {skin}", (st, data, err))
    st, data, err = await b.gapi("GET", "/skins/%d" % s2["skinId"])
    check(st == 404 and code(err) == "SKIN_NOT_FOUND", "get: another account's skin is 404", (st, err))
    st, data, err = await a.gapi("GET", "/skins/abc")
    check(st == 404, "get: a non-numeric id is 404", (st, err))
    st, data, err = await a.gapi("GET", "/skins/99999999")
    check(st == 404 and code(err) == "SKIN_NOT_FOUND", "get: an unknown id is 404", (st, err))

    # ---- update: the editor's Save sends the whole skin; missing fields keep theirs
    st, data, err = await a.gapi("PUT", "/skins/%d" % s3["skinId"], {"skinId": s3["skinId"], "name": "Sniper Glow 2", "type": 0, "weapon": 3,
                                                                    "rarity": 5, "itemConfig": {"copyId": 4}})
    u = (data or {}).get("skin") or {}
    check(st == 200 and u.get("name") == "Sniper Glow 2" and u["weapon"] == 3 and u["rarity"] == 5 and u["itemConfig"] == {"copyId": 4},
          "update: {skin} with the new fields", (st, data, err))
    st, data, err = await a.gapi("PUT", "/skins/%d" % s3["skinId"], {"rarity": 1})
    u = (data or {}).get("skin") or {}
    check(st == 200 and u.get("name") == "Sniper Glow 2" and u["weapon"] == 3 and u["rarity"] == 1 and u["itemConfig"] == {"copyId": 4},
          "update: a partial body keeps the other fields", (st, data, err))
    st, data, err = await a.gapi("PUT", "/skins/%d" % s3["skinId"], {"name": ""})
    check(st == 400 and code(err) == "SKIN_NAME_INVALID", "update: an empty name is refused", (st, err))
    st, data, err = await b.gapi("PUT", "/skins/%d" % s3["skinId"], {"name": "Mine now"})
    check(st == 404 and code(err) == "SKIN_NOT_FOUND", "update: another account's skin is 404", (st, err))
    check(q("SELECT name FROM custom_skins WHERE id = ?", s3["skinId"]) == [("Sniper Glow 2",)], "update: the other account changed nothing", None)

    # ---- duplicate
    st, data, err = await a.gapi("POST", "/skins/%d/duplicate" % s2["skinId"])
    d = (data or {}).get("skin") or {}
    check(st == 200 and d.get("skinId") not in (None, s2["skinId"]) and d["name"] == "Hat 50%_off (Copy)" and d["type"] == 1
          and d["rarity"] == 4 and d["itemConfig"] == cfg, "duplicate: a new skin named \"<name> (Copy)\" with the same fields", (st, data, err))
    st, data, err = await a.gapi("PUT", "/skins/%d" % s1["skinId"], {"name": "y" * 64})
    st, data, err = await a.gapi("POST", "/skins/%d/duplicate" % s1["skinId"])
    d64 = ((data or {}).get("skin") or {}).get("name") or ""
    check(st == 200 and len(d64) == 64 and d64.endswith(" (Copy)"), "duplicate: a 64-character name is shortened to fit \" (Copy)\"", (st, d64, err))
    st, data, err = await b.gapi("POST", "/skins/%d/duplicate" % s2["skinId"])
    check(st == 404 and code(err) == "SKIN_NOT_FOUND", "duplicate: another account's skin is 404", (st, err))

    # ---- uploads (asset_upload.c): the files of a skin where v10's game loads them
    check(s1["skinId"] >= 1000000000, "ids: from 1000000000, a Box Office skin's skins/<id>/ folder is the brain's", s1["skinId"])
    sid = s2["skinId"]
    tex = png(4, 4)
    st, body = await upload(a, port, sid, "texture", tex, "diffuse", "skin.png")
    check(st == 200 and (body or {}).get("data", {}).get("asset_path") == "skins/%d/texture.png" % sid,
          "upload: a diffuse texture -> {data: {asset_path: skins/<id>/texture.png}}", (st, body))
    st, got = raw(port, "GET", "/api/user-assets/skins/%d/texture.png" % sid, None)
    check(st == 200 and got == tex, "upload: GET /api/user-assets/skins/<id>/texture.png serves it", (st, len(got or b"")))
    for asset_type, category, data, fname, want in (("texture", "emissive", png(), "g.png", "texture_e.png"),
                                                   ("texture", "wear", png(), "w.png", "texture_w.png"),
                                                   ("texture", "mask", png(), "p.png", "texture_p.png"),
                                                   ("sound", None, MP3, "shot.mp3", "sound.mp3"),
                                                   ("sound", "swap", WAV, "s.wav", "sound_swap.mp3"),
                                                   ("sound", "inspect", MP3, "i.mp3", "sound_inspect.mp3"),
                                                   ("thumbnail", None, png(8, 8), "thumb.png", "thumbnail.png"),
                                                   ("model", None, OBJ, "Gun.OBJ", "model.obj")):
        st, body = await upload(a, port, sid, asset_type, data, category, fname)
        check(st == 200 and (body or {}).get("data", {}).get("asset_path") == "skins/%d/%s" % (sid, want),
              "upload: %s %s -> %s" % (asset_type, category or "(default)", want), (st, body))
    st, body = await upload(a, port, sid, "model", glb(), None, "gun.glb")
    st1, _ = raw(port, "GET", "/api/user-assets/skins/%d/model.glb" % sid, None)
    st2, _ = raw(port, "GET", "/api/user-assets/skins/%d/model.obj" % sid, None)
    check(st == 200 and st1 == 200 and st2 == 404, "upload: a GLB model replaces the OBJ (one model per skin)", (st, st1, st2))
    for asset_type, category, data, fname, want_st, what in (
            ("texture", "normal", png(), "n.png", 400, "an unknown texture category"),
            ("model", None, OBJ, "gun.exe", 400, "a model of no model format"),
            ("video", None, png(), "v.mp4", 400, "an unknown asset type")):
        st, body = await upload(a, port, sid, asset_type, data, category, fname)
        check(st == want_st and code((body or {}).get("error")) == "SKIN_UPLOAD_TYPE", "upload: %s is refused" % what, (st, body))
    st, d, err = await a.gapi("POST", "/skins/%d/upload" % sid, {"asset_type": "thumbnail", "size": 2000000})
    check(st == 400 and code(err) == "SKIN_UPLOAD_SIZE", "upload: a thumbnail over 1 MB is refused at the key", (st, err))
    st, body = await upload(a, port, sid, "texture", b"<html>not a png</html>", "diffuse", "x.png")
    check(st == 400, "upload: a texture that is no image fails its check at done", (st, body))
    # v10's drop zone takes .ogg (deob-10.0.7.js:127038): an Ogg Vorbis file is stored under sound_inspect.mp3, as v10's
    # game loads it (the client decodes it by content: miniaudio + stb_vorbis)
    ogg = open(os.path.join(ROOT, "client", "tests", "data", "tone_440.ogg"), "rb").read()
    st, body = await upload(a, port, sid, "sound", ogg, "inspect", "i.ogg")
    st1, got = raw(port, "GET", "/api/user-assets/skins/%d/sound_inspect.mp3" % sid, None)
    check(st == 200 and (body or {}).get("data", {}).get("asset_path") == "skins/%d/sound_inspect.mp3" % sid and st1 == 200
          and got == ogg, "upload: an Ogg Vorbis sound is stored as sound_inspect.mp3", (st, body, st1))
    st, body = await upload(a, port, sid, "sound", NOT_VORBIS, "inspect", "i.ogg")
    check(st == 400 and "MP3, WAV or OGG" in json.dumps(body), "upload: an Ogg file with no Vorbis stream is refused at done",
          (st, body))
    st, d, err = await b.gapi("POST", "/skins/%d/upload" % sid, {"asset_type": "thumbnail", "size": 100})
    check(st == 404 and code(err) == "SKIN_NOT_FOUND", "upload: another account's skin is 404", (st, err))
    # the skin deleted between the key and done: nothing is stored
    st, d, err = await a.gapi("POST", "/skins", {"name": "Doomed"})
    doomed = d["skin"]["skinId"]
    st, k, err = await a.gapi("POST", "/skins/%d/upload" % doomed, {"asset_type": "thumbnail", "size": len(tex)})
    raw(port, "POST", "%s?fileId=%s&key=file&offset=0" % (k["uploadUrl"], k["fileId"]), tex)
    await a.gapi("DELETE", "/skins/%d" % doomed)
    st, out = raw(port, "POST", "%s?fileId=%s&done=1" % (k["uploadUrl"], k["fileId"]))
    st2, _ = raw(port, "GET", "/api/user-assets/skins/%d/thumbnail.png" % doomed, None)
    check(st == 404 and st2 == 404, "upload: a skin deleted before done stores nothing (404)", (st, out, st2))
    # a duplicate copies the files
    st, data, err = await a.gapi("POST", "/skins/%d/duplicate" % sid)
    dup = ((data or {}).get("skin") or {}).get("skinId", 0)
    st1, got = raw(port, "GET", "/api/user-assets/skins/%d/texture.png" % dup, None)
    check(st == 200 and st1 == 200 and got == tex, "duplicate: the copy has the original's files", (st, st1))
    st, data, err = await a.gapi("DELETE", "/skins/%d" % dup)
    st1, _ = raw(port, "GET", "/api/user-assets/skins/%d/texture.png" % dup, None)
    check(st == 200 and st1 == 404, "delete: the skin's files go with it", (st, st1))

    # ---- delete
    st, data, err = await b.gapi("DELETE", "/skins/%d" % s2["skinId"])
    check(st == 404 and code(err) == "SKIN_NOT_FOUND", "delete: another account's skin is 404", (st, err))
    st, data, err = await a.gapi("DELETE", "/skins/%d" % s2["skinId"])
    check(st == 200 and data == {"deleted": True, "skinId": s2["skinId"]}, "delete: {deleted, skinId}", (st, data, err))
    st, data, err = await a.gapi("DELETE", "/skins/%d" % s2["skinId"])
    check(st == 404 and code(err) == "SKIN_NOT_FOUND", "delete: twice is 404", (st, err))
    st, data, err = await a.gapi("GET", "/skins")
    check(st == 200 and data["total"] == 4 and s2["skinId"] not in [s["skinId"] for s in data["skins"]], "delete: gone from the list", data)

    # ---- 200 per account (rows past the 60 writes a minute go in through the database)
    for i in range(195):
        q("INSERT INTO custom_skins (owner_id, name, item_config) VALUES (?, ?, '{}')", b.id, "Filler %d" % i)
    for i in range(5):
        st, data, err = await b.gapi("POST", "/skins", {"name": "B %d" % i})
    check(st == 200, "create: the 200th skin", (st, err))
    st, data, err = await b.gapi("POST", "/skins", {"name": "B 201"})
    check(st == 400 and code(err) == "SKIN_LIMIT", "create: 200 skins per account", (st, err))
    st, data, err = await b.gapi("POST", "/skins/%d/duplicate" % q("SELECT max(id) FROM custom_skins WHERE owner_id = ?", b.id)[0][0])
    check(st == 400 and code(err) == "SKIN_LIMIT", "duplicate: the limit holds too", (st, err))
    st, data, err = await b.gapi("GET", "/skins?page=20&page_size=10")
    check(st == 200 and data["total"] == 200 and len(data["skins"]) == 10, "list: the 20th page of 200", data and data.get("total"))

    # ---- the same routes over HTTP on the brain root (v10_catalog.c), Bearer = a session token
    st, body = http(port, "GET", "/skins")
    check(st == 401 and isinstance(body, dict) and (body.get("error") or {}).get("code") == "UNAUTHORIZED",
          "HTTP: a guest's list is 401 {error: {code, message}}", (st, body))
    st, body = http(port, "POST", "/auth/register", {"username": "skH" + stamp, "password": "skins_pass_1"})
    tok = (body or {}).get("token") or ((body or {}).get("data") or {}).get("token")
    check(bool(tok), "HTTP: register for a token", (st, body))
    st, body = http(port, "POST", "/skins", {"name": "Http Skin", "type": 3, "rarity": 1}, token=tok)
    hs = ((body or {}).get("data") or {}).get("skin") or {}
    check(st == 200 and hs.get("name") == "Http Skin" and hs.get("type") == 3, "HTTP: create -> {data: {skin}}", (st, body))
    st, body = http(port, "GET", "/skins?search=http", token=tok)
    check(st == 200 and ((body or {}).get("data") or {}).get("total") == 1, "HTTP: list -> {data: {skins, total}}", (st, body))
    st, body = http(port, "PUT", "/skins/%d" % hs.get("skinId", 0), {"name": ""}, token=tok)
    check(st == 400 and isinstance((body or {}).get("error"), str) and "1 to 64" in body["error"],
          "HTTP: a refused save is a text error (the service throws new Error(res.error))", (st, body))
    st, body = http(port, "GET", "/skins/%d" % s3["skinId"], token=tok)
    check(st == 404 and isinstance((body or {}).get("error"), str), "HTTP: another account's skin is 404", (st, body))
    st, body = http(port, "POST", "/skins/%d/duplicate" % hs.get("skinId", 0), {}, token=tok)
    check(st == 200 and (((body or {}).get("data") or {}).get("skin") or {}).get("name") == "Http Skin (Copy)", "HTTP: duplicate", (st, body))
    # v10's own uploadAsset: one FormData POST {file, asset_type, category}
    data_, ctype = form({"asset_type": "texture", "category": "emissive"}, "glow.png", tex)
    st, out = raw(port, "POST", "/skins/%d/upload" % hs.get("skinId", 0), data_, ctype, token=tok)
    check(st == 200 and json.loads(out).get("data", {}).get("asset_path") == "skins/%d/texture_e.png" % hs.get("skinId", 0),
          "HTTP: the FormData upload -> {data: {asset_path}}", (st, out[:200]))
    st1, got = raw(port, "GET", "/user-assets/skins/%d/texture_e.png" % hs.get("skinId", 0), None)
    check(st1 == 200 and got == tex, "HTTP: the uploaded file is served", st1)
    data_, ctype = form({"asset_type": "texture"}, "x.png", tex)
    st, out = raw(port, "POST", "/skins/%d/upload" % s3["skinId"], data_, ctype, token=tok)
    check(st == 404 and isinstance(json.loads(out).get("error"), str), "HTTP: an upload to another account's skin is 404", (st, out[:200]))
    st, out = raw(port, "POST", "/skins/%d/upload" % hs.get("skinId", 0), b"{}", "application/json", token=tok)
    check(st == 400, "HTTP: an upload without a file is 400", (st, out[:200]))
    st, out = raw(port, "POST", "/skins/%d/upload" % hs.get("skinId", 0), data_, ctype)
    check(st == 401, "HTTP: a guest's upload is 401", st)
    st, body = http(port, "DELETE", "/skins/%d" % hs.get("skinId", 0), token=tok)
    check(st == 200 and ((body or {}).get("data") or {}).get("deleted") is True, "HTTP: delete", (st, body))

    for s in (a, b):
        await s.close()


def main():
    (port,) = free_ports(1)
    log = open(os.path.join(WORK, "brain.log"), "w")
    cfg = os.path.join(WORK, "brain.json")
    dbcfg = {"driver": "sqlite", "path": DB}
    if DRIVER == "postgres":
        with open(os.environ.get("KN_TEST_CONFIG", os.path.join(ROOT, "config", "config.json"))) as fp:
            db = json.load(fp).get("database", {})
        dbname = os.environ.get("KN_TEST_DB_NAME", "krunknative_svcacc")
        PG["schema"] = "kn_test_skins_%d" % os.getpid()
        PG["env"] = dict(os.environ, PGPASSWORD=str(db.get("password", "")))
        PG["psql"] = ["psql", "-h", str(db.get("host", "127.0.0.1")), "-p", str(db.get("port", 5432)), "-U",
                      str(db.get("user", "krunknative")), "-d", dbname, "-v", "ON_ERROR_STOP=1", "-qAt"]
        try:
            subprocess.run(PG["psql"] + ["-c", "CREATE SCHEMA %s" % PG["schema"]], env=PG["env"], check=True, capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            print("PostgreSQL unavailable (%s): skip" % e)
            sys.exit(77)
        dbcfg = dict(db, name=dbname, search_path=PG["schema"])
    elif DRIVER == "mysql":
        tool = shutil.which("mariadb") or shutil.which("mysql")
        host, mport = os.environ.get("KN_TEST_MYSQL_HOST", "127.0.0.1"), os.environ.get("KN_TEST_MYSQL_PORT", "3306")
        user, password = os.environ.get("KN_TEST_MYSQL_USER", "root"), os.environ.get("KN_TEST_MYSQL_PASSWORD", "")
        MY["name"] = "kn_test_skins_%d" % os.getpid()
        MY["env"] = dict(os.environ, MYSQL_PWD=password)
        base = [tool or "mariadb", "--no-defaults", "--protocol=TCP", "-h", host, "-P", mport, "-u", user]
        try:
            subprocess.run(base + ["-e", "CREATE DATABASE %s CHARACTER SET utf8mb4" % MY["name"]], env=MY["env"], check=True,
                           capture_output=True, timeout=30)
        except (OSError, subprocess.SubprocessError) as e:
            print("MySQL/MariaDB unavailable (%s): skip" % e)
            sys.exit(77)
        MY["base"], MY["cmd"] = base, base + [MY["name"]]
        dbcfg = {"driver": "mysql", "host": host, "port": int(mport), "user": user, "password": password, "name": MY["name"]}
    with open(cfg, "w") as f:
        json.dump({"database": dict(dbcfg, migrations_dir=os.path.join(ROOT, "services", "db", "migrations")),
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": "skins-%d" % os.getpid()},
                   "uploads": {"dir": os.path.join(WORK, "uploads")},
                   "accounts": {"free_premium": False, "starting_kr": 0, "login_throttle_s": 0, "password_iterations": 1000,
                                "skins_file": os.path.join(ROOT, "assets", "skins.json")}}, f)
    brain = subprocess.Popen([BRAIN, "--config", cfg, "--migrate"], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
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
            subprocess.run(PG["psql"] + ["-c", "DROP SCHEMA IF EXISTS %s CASCADE" % PG["schema"]], env=PG["env"], capture_output=True)
        if MY:
            subprocess.run(MY["base"] + ["-e", "DROP DATABASE IF EXISTS %s" % MY["name"]], env=MY["env"], capture_output=True)
    print("skins test: " + ("OK" if not FAILS else "%d failure(s), files in %s" % (len(FAILS), WORK)))
    if not FAILS and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

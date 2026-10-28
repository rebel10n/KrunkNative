#!/usr/bin/env python3
"""The v10 REST data routes on kn_brain at the root (services/brain/v10_api.c, v10_pack.c).

  v10_api_data_test.py <kn_brain | build dir> [repo root]

A scratch sqlite database (db_scratch.py), kn_brain --migrate-only, then kn_brain on an ephemeral port.
Stdlib only. One check per route, each printing PASS or FAIL:

  GET /maps                  {data: 47 official rows in mapIndex order} (v10.js 26581, 222889)
  GET /maps/0                {data: the Burg map JSON, its objects as in assets/maps/official} (26586)
  GET /maps/47, /maps/x      404 {error: {code: MAP_NOT_FOUND}} (no such official map)
  GET /maps/by-name/burg     {data: the Burg map} case-insensitive (26592); unknown name 404
  GET /titles                {data: the 111 titles of assets/titles.json} (27518, 103393)
  GET /data/skins            the packed table of assets/skins.json; decoded with a port of v10.js
                             unpack (27906-27985) it is the file's array, in order (28035)
  GET /data/spins            the packed wheels; decoded it equals GET /api/data/spins (102764)

Exit 0 when every check passes, 1 otherwise, 77 when kn_brain or the sqlite scratch database is
unavailable (CTest's skip).
"""
import json
import os
import re
import shutil
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
NAME = "kn_test_v10api_%d" % os.getpid()
SECRET = "kn-v10api-test-secret-%d" % os.getpid()
RESULTS = []


def check(name, ok, detail=""):
    RESULTS.append(bool(ok))
    print(("PASS  " if ok else "FAIL  ") + name + ("" if ok or not detail else "  -- " + detail), flush=True)


def read_wait(path, secs=20):
    end = time.time() + secs
    while time.time() < end:
        try:
            with open(path) as fp:
                text = fp.read()
        except OSError:
            text = ""
        m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", text.split("== start ")[-1])
        if m:
            return int(m.group(1))
        time.sleep(0.1)
    return None


def http(port, path):
    """(status, content type, body bytes) of a GET."""
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), method="GET")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, r.headers.get("Content-Type", ""), r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers.get("Content-Type", ""), e.read()


def jget(port, path):
    status, ctype, raw = http(port, path)
    try:
        return status, json.loads(raw or b"null")
    except ValueError:
        return status, None


def v10_unpack(text):
    """A port of v10.js unpack (27906-27985): dictionaries split on '^', the tree on '|$@]'."""
    parts = text.split("^")
    unesc_map = {"+": " ", "%2B": "+", "%7C": "|", "%5E": "^", "%25": "%"}

    def unesc(s):
        return re.sub(r"\+|%2B|%7C|%5E|%25", lambda m: unesc_map[m.group(0)], s)

    items = []
    if parts[0] != "":
        items.extend(unesc(x) for x in parts[0].split("|"))
    if parts[1] != "":
        items.extend(int(x, 36) for x in parts[1].split("|"))
    if parts[2] != "":
        items.extend(float(x) for x in parts[2].split("|"))
    toks, cur = [], ""
    for ch in parts[3]:
        if ch in "|$@]":
            if cur:
                toks.append(int(cur, 36))
                cur = ""
            if ch != "|":
                toks.append(ch)
        else:
            cur += ch
    if cur:
        toks.append(int(cur, 36))
    pos = [0]

    def leaf(t):
        if t == -1:
            return True
        if t == -2:
            return False
        if t in (-3, -5):
            return None
        if t == -4:
            return ""
        return items[t]

    def read():
        t = toks[pos[0]]
        pos[0] += 1
        if t == "@":
            arr = []
            while True:
                tk = toks[pos[0]]
                if tk == "]":
                    return arr
                arr.append(read() if tk in ("@", "$") else leaf(tk))
                pos[0] += 1
        if t == "$":
            obj = {}
            while True:
                k = toks[pos[0]]
                if k == "]":
                    return obj
                key = "" if k == -4 else items[k]
                pos[0] += 1
                v = toks[pos[0]]
                obj[key] = read() if v in ("@", "$") else leaf(v)
                pos[0] += 1
        raise ValueError("bad tree token %r" % (t,))

    return read()


FOLDERS = ["weapons/weapon_", "hats/hat_", "body/body_", "melee/melee_", "sprays/", "dyes/", "waist/waist_",
           "faces/face_", "shoes/shoe_", "pets/pet_", "collectibles/collect_", "wrist/wrist_", "charms/charm_",
           "tickets/ticket_", "back/back_", "head/head_", "playercards/card_", "emoji/emoji_", "paints/paint_"]


def truthy(v):
    """a JSON value JavaScript reads as true (None is both a missing key and null here)"""
    if v is None or v is False:
        return False
    if isinstance(v, bool):
        return v
    if isinstance(v, (int, float)):
        return v != 0
    if isinstance(v, str):
        return v != ""
    return True


def js(v):
    """the text JavaScript's + gives a value (a missing key is "undefined")"""
    if v is None:
        return "undefined"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float) and v.is_integer():
        return str(int(v))
    return str(v)


def thumb_path(r):
    """The thumbnail the v10.0.3 client gives a skin (deob-10.0.3.js: path builder 1051172, store loop 4804884;
    doc/v10/skin-thumbnails.md). The store loop sets it on every record, so it is the path of each row."""
    t, i, w = r.get("type"), r.get("id"), r.get("weapon")
    tex, pat, mid, midT, at = r.get("tex"), r.get("pat"), r.get("mid"), r.get("midT"), r.get("assetType")
    if not truthy(t) or t not in (4, 16, 17, 18):
        if truthy(t) and t != 3:
            tail = "cosmetics/" + js(at if truthy(at) else t) + "_" + js(i)
            tail += ("_c" + js(pat)) if pat is not None else ("_" + js(tex) if truthy(tex) else "")
        else:
            idx = at if truthy(at) else (t if truthy(t) else 0)
            folder = FOLDERS[idx] if isinstance(idx, int) and 0 <= idx < len(FOLDERS) else "undefined"
            if truthy(t) and t == 3:
                tail = folder + js(i) + (("_c" + js(pat)) if pat is not None else (("_" + js(tex)) if tex is not None else ""))
            else:
                if mid is not None:
                    last = "m" + js(mid) + (("_" + js(midT)) if midT is not None else "")
                elif pat is not None:
                    last = "c" + js(pat)
                else:
                    last = js(tex) if truthy(tex) else js(i)
                tail = folder + js(w if truthy(w) else 0) + "_" + last
        p = "previews/" + tail
    else:
        p = FOLDERS[t] + js(i)
    ext = r.get("prevExt") if truthy(r.get("prevExt")) else ("webp" if t in (16, 17) else r.get("texExt"))
    return "/textures/" + p + "." + (ext if truthy(ext) else "png")


def skins_as_sent(rows):
    """The rows GET /data/skins sends for assets/skins.json (services/brain/v10_api.c h_data_skins): each video
    skin (`video` set, no truthy `asset`) without `video`, `projection` and `texSrc` (v10_skin_still.c), and
    every row with its `thumbnail` = thumb_path(row) (v10_skin_thumb.c). Nothing else changes."""
    out = []
    for r in rows:
        r = dict(r)
        if r.get("video") is not None and not truthy(r.get("asset")):
            for k in ("video", "projection", "texSrc"):
                r.pop(k, None)
        r["thumbnail"] = thumb_path(r)
        out.append(r)
    return out


def load_json(rel):
    with open(os.path.join(ROOT, rel), "rb") as fp:
        return json.loads(fp.read())


def main():
    if not os.access(BRAIN, os.X_OK):
        print("v10_api_data_test: no kn_brain at %s: skipped" % BRAIN)
        return 77
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "create", "sqlite", NAME],
                       capture_output=True)
    if r.returncode:
        print("v10_api_data_test: no sqlite scratch database: skipped")
        return 77
    db = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET,
               **{"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()})
    work = tempfile.mkdtemp(prefix="kn-v10api-test.")
    log = os.path.join(work, "brain.log")
    brain = None
    try:
        mig = subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, capture_output=True)
        check("the migrations apply", mig.returncode == 0, mig.stderr.decode(errors="replace")[-200:])
        if mig.returncode:
            return 1
        brain = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                  "--brain.id=v10api-test-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        port = read_wait(log)
        check("kn_brain starts on an ephemeral port", port is not None)
        if port is None:
            return 1

        # /maps: the 47 official rows, mapIndex order, names and file stems as the v10 list shows them
        status, maps = jget(port, "/maps")
        rows = maps.get("data") if isinstance(maps, dict) else None
        check("GET /maps is 200 with {data: list} (v10.js 26581)",
              status == 200 and isinstance(rows, list) and maps.get("error") is None, "status %s" % status)
        check("GET /maps lists 47 official maps in mapIndex order",
              isinstance(rows, list) and len(rows) == 47 and [r.get("index") for r in rows] == list(range(47)))
        check("GET /maps row 0 is Burg with fileName 00_burg and creatorname Krunker",
              isinstance(rows, list) and rows and rows[0].get("name") == "Burg"
              and rows[0].get("fileName") == "00_burg" and rows[0].get("creatorname") == "Krunker")

        # /maps/:id: the map JSON the client loads by index (v10.js 219048, 26586)
        status, burg = jget(port, "/maps/0")
        data = burg.get("data") if isinstance(burg, dict) else None
        file_map = load_json("assets/maps/official/00_burg.json")
        check("GET /maps/0 is 200 with {data: the map JSON} (v10.js 26586)",
              status == 200 and isinstance(data, dict), "status %s" % status)
        check("GET /maps/0 has the objects of assets/maps/official/00_burg.json",
              isinstance(data, dict) and isinstance(data.get("objects"), list)
              and len(data["objects"]) == len(file_map.get("objects", [])) == 1627)
        status, candy = jget(port, "/maps/46")
        candy_data = candy.get("data") if isinstance(candy, dict) else None
        candy_file = load_json("assets/maps/official/46_candy_heist.json")
        check("GET /maps/46 is 200 with the Candy Heist map JSON (v10.0.4)",
              status == 200 and isinstance(candy_data, dict) and len(candy_data.get("objects", [])) == len(candy_file.get("objects", [])),
              "status %s" % status)
        status, body = jget(port, "/maps/47")
        check("GET /maps/47 is 404 MAP_NOT_FOUND (no official map 47)",
              status == 404 and isinstance(body, dict) and body.get("error", {}).get("code") == "MAP_NOT_FOUND",
              "status %s" % status)
        status, body = jget(port, "/maps/x")
        check("GET /maps/x is 404 MAP_NOT_FOUND", status == 404 and body.get("error", {}).get("code") == "MAP_NOT_FOUND")

        # /maps/by-name/:name (v10.js 26592), case-insensitive on the display name
        status, byname = jget(port, "/maps/by-name/burg")
        check("GET /maps/by-name/burg (case-insensitive) is the Burg map",
              status == 200 and isinstance(byname, dict) and byname.get("data", {}).get("name") == "Burg",
              "status %s" % status)
        status, body = jget(port, "/maps/by-name/no%20such%20map")
        check("GET /maps/by-name/<unknown> is 404 MAP_NOT_FOUND",
              status == 404 and body.get("error", {}).get("code") == "MAP_NOT_FOUND", "status %s" % status)

        # /titles: assets/titles.json (27518 getTitles; the consumer keys them by Number(id), 103393)
        titles_file = load_json("assets/titles.json")
        status, titles = jget(port, "/titles")
        tdata = titles.get("data") if isinstance(titles, dict) else None
        check("GET /titles is 200 with {data: list} (v10.js 27518)",
              status == 200 and isinstance(tdata, list), "status %s" % status)
        check("GET /titles returns every row of assets/titles.json with numeric ids",
              isinstance(tdata, list) and tdata == titles_file and all(isinstance(t.get("id"), int) for t in tdata))

        # /data/skins: packed text, decoded the way the client decodes it (28035, 27906)
        status, ctype, raw = http(port, "/data/skins")
        skins_file = load_json("assets/skins.json")
        check("GET /data/skins is 200 text/plain (v10.js 28035 reads responseText)",
              status == 200 and ctype.startswith("text/plain"), "status %s type %s" % (status, ctype))
        try:
            decoded = v10_unpack(raw.decode("utf-8"))
            ok = decoded == skins_as_sent(skins_file)
            detail = "decoded %d rows, expected %d" % (len(decoded), len(skins_file)) if not ok else ""
        except Exception as e:  # noqa: BLE001 - a broken pack is a failed check
            ok, detail = False, repr(e)[:200]
        check("GET /data/skins decodes (v10 unpack) to every row of assets/skins.json (video skins as stills, thumbnails), in order", ok, detail)
        # every record carries its thumbnail: the rule's path for its type, and the count of records with one
        try:
            rows = v10_unpack(raw.decode("utf-8"))
            with_thumb = sum(1 for r in rows if isinstance(r.get("thumbnail"), str) and r["thumbnail"].startswith("/textures/"))
            equal = sum(1 for r, s in zip(rows, skins_file) if r.get("thumbnail") == thumb_path(s))
            ok = with_thumb == len(skins_file) == equal
            detail = "%d of %d rows with a thumbnail, %d equal to the rule" % (with_thumb, len(skins_file), equal)
        except Exception as e:  # noqa: BLE001
            ok, detail = False, repr(e)[:200]
        check("GET /data/skins: every skin record has a thumbnail equal to the rule's path for its type (the count is in the detail)", ok, detail)

        # /data/spins: the wheels table, packed; equal to the JSON /api/data/spins serves (102764)
        status, ctype, raw = http(port, "/data/spins")
        jstatus, spins_json = jget(port, "/api/data/spins")
        check("GET /data/spins is 200 text/plain (v10.js 102764 reads responseText)",
              status == 200 and ctype.startswith("text/plain"), "status %s" % status)
        try:
            spins_decoded = v10_unpack(raw.decode("utf-8"))
            ok = jstatus == 200 and spins_decoded == spins_json
            detail = "" if ok else "decoded keys %s" % (sorted(spins_decoded) if isinstance(spins_decoded, dict) else type(spins_decoded).__name__)
        except Exception as e:  # noqa: BLE001
            ok, detail = False, repr(e)[:200]
        check("GET /data/spins decodes to the wheels JSON of GET /api/data/spins", ok, detail)
    finally:
        if brain is not None:
            try:
                brain.terminate()
                brain.wait(timeout=10)
            except Exception:  # noqa: BLE001
                brain.kill()
        subprocess.run([sys.executable, "-I", os.path.join(HERE, "db_scratch.py"), ROOT, "drop", "sqlite", NAME],
                       capture_output=True)
    ok = bool(RESULTS) and all(RESULTS)
    print("v10_api_data_test: " + ("OK" if ok else "FAILED") + " (%d checks)" % len(RESULTS))
    if ok:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("logs kept in " + work)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

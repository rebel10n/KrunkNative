#!/usr/bin/env python3
"""Community maps on kn_brain (Krunker v10.0.2 publish / Games window parity; services/brain/creations.c).

  community_maps_test.py <kn_brain> <repo root> [database]
Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's database.name; every run
uses its own schema (kn_test_cm_<pid>, dropped after) and an ephemeral port. 77 = skip (no
PostgreSQL / python websockets). KN_TEST_DB_DRIVER=sqlite|mysql runs on that store (db_scratch.py).

Covers the publish checks (uploadM = the editor's publishMap: the 500k limit, the category list,
validateMap's messages, the catalogue name), the stored fields (category, modes from map_info,
official split, votes / plays), the Games window tabs and the new cat= / source= filters on
/api/maps and /api/search (source=published: no catalogue map), republish, another creator's name,
and /api/map-data by name, id and name + creator.
"""
import asyncio
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BRAIN, ROOT = sys.argv[1], sys.argv[2]
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
DB = sys.argv[3] if len(sys.argv) > 3 else os.environ.get("KN_MM_TEST_DATABASE", "")
CONFIG_DB = {}
try:
    with open(os.path.join(ROOT, "config", "config.json")) as fp:
        CONFIG_DB = json.load(fp).get("database", {})
except (OSError, ValueError):
    pass
if not DB:
    DB = CONFIG_DB.get("name", "")
if not DB and DRIVER == "postgres":
    print("community maps test: no database configured; skipped")
    sys.exit(77)
SCHEMA = "kn_test_cm_%d" % os.getpid()
FAILS = []
OFFICIAL_COUNT = 47  # map_catalog.h MAP_CATALOG_OFFICIAL_COUNT (Krunker's official list; 46 = Candy Heist, v10.0.4)
CATEGORY_PARKOUR = 1  # map_catalog.c map_category_name: "Parkour"


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


def bound_port(log_path, proc):
    for _ in range(200):
        if proc.poll() is not None:
            raise RuntimeError("kn_brain exited early (%s)" % log_path)
        with open(log_path) as fp:
            m = re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", fp.read())
        if m:
            return int(m.group(1))
        time.sleep(0.05)
    raise RuntimeError("kn_brain did not start listening")


class Social:
    def __init__(self, url):
        self.url = url + "?enc=json"
        self.inbox = []

    async def __aenter__(self):
        self.ws = await websockets.connect(self.url, max_size=4 << 20)
        return self

    async def __aexit__(self, *a):
        await self.ws.close()

    async def call(self, label, *frame, also=()):
        """Send a frame and return the next reply whose label is `label` (or one of `also`)."""
        await self.ws.send(json.dumps(list(frame)))
        wanted = (label,) + tuple(also)
        deadline = time.time() + 10
        while True:
            for i, f in enumerate(self.inbox):
                if f[0] in wanted:
                    return self.inbox.pop(i)
            self.inbox.append(json.loads(await asyncio.wait_for(self.ws.recv(), deadline - time.time())))


def make_map(name, modes=(0, 1), spawns=2, objects=1, pad=0):
    doc = {
        "name": name,
        "objects": [{"p": [0, 0, 0], "s": [10, 1, 10]} for _ in range(objects)],
        "spawns": [[i * 4, 1, 0] for i in range(spawns)],
        "modes": list(modes),
    }
    if pad:
        doc["pad"] = "x" * pad
    return json.dumps(doc)


async def register(port, name):
    s = await Social("ws://127.0.0.1:%d/social/ws" % port).__aenter__()
    r = await s.call("a", "a", 0, [name, "cmpass123", 0], "captcha")
    assert r[1] is None, r
    return s, r[2]


async def publish(sock, data, cat=CATEGORY_PARKOUR, desc="", video=""):
    """uploadM -> the reply: ["pubMR", msg] or ["upMp", err]."""
    return await sock.call("pubMR", "uploadM", data, None, desc, video, cat, also=("upMp",))


def get(base, path):
    with urllib.request.urlopen(base + path) as resp:
        return json.loads(resp.read())


async def scenario(port):
    stamp = str(int(time.time()) % 100000)
    alice, bob = "cma" + stamp, "cmb" + stamp
    name = "Cm_" + stamp
    base = "http://127.0.0.1:%d" % port

    alice_sock, alice_id = await register(port, alice)
    bob_sock, bob_id = await register(port, bob)

    print("== publish checks (validateMap messages, first error)")
    cases = [
        (make_map("Seventeen_chars_x", spawns=2), CATEGORY_PARKOUR, "Map name too long.", "17-character name"),
        (make_map("Bad<name"), CATEGORY_PARKOUR, "Illegal characters in map name.", "< in the name"),
        (make_map("   "), CATEGORY_PARKOUR, "Missing map name.", "blank name"),
        (make_map("NoSpawns", spawns=0), CATEGORY_PARKOUR, "Missing spawn points.", "no spawns"),
        (make_map("TooManySp", spawns=21), CATEGORY_PARKOUR, "Too many spawn points.", "21 spawns"),
        (make_map("NoObjects", objects=0), CATEGORY_PARKOUR, "Missing objects.", "no objects"),
        (make_map(name), 24, "Invalid Category", "category 24"),
        ("{not json", CATEGORY_PARKOUR, "Map error occured.", "unparsable JSON"),
        (make_map(name, pad=500001), CATEGORY_PARKOUR, "Failed. 500k Character Game Data Limit", "over 500k"),
        (make_map("Burg"), CATEGORY_PARKOUR, "Map name taken", "catalogue name Burg"),
    ]
    for data, cat, want, what in cases:
        r = await publish(alice_sock, data, cat=cat)
        check(r[0] == "upMp" and r[1] == want, "%s -> %r (want %r)" % (what, r, want))

    print("== publish and republish")
    r = await publish(alice_sock, make_map(name, modes=(0, 1)), desc="first", video="")
    check(r[0] == "pubMR" and r[1] == "Map Published!", "valid map published: %r" % r)
    r = await publish(alice_sock, make_map(name, modes=(0, 1)), desc="second", video="")
    check(r[0] == "pubMR" and r[1] == "Map Updated!", "own map republished: %r" % r)
    r = await publish(bob_sock, make_map(name))
    check(r[0] == "upMp" and r[1] == "Map name taken", "another creator's name: %r" % r)

    print("== /api/maps New tab and the row fields")
    page = get(base, "/api/maps?index=2&pos=0")
    rows = page["data"]
    mine = [r for r in rows if r.get("map_name") == name]
    check(page["error"] is None and rows and rows[0]["map_name"] == name, "New lists the published map first")
    row = mine[0] if mine else {}
    check(row.get("creatorname") == alice and row.get("category") == "Parkour" and row.get("map_category") == CATEGORY_PARKOUR,
          "row carries creatorname, category name and index: %r" % {k: row.get(k) for k in ("creatorname", "category", "map_category")})
    check(row.get("official") is False and row.get("modes") == [0, 1], "community row, mode tags from map_info: %r" % row.get("modes"))
    check(row.get("map_updatecounter") == 2 and row.get("map_pl") == 0 and row.get("map_votes") == 0,
          "row has updatecounter, plays and votes (%r)" % {k: row.get(k) for k in ("map_updatecounter", "map_pl", "map_votes")})
    check(row.get("map_description") == "second", "republish replaced the description")
    catalogue_row = [r for r in get(base, "/api/maps?index=0&pos=0")["data"] if r.get("map_name") == "Burg"]
    check(catalogue_row and catalogue_row[0]["official"] is True and isinstance(catalogue_row[0]["modes"], list)
          and catalogue_row[0]["map_category"] == -1 and catalogue_row[0]["category"] == "",
          "catalogue row: official, no category: %r" % catalogue_row[:1])

    print("== cat= and source= filters")
    cat1 = get(base, "/api/maps?index=2&pos=0&cat=%d" % CATEGORY_PARKOUR)["data"]
    check(len(cat1) == 1 and cat1[0]["map_name"] == name, "cat=Parkour keeps our map (and no catalogue map): %d rows" % len(cat1))
    check(get(base, "/api/maps?index=2&pos=0&cat=2")["data"] == [], "cat=Puzzle keeps nothing")
    s1 = get(base, "/api/search?type=map&val=" + urllib.parse.quote(name.lower()) + "&cat=%d" % CATEGORY_PARKOUR)
    check(len(s1["data"]) == 1 and s1["error"] is None, "search with cat=Parkour finds it")
    s2 = get(base, "/api/search?type=map&val=" + urllib.parse.quote(name.lower()) + "&cat=2")
    check(s2["data"] == [], "search with another category finds nothing")
    by_cat = get(base, "/api/search?type=map&val=&cat=%d" % CATEGORY_PARKOUR)["data"]
    check([r["map_name"] for r in by_cat] == [name], "empty search with cat=Parkour (Popular Categories): our map only")
    check(get(base, "/api/search?type=map&val=")["data"] == [], "empty search without a filter finds nothing")
    off = get(base, "/api/search?type=map&val=burg&source=official")["data"]
    check(off and all(r["official"] is True for r in off), "source=official search: official rows only (%d)" % len(off))
    check(get(base, "/api/search?type=map&val=" + urllib.parse.quote(name.lower()) + "&source=official")["data"] == [],
          "source=official hides published maps")
    com = get(base, "/api/search?type=map&val=" + urllib.parse.quote(name.lower()) + "&source=community")["data"]
    check(len(com) == 1, "source=community finds the published map")
    test_rows = get(base, "/api/search?type=map&val=test&source=community")["data"]
    check(test_rows and all(r["official"] is False for r in test_rows) and any(r["map_name"] == "Test" for r in test_rows),
          "source=community keeps the bundled community maps (Test) and no official one")
    official_tab = get(base, "/api/maps?index=0&pos=0&source=official")["data"] + get(base, "/api/maps?index=0&pos=1&source=official")["data"]
    check(len(official_tab) == OFFICIAL_COUNT and all(r["official"] is True for r in official_tab),
          "Popular with source=official: the %d official maps" % OFFICIAL_COUNT)
    community_tab = get(base, "/api/maps?index=0&pos=0&source=community")["data"]
    check(community_tab and all(r["official"] is False for r in community_tab) and any(r["map_name"] == name for r in community_tab),
          "Popular with source=community: no official map, our map included")
    check(get(base, "/api/maps?index=0&pos=0&cat=%d" % CATEGORY_PARKOUR)["data"][0]["map_name"] == name,
          "Popular with cat=Parkour: our map")

    print("== map data (download by name and by id)")
    by_name = urllib.request.urlopen(base + "/api/map-data?name=" + urllib.parse.quote(name)).read()
    check(json.loads(by_name)["name"] == name and json.loads(by_name)["modes"] == [0, 1], "map-data by name serves the map JSON")
    map_id = row.get("map_id")
    by_id = urllib.request.urlopen(base + "/api/map-data?id=%s" % map_id).read()
    check(json.loads(by_id)["name"] == name, "map-data by id (%s) serves the same map" % map_id)
    meta = json.loads(urllib.request.urlopen(base + "/api/map-data?name=%s&meta=1" % urllib.parse.quote(name)).read())
    check(str(meta.get("id")) == str(map_id) and meta.get("map", {}).get("name") == name,
          "map-data meta=1 wraps the map with its id (a hosting game server's customMapData.id)")
    by_creator = urllib.request.urlopen(base + "/api/map-data?name=%s&creator=%s" % (urllib.parse.quote(name), alice.upper())).read()
    check(json.loads(by_creator)["name"] == name, "map-data with its creator (any case) serves it")
    try:
        urllib.request.urlopen(base + "/api/map-data?name=%s&creator=%s" % (urllib.parse.quote(name), bob))
        check(False, "map-data with another creator is 404")
    except urllib.error.HTTPError as e:
        check(e.code == 404, "map-data with another creator is 404")
    published = get(base, "/api/search?type=map&val=&source=published")["data"]
    check([r["map_name"] for r in published] == [name] and published[0]["creatorname"] == alice,
          "source=published lists the published maps only (no catalogue map): %r" % [r.get("map_name") for r in published][:5])
    try:
        urllib.request.urlopen(base + "/api/map-data?name=Nope_" + stamp)
        check(False, "map-data of an unknown name is 404")
    except urllib.error.HTTPError as e:
        check(e.code == 404, "map-data of an unknown name is 404")

    print("== ulb: the editor's Leaderboard Display (v10 updateMapLB) on the map's LEADERBOARD objects")
    lb_name = "Lb_" + stamp
    lb_doc = json.loads(make_map(lb_name))
    lb_doc["objects"].append({"i": 72, "p": [0, 5, 0], "s": [4, 2, 1]})
    r = await publish(alice_sock, json.dumps(lb_doc))
    check(r[0] == "pubMR", "a map with a LEADERBOARD object published: %r" % r)
    lb_row = [x for x in get(base, "/api/search?type=map&val=" + urllib.parse.quote(lb_name.lower()) + "&source=community")["data"]
              if x.get("map_name") == lb_name]
    lb_id = lb_row[0]["map_id"] if lb_row else 0

    def stored_objects(map_id):
        doc = json.loads(urllib.request.urlopen(base + "/api/map-data?id=%s" % map_id).read())
        return doc["objects"]

    def leaderboards(map_id):
        return [o for o in stored_objects(map_id) if o.get("i") == 72]

    r = await bob_sock.call("upMp", "ulb", lb_id, 1, "Kills", 1)
    check(r == ["upMp", "Not allowed"], "ulb by another creator: %r" % (r,))
    r = await alice_sock.call("upMp", "ulb", lb_id, 1, "Kills", 1)
    check(r == ["upMp", "Leaderboard Updated!"], "ulb by the creator: %r" % (r,))
    lbs = leaderboards(lb_id)
    check(len(lbs) == 1 and lbs[0].get("ldt") == 1 and lbs[0].get("ldc") == "Kills" and lbs[0].get("ldo") == 1,
          "the LEADERBOARD carries ldt / ldc / ldo: %r" % lbs)
    check(all("ldt" not in o for o in stored_objects(lb_id) if o.get("i") != 72), "other objects are untouched")
    lb_row = [x for x in get(base, "/api/search?type=map&val=" + urllib.parse.quote(lb_name.lower()) + "&source=community")["data"]
              if x.get("map_name") == lb_name]
    check(lb_row and lb_row[0].get("map_updatecounter") == 2, "the update republishes the map (updatecounter 2)")
    r = await alice_sock.call("upMp", "ulb", lb_id, 0, "", 0)
    check(r == ["upMp", "Leaderboard Updated!"], "ulb back to Score / ASC: %r" % (r,))
    lbs = leaderboards(lb_id)
    check(len(lbs) == 1 and not any(k in lbs[0] for k in ("ldt", "ldc", "ldo")),
          "zero type, empty custom and zero order are left out (v10's export): %r" % lbs)
    r = await alice_sock.call("upMp", "ulb", 99999999, 0, "", 0)
    check(r == ["upMp", "Not allowed"], "ulb on an unknown map: %r" % (r,))
    r = await alice_sock.call("upMp", "ulb", row.get("map_id") or 0, 1, "Kills", 1)
    check(r == ["upMp", "No leaderboard on this map"], "ulb on a map without a LEADERBOARD: %r" % (r,))

    for sock in (alice_sock, bob_sock):
        await sock.ws.close()



async def run(port):
    await scenario(port)


def main():
    if DRIVER == "postgres":
        pg = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "pg_schema.py"), ROOT, DB]
        rc = subprocess.run(pg + ["create", SCHEMA]).returncode
        if rc:
            sys.exit(rc)
        dbenv = {"KN_DATABASE_NAME": DB, "KN_DATABASE_SEARCH_PATH": SCHEMA}
        drop = pg + ["drop", SCHEMA]
    else:
        scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
        r = subprocess.run(scratch + ["create", DRIVER, SCHEMA], capture_output=True)
        if r.returncode:
            print("community maps test: %s unavailable: skipped" % DRIVER)
            sys.exit(r.returncode)
        dbcfg = json.loads(r.stdout)
        dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()}
        drop = scratch + ["drop", DRIVER, SCHEMA]
    work = tempfile.mkdtemp(prefix="kn-cm.")
    env = dict(os.environ, **dbenv)
    log_path = os.path.join(work, "brain.log")
    proc = None
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        with open(log_path, "w") as log:
            proc = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.id=cm-%d" % os.getpid()],
                                    cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        port = bound_port(log_path, proc)
        asyncio.run(run(port))
    finally:
        if proc:
            proc.terminate()
            proc.wait(5)
        subprocess.run(drop)
    with open(log_path) as f:
        bad = [l.strip() for l in f if re.search(r"failed:|ERROR|FATAL", l)]
    check(not bad, "no statement errors in the brain log %s" % bad[:3])
    print("community maps test: " + ("OK" if not FAILS else "%d failure(s)" % len(FAILS)))
    if FAILS:
        print("log kept in " + work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

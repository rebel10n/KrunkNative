#!/usr/bin/env python3
"""Editor publishing, My Games and the asset windows against a local kn_brain (doc/v7/editor.md WP6).

  python3 -I tools/editor/publish_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default postgres from config/config.json;
sqlite needs no server; services/brain/tests/db_scratch.py) on an ephemeral port. An account
registers over the social socket; two headless editor runs (client_bin --editor, EGL, no window on
any display) restore its session from storage and drive editor_net.c with KRUNKNATIVE_EDITOR_SCRIPT:

0. Publish > Game on a map without a spawn point: v10's publishMap runs validateMap first and shows
   "Missing spawn points." as the failure popup; no publish form, nothing uploaded.
1. Publish > Game (publishMap -> plm -> preloadMap -> publishGamePopup -> uploadM -> pubMR {fileId} ->
   POST /upload-user-asset -> mapResp "Game Uploaded!"), the map on /api/maps; publishMap again: "Update Game" /
   "Publish v0.2" with the map's category and description; Mobile Friendly, Post to Hub and a picked thumbnail ->
   "Game Updated!", the thumbnail is the map's map_image, a hub post links the map, the next form has Mobile on; Publish > Asset of a sound (uploadA -> pubR -> the chunked POST /upload-user-asset,
   services/brain/asset_upload.c): a file that is no MP3 fails with the brain's reason in the popup, and the same
   popup publishes once a real MP3 is picked (Krunker 10.0.7: a failed upload is retryable); the Sounds window
   lists it and /api/user-assets/<id>/sound.mp3 answers its bytes.
2. My Games (window 14) lists the map; openEdit; loadMap (edMp) imports the published map;
   updateMapLB (ulb) keeps v10's "Please wait..." popup (kn_brain answers upMp; the brain test checks the stored
   LEADERBOARD fields); deleteCreation
   (delC) -> "Deleted", gone from /api/maps; selectAsset from the Sounds / Assets windows.
The windows 2/5/7/9/14 are dumped too: with the brain they show v7's markup with real lists.
Exit 77 when the database or python websockets is missing.
"""
import asyncio
import base64
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
NAME = "kn_test_edpub_%d" % os.getpid()
WORK = tempfile.mkdtemp(prefix="kn-editor-publish-", dir=os.path.expanduser("~/.cache"))
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def wait_for(path, pattern, secs=15.0):
    end = time.time() + secs
    while time.time() < end:
        with open(path, errors="replace") as f:
            m = re.search(pattern, f.read())
        if m:
            return m
        time.sleep(0.1)
    return None


def api(port, path):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=20) as r:
        return json.loads(r.read())


async def register(port, name):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port, max_size=4 << 20) as ws:
        await ws.send(json.dumps(["a", 0, [name, "editorpass1", None], None]))
        end = time.time() + 10
        while True:
            f = json.loads(await asyncio.wait_for(ws.recv(), end - time.time()))
            if f[0] == "a":
                return f[2], f[5]


def seed_assets(dbcfg, account_id):
    """Two published assets of the account, inserted directly (asset ids below Box Office's own): a model with a
    thumbnail flag and a sound. False when this driver cannot be written from here."""
    return db_exec(dbcfg, "INSERT INTO published_assets (creator_id, name, category, data, size_kb, uvc) VALUES "
                   "(%d, 'Zed model', 3, '{\"t\":1,\"e\":1}', 1500, '{\"e\":2,\"c\":3}'), "
                   "(%d, 'Alpha sound', 17, '{\"u\":1}', 42, NULL)" % (account_id, account_id))


def db_query(dbcfg, sql):
    """The first column of a query's rows on the scratch database, as text; None when this driver cannot be read."""
    if dbcfg.get("driver") == "sqlite":
        import sqlite3
        con = sqlite3.connect(dbcfg["path"])
        rows = [str(r[0]) for r in con.execute(sql)]
        con.close()
        return rows
    if dbcfg.get("driver") == "postgres":
        env = dict(os.environ, PGPASSWORD=str(dbcfg.get("password", "")))
        r = subprocess.run(["psql", "-h", str(dbcfg["host"]), "-p", str(dbcfg["port"]), "-U", str(dbcfg["user"]), "-d", str(dbcfg["name"]),
                            "-v", "ON_ERROR_STOP=1", "-qAt", "-c", "SET search_path TO %s; %s" % (dbcfg["search_path"], sql)],
                           env=env, capture_output=True, text=True, timeout=30)
        return r.stdout.splitlines() if r.returncode == 0 else None
    return None


def db_exec(dbcfg, sql):
    """One statement on the scratch database. False when this driver cannot be written from here."""
    if dbcfg.get("driver") == "sqlite":
        import sqlite3
        con = sqlite3.connect(dbcfg["path"])
        con.execute(sql)
        con.commit()
        con.close()
        return True
    if dbcfg.get("driver") == "postgres":
        env = dict(os.environ, PGPASSWORD=str(dbcfg.get("password", "")))
        r = subprocess.run(["psql", "-h", str(dbcfg["host"]), "-p", str(dbcfg["port"]), "-U", str(dbcfg["user"]), "-d", str(dbcfg["name"]),
                            "-v", "ON_ERROR_STOP=1", "-qAt", "-c", "SET search_path TO %s; %s" % (dbcfg["search_path"], sql)],
                           env=env, capture_output=True, timeout=30)
        return r.returncode == 0
    return False


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def editor(port, cfg, script, tag):
    """One headless editor run; KRUNKNATIVE_SCREENSHOT ends it once the script is done."""
    env = dict(os.environ)
    for k in ("DISPLAY", "WAYLAND_DISPLAY"):
        env.pop(k, None)
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port,
                "KRUNKNATIVE_SCREENSHOT": os.path.join(WORK, tag + ".png"), "KRUNKNATIVE_EDITOR_SCRIPT": ";".join(script) + ";wait:0.5"})
    log = os.path.join(WORK, tag + ".log")
    with open(log, "w") as out:
        r = subprocess.run([os.path.join(BUILD, "client_bin"), "--editor", "new"], cwd=ROOT, env=env, stdout=out,
                           stderr=subprocess.STDOUT, timeout=300)
    check("%s: client_bin exits cleanly" % tag, r.returncode == 0, "exit %d (%s)" % (r.returncode, log))
    return read(log)


def main():
    if not os.path.exists(os.path.join(BUILD, "client_bin")) or not os.path.exists(os.path.join(BUILD, "services/brain/kn_brain")):
        print("publish_e2e: client_bin or kn_brain not built: skipped")
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("publish_e2e: %s unavailable: skipped" % DRIVER)
        return 77
    dbcfg = json.loads(r.stdout)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()}
    env = dict(os.environ, **dbenv)
    brain = None
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([os.path.join(BUILD, "services/brain/kn_brain"), "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([os.path.join(BUILD, "services/brain/kn_brain"), "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.id=edpub-%d" % os.getpid(), "--uploads.dir=%s" % os.path.join(WORK, "uploads")],
                                 cwd=ROOT, env=env,
                                 stdout=open(brain_log, "w"), stderr=subprocess.STDOUT)
        m = wait_for(brain_log, r"listening on http://127\.0\.0\.1:(\d+) \(ws")
        if not m:
            check("kn_brain starts", False, brain_log)
            return 1
        port = int(m.group(1))
        stamp = str(int(time.time()) % 100000)
        account, mapname = "ed6u" + stamp, "Ed6Map" + stamp
        acc_id, token = asyncio.run(register(port, account))
        check("account registered", bool(token), str(token))
        seeded = seed_assets(dbcfg, acc_id)
        cfg = os.path.join(WORK, "cfg")
        os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
        for k, v in (("krunker_username", account), ("krunker_token", token), ("krunker_id", str(acc_id))):
            with open(os.path.join(cfg, "krunknative", k + ".json"), "w") as f:
                f.write(v)
        D = lambda n: os.path.join(WORK, n)

        # ---- run 0: v10's publishMap checks a named map before anything is uploaded: no spawn point
        # -> validateMap's first error as the failure popup, no publish form, nothing on the brain
        bad = mapname + "x"
        s = ["cfg:set:map.name:%s" % json.dumps(bad), "add:0", "wait:1", "publish:7", "wait:4", "dump:popup:%s" % D("pub0.txt")]
        log0 = editor(port, cfg, s, "run0")
        p0 = read(D("pub0.txt"))
        check("publish refuses a spawnless map: v10's \"Missing spawn points.\" popup", "X Missing spawn points." in p0, p0 + log0[-800:])
        check("... before the upload: no publish form, no uploadM", "Publish Game" not in p0 and "KE.uploadGame" not in log0, p0)
        rows = api(port, "/api/maps?index=4&pos=0&accountId=%d" % acc_id)["data"]
        check("... and the brain has no such map", not [r for r in rows if r["map_name"] == bad], str(rows)[:300])

        # ---- run 1: publish, update, publish asset
        bad_mp3, good_mp3 = D("not-a-sound.mp3"), os.path.join(ROOT, "assets", "sound", "adrop_0.mp3")
        thumb_png = os.path.join(ROOT, "ui", "img", "noimg.png")
        if not os.path.exists(good_mp3):  # a tree without the game's sounds (a fresh worktree): one MPEG audio frame
            good_mp3 = D("good.mp3")
            with open(good_mp3, "wb") as f:
                f.write(b"\xff\xfb\x90\x00" + bytes(413) + b"\xff\xfb\x90\x00" + bytes(413))
        with open(bad_mp3, "w") as f:
            f.write("plain text, not a sound\n")
        # add:5 = SPAWN_POINT: uploadM runs v10's validateMap, which refuses a map without spawns
        s = ["cfg:set:map.name:%s" % json.dumps(mapname), "add:0", "add:1", "add:0", "add:5", "wait:1",
             "publish:7", "wait:6", "dump:popup:%s" % D("pub1.txt"),
             "ui:KE.publishMap()", "wait:4", "dump:popup:%s" % D("pub2-form.txt"),
             "net:check:gameMobNew:1", "net:check:gamePostNew:1", "net:file:gameThumbnail:%s" % thumb_png,
             "ui:KE.uploadGame()", "wait:4", "dump:popup:%s" % D("pub2.txt"),
             "ui:KE.publishMap()", "wait:4", "dump:popup:%s" % D("pub3-form.txt"),
             "net:publish-asset", "wait:0.5", "dump:popup:%s" % D("asset-form.txt"),
             "net:asset-type:3", "net:asset-name:Ed6Snd%s" % stamp, "net:file:gameSound:%s" % bad_mp3, "net:upload-asset",
             "wait:4", "dump:popup:%s" % D("asset-bad.txt"),
             "net:file:gameSound:%s" % good_mp3, "net:upload-asset", "wait:4", "dump:popup:%s" % D("asset.txt")]
        for n in (2, 5, 7, 9):
            s += ["window:%d" % n, "wait:2", "dump:window:%s" % D("w%d.txt" % n)]
        s.append("closewindow")
        log1 = editor(port, cfg, s, "run1")
        p1 = read(D("pub1.txt"))
        check("publish: mapResp \"Game Uploaded!\" + share button", "X Game Uploaded!" in p1 and "Share the game here" in p1, p1 + log1[-800:])
        form = read(D("pub2-form.txt"))
        check("publish again: \"Update Game\" popup, button \"Publish v0.2\"", "Update Game" in form and "Publish v0.2" in form, form)
        # plm -> preloadMap(false, row): the form is the published map's (category, description, Mobile Friendly off)
        check("plm: the Update Game form has the map's category and description",
              "select#24[Other..Education]=Adventure" in form and "X Published by the KRUNKNATIVE_EDITOR_SCRIPT test" in form, form)
        f3 = read(D("pub3-form.txt")).splitlines()
        mob = f3.index("X Mobile Friendly") if "X Mobile Friendly" in f3 else -1
        check("the update kept Mobile Friendly (map_mobile): the next form has it on, \"Publish v0.3\"",
              mob >= 0 and f3[mob + 1] == "I switch=1" and "X Publish v0.3" in f3, "\n".join(f3))
        check("publish form: 24 categories, Post to Hub, Mobile Friendly", "select#24[Other..Education]" in form
              and "Post to Hub" in form and "Mobile Friendly" in form, form)
        p2 = read(D("pub2.txt"))
        check("update: \"Game Updated!\"", "X Game Updated!" in p2, p2)
        rows = api(port, "/api/maps?index=4&pos=0&accountId=%d" % acc_id)["data"]
        mine = [r for r in rows if r["map_name"] == mapname]
        check("the brain lists the map under My Games, version 2", len(mine) == 1 and mine[0]["map_updatecounter"] == 2, str(rows)[:300])
        with open(thumb_png, "rb") as f:
            png_bytes = f.read()
        check("the picked thumbnail went up with publishMapResp's ticket: the map's map_image",
              bool(mine) and mine[0]["map_image"] == "data:image/png;base64," + base64.b64encode(png_bytes).decode(), log1[-800:])
        info = mine[0].get("map_info") if mine else None
        check("... map_info.t set", (json.loads(info) if isinstance(info, str) else info or {}).get("t") == 1, str(info))
        if mine:
            with urllib.request.urlopen("http://127.0.0.1:%d/api/user-assets/m%s/thumb.png" % (port, mine[0]["map_id"]), timeout=10) as r:
                check("GET /api/user-assets/m<id>/thumb.png: the picked file", r.read() == png_bytes)
        posts = db_query(dbcfg, "SELECT link FROM feed_posts WHERE author_id = %d AND type = 2" % acc_id)
        check("Post to Hub: one hub game post (type 2) linking the map", posts is not None and bool(mine) and posts == [str(mine[0]["map_id"])],
              str(posts))
        check("the published description is the script's", bool(mine) and "KRUNKNATIVE_EDITOR_SCRIPT" in mine[0]["map_description"])
        af = read(D("asset-form.txt"))
        check("publish asset popup: v7's fields", "Publish or Update Asset" in af and "select#5[Model..Video]" in af
              and "select#17[Clutter..Triggers]" in af, af)
        a0, a = read(D("asset-bad.txt")), read(D("asset.txt"))
        check("Publish Asset of a file that is no MP3: the brain's reason in the popup", "Invalid Sound (MP3 only)" in a0, a0 + log1[-800:])
        check("the same popup again with an MP3: \"Asset Uploaded!\" (10.0.7: retryable)", "Asset Uploaded!" in a, a + log1[-800:])
        mine_a = [r for r in api(port, "/api/assets?index=0&cat=17&username=%s" % account).get("data", [])
                  if r.get("nm") == "Ed6Snd" + stamp] if "Asset Uploaded!" in a else []
        check("the brain lists the uploaded sound (an id from 1000000000)", len(mine_a) == 1 and int(mine_a[0]["id"]) >= 1000000000,
              str(mine_a)[:300])
        if mine_a:
            with open(good_mp3, "rb") as f:
                want = f.read()
            with urllib.request.urlopen("http://127.0.0.1:%d/api/user-assets/%s/sound.mp3" % (port, mine_a[0]["id"]), timeout=10) as r:
                check("GET /api/user-assets/<id>/sound.mp3: the picked file", r.read() == want)
        w2, w9 = read(D("w2.txt")), read(D("w9.txt"))
        if seeded:
            # newest first; formatBytes(1500 kB, 0) = "2MB"; the owner gets the delete icon, a sound the play button
            check("Assets window: the brain's assets as v7 cards (kind, delete, id, size, category, name)",
                  re.search(r"X Model\nX delete_forever\nX \d+e\nX 2MB\nX Vehicles\nX Zed model", w2) is not None
                  and re.search(r"X play_arrow\nX Sound\nX delete_forever\nX \d+\nX 42KB\nX Alpha sound\nX Model", w2) is not None, w2)
            check("Sounds window: only category 17", "Alpha sound" in w9 and "Zed model" not in w9, w9)
            # listAssets: index echoed, the username narrows My Assets only; dat.mp from the stored UV channels
            r0 = api(port, "/api/assets?index=0&cat=-1&username=nobody%s" % stamp)
            zed = [r for r in r0.get("data", []) if r.get("nm") == "Zed model"]
            check("assets/list index 0: every account's assets whatever the username, index 0 echoed",
                  r0.get("index") == 0 and len(zed) == 1, str(r0)[:300])
            check("... a model's dat carries mp {letter: {ch}} from its UV channels (no diffuse 'c')",
                  bool(zed) and json.loads(zed[0]["dat"]).get("mp") == {"e": {"ch": 2}}, str(zed)[:300])
            r1 = api(port, "/api/assets?index=1&cat=-1&username=nobody%s" % stamp)
            check("assets/list index 1 (My Assets): the username's only, index 1 echoed", r1.get("index") == 1 and r1.get("data") == [],
                  str(r1)[:300])
            check("... and none without a username", api(port, "/api/assets?index=1&cat=-1").get("data") == [])
        else:
            check("Assets window with the brain (nothing published)", "X No Assets found" in w2, w2)
        for n, want in ((5, "T Home *"), (7, "X No Textures found")):
            w = read(D("w%d.txt" % n))
            check("window %d with the brain" % n, want in w, w)
        w5 = read(D("w5.txt"))
        check("Mods window: the brain's mod list (or its empty text)", "No Mods found" in w5 or "X " in w5.split("T Search", 1)[-1], w5)
        if not mine:
            return 1
        map_id = mine[0]["map_id"]

        # ---- run 2: My Assets with the session (the sound made Private: only its signed-in owner's list has it), My Games,
        # load in editor, leaderboard, asset selection, delete
        zed_id = ([r["id"] for r in api(port, "/api/assets?index=0&cat=-1").get("data", []) if r.get("nm") == "Zed model"] or [0])[0]
        private = bool(mine_a) and db_exec(dbcfg, "UPDATE published_assets SET is_private = %s WHERE id = %d"
                                           % ("TRUE" if dbcfg.get("driver") == "postgres" else "1", int(mine_a[0]["id"])))
        s = ["window:2", "wait:2", "ui:windows[1].switchTab(1)", "wait:3", "dump:window:%s" % D("w2mine.txt"), "closewindow",
             "window:14", "wait:3", "dump:window:%s" % D("w14.txt"),
             "net:edit:%s:%d:0" % (mapname, map_id), "dump:popup:%s" % D("edit.txt"),
             "net:load:%s" % mapname, "wait:3", "objdump:%s" % D("loaded.txt"),
             "net:lb:%d" % map_id, "wait:10", "dump:popup:%s" % D("lb.txt"),
             "clear", "add:55", "select:0", "net:select:77:0:selectShootHit", "objdump:%s" % D("ai.txt"),
             "net:select:12345:0:createObjects", "objdump:%s" % D("custom.txt"),
             "window:2", "wait:2", "closewindow", "clear", "net:select:%se:0:createObjects" % zed_id, "objdump:%s" % D("mp.txt"),
             "net:delete:%d:0" % map_id, "wait:3", "dump:popup:%s" % D("delete.txt")]
        log2 = editor(port, cfg, s, "run2")
        w2m = read(D("w2mine.txt"))
        check("My Assets with the account's session lists its Private sound", private and "Ed6Snd" + stamp in w2m, w2m[-600:])
        check("... which no list without the session has", private and "Ed6Snd" + stamp not in json.dumps(
            api(port, "/api/assets?index=1&cat=-1&username=%s" % account)), "")
        w14 = read(D("w14.txt"))
        check("My Games (window 14) lists the published map", "H Maps" in w14 and mapname in w14, w14 + log2[-600:])
        e = read(D("edit.txt"))
        check("openEdit: Load in Editor, leaderboard and funds", "Load in Editor" in e and "Update Leaderboard" in e and "Withdraw" in e, e)
        loaded = [l for l in read(D("loaded.txt")).splitlines() if l.strip()]
        check("Load in Editor (edMp) imports the published map (3 objects + the spawn)", len(loaded) == 4, "%d objects" % len(loaded))
        lb = read(D("lb.txt"))
        check("ulb: v10's upMp reply shows nothing, the \"Please wait...\" popup stays",
              "Please wait" in lb and "Unknown request" not in lb and "No reply from the server" not in lb, lb)
        ai = read(D("ai.txt"))
        check("selectShootHit sets the AI's shootHitSound", '"atkh":77' in ai.replace(" ", ""), ai[:300])
        custom = read(D("custom.txt"))
        check("selectAsset createObjects adds a CUSTOM_ASSET", '"aid":"12345"' in custom.replace(" ", ""), custom[-300:])
        if seeded:
            mp = read(D("mp.txt")).replace(" ", "")
            check("createObjects of a listed model: the object takes KE.assetMapProps[id] as mp", '"mp":{"e":{"ch":2}}' in mp, mp[-300:])
        d = read(D("delete.txt"))
        check("deleteCreation (delC): \"Deleted\"", "X Deleted" in d, d)
        rows = api(port, "/api/maps?index=4&pos=0&accountId=%d" % acc_id)["data"]
        check("the map is gone from the brain", not [r for r in rows if r["map_name"] == mapname], str(rows)[:300])
    finally:
        if brain:
            brain.terminate()
            try:
                brain.wait(10)
            except subprocess.TimeoutExpired:
                brain.kill()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    bad = [l for l in read(brain_log).splitlines() if re.search(r"failed:|ERROR|FATAL", l)]
    check("no statement errors in the brain log", not bad, str(bad[:3]))
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

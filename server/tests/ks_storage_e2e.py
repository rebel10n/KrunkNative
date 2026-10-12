#!/usr/bin/env python3
"""GAME.STORAGE end to end through kn_brain (doc/v7/krunkscript.md 2.6 / WP6, doc/backend.md 7.6).

  python3 -I server/tests/ks_storage_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default postgres from config/config.json;
sqlite needs no server; tests/db_scratch.py) + a brain-linked server_bin. An account publishes
a map whose scripts.server saves and loads on spawn, hosts it through /mm/host-game with KR
rewards on, and joins with the host ticket over the raw protocol: the server's "storage" /
"storage_load" ops reach the database and come back to the script (SCRIPT_LOG lines), a second
join reads the stored value again, giveKR is refused for lack of map funds, and a lobby that is
not a published map refuses storage. Exit 77 when the database or python websockets is missing.
"""
import asyncio
import json
import os
import re
import shutil
import struct
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

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
NAME = "kn_test_kse_%d" % os.getpid()
SECRET = "kn-kse-%d" % os.getpid()
GAME_PORT = int(os.environ.get("KN_TEST_GAME_PORT", "22000")) + os.getpid() % 1000
SCRIPT_LOG = 201
WORK = tempfile.mkdtemp(prefix="kn-ks-storage-e2e-")
results = []

SERVER_SCRIPT = """
action saved(obj data, bool ok, str name) {
    GAME.log("saved", toStr ok, (str) data.visits);
    if (ok) { GAME.STORAGE.load(name, "", loaded); }
}
action loaded(obj data, bool ok, str name) { GAME.log("loaded", toStr ok, (str) data.visits); }
action visit(str id) {
    obj p = GAME.PLAYERS.findByID(id);
    str acct = (str) p.accountName;
    GAME.log("account", acct);
    GAME.STORAGE.update(acct, {visits: 1}, "private", saved);
    GAME.PAYMENTS.giveKR(p, 5);
}
public action onPlayerSpawn(str id) { visit(id); }
public action onChatMessage(str msg, str playerID) { visit(playerID); }
"""


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def wait_for(path, pattern, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        with open(path, errors="replace") as f:
            m = re.search(pattern, f.read())
        if m:
            return m
        time.sleep(0.1)
    return None


def http(port, path, body=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=json.dumps(body).encode() if body else None,
                                 headers={"Content-Type": "application/json"}, method="POST" if body else "GET")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


async def social_call(ws, label, *frame):
    await ws.send(json.dumps(list(frame)))
    end = time.time() + 10
    while True:
        f = json.loads(await asyncio.wait_for(ws.recv(), end - time.time()))
        if f[0] == label:
            return f


async def register_and_publish(port, name, mapjson):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port, max_size=4 << 20) as ws:
        r = await social_call(ws, "a", "a", 0, [name, "storagepass1", 0], "captcha")
        token = r[5]
        p = await social_call(ws, "pubMR", "uploadM", mapjson, None, "storage e2e", "", 2)
        return token, p[1]


def logs(conn):
    return [p[1:].decode(errors="replace") for t, p in conn.packets if t == SCRIPT_LOG]


def pump(c, secs):
    end = time.time() + secs
    while time.time() < end:
        c.read(timeout=0.3)


def play(seek, secs=4.0):
    c = kn_proto.Conn("127.0.0.1", GAME_PORT)
    c.send(kn_proto.HELLO, bytes([1, 3, 0]))
    c.join(client_key=seek.get("clientId", ""), game_id=seek.get("gameId", ""))
    c.ent()
    pump(c, secs)
    return c


def main():
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("ks_storage_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    procs = []
    brain_log, game_log = os.path.join(WORK, "brain.log"), os.path.join(WORK, "game.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                       "--brain.id=kse-%d" % os.getpid()], cwd=ROOT, env=env,
                                      stdout=open(brain_log, "w"), stderr=subprocess.STDOUT))
        m = wait_for(brain_log, r"listening on http://127\.0\.0\.1:(\d+) \(ws")
        if not m:
            check("kn_brain starts", False)
            return 1
        port = int(m.group(1))

        stamp = str(int(time.time()) % 100000)
        account, mapname = "kse" + stamp, "KsE" + stamp
        with open(os.path.join(ROOT, "server/tests/arena.json")) as f:
            arena = json.load(f)
        arena["name"] = mapname
        arena["scripts"] = {"server": SERVER_SCRIPT}
        token, published = asyncio.run(register_and_publish(port, account, json.dumps(arena)))
        check("map published", published == "Map Published!", str(published))
        other_token, _ = asyncio.run(register_and_publish(port, "kso" + stamp, json.dumps(dict(arena, name="KsO" + stamp))))
        public_map = os.path.join(WORK, "public.json")
        with open(public_map, "w") as f:
            json.dump(dict(arena, name="KsP" + stamp), f) # the same script on a map that is not published

        gs = os.path.join(WORK, "gs.json")
        with open(gs, "w") as f:
            json.dump({"gameserver": {"node_id": "KSE", "name": "ks storage e2e", "region": "local", "listen_port": GAME_PORT,
                                      "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % port, "brain_secret": SECRET,
                                      "modes": ["ffa"], "max_players": 4}}, f)
        procs.append(subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", gs, public_map], cwd=ROOT,
                                      stdout=open(game_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
        check("server linked to the brain", wait_for(game_log, r"welcomed", 15) is not None)
        time.sleep(1.0)

        # the public lobby (its map is not a published one): storage is refused
        lobby = ""
        for _ in range(50):
            code, games = http(port, "/mm/game-list")
            lobby = ([g[0] for g in (games or {}).get("games", []) if g[0].startswith("KSE:")] + [""])[0]
            if lobby:
                break
            time.sleep(0.2)
        check("public lobby listed", bool(lobby), str(games)[:200])
        code, seek0 = http(port, "/mm/seek-game?token=" + other_token + "&game=" + lobby)
        o = play(seek0 if code == 200 else {})
        lo = logs(o)
        check("not a published map: refused", "Storage only supported on published maps" in lo and "saved false undefined" in lo, str(lo))
        o.close()
        time.sleep(1.0)

        code, seek = http(port, "/mm/host-game?token=" + token, {"config": {"customMap": mapname, "krRewards": True}})
        check("hosted the published map", code == 200 and seek.get("clientId"), "%s %s" % (code, str(seek)[:200]))
        if code != 200:
            return 1
        a = play(seek)
        la = logs(a)
        check("the script sees the account", ("account " + account) in la, str(la))
        check("STORAGE.update reached the database", "saved true 1" in la, str(la))
        check("STORAGE.load reads it back", "loaded true 1" in la, str(la))
        # the next 10-s window: one more visit adds to the stored value
        time.sleep(10.5)
        a.send(8, b"again\x00")  # CHAT_SEND -> onChatMessage
        pump(a, 3.0)
        la = logs(a)
        check("the database adds up: 2", "saved true 2" in la and "loaded true 2" in la, str(la))
        a.close()
        game = open(game_log, errors="replace").read()
        check("giveKR goes to kn_brain and is refused (no map funds)", "refused: Not enough map funds" in game,
              "\n".join(l for l in game.splitlines() if "storage" in l)[-400:])
        check("no runtime errors", "RUNTIME ERROR" not in game)
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    brain = open(brain_log, errors="replace").read()
    bad = [l for l in brain.splitlines() if re.search(r"failed:|ERROR|FATAL", l)]
    check("no statement errors in the brain log", not bad, str(bad[:3]))
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

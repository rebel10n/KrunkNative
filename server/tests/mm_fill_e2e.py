#!/usr/bin/env python3
"""Quick join fills populated lobbies first (doc/backend.md 6.2), end to end.

  python3 -I server/tests/mm_fill_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default postgres from config/config.json;
sqlite needs no server; services/brain/tests/db_scratch.py) + three brain-linked server_bin
nodes in one region, every port from kn_proto.free_ports(). Seekers are guests that really
join (seek-game -> HELLO -> JOIN with the ticket -> ENT -> SPAWN) and stay connected:

  1. a player joins the LAST node's lobby by id; three seekers then seek one after the other,
     each right after the previous one spawned (inside the server's lobby-up debounce, when the
     brain's player count is stale): all of them land in that populated lobby, none in an
     empty one
  2. everyone leaves; once the counts are back to 0, three seekers seek at the same moment:
     the three tickets name the same lobby, and the three players spawn there
  3. the game list agrees: one lobby holds every player, the other two are empty

Exit 77 when the database is unavailable.
"""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
NAME = "kn_test_mmfill_%d" % os.getpid()
SECRET = "kn-mmfill-%d" % os.getpid()
NODES = 3
GAME_PORTS = kn_proto.free_ports(NODES)
WORK = tempfile.mkdtemp(prefix="kn-mm-fill-e2e-")
results = []


def check(name, ok, detail=""):
    """detail: shown with a FAIL, and with a PASS when short (lobby ids and counts)"""
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 120)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.1)
    return None


def http(port, path):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=10) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def lobbies(port):
    """{gameId: players} of the e2e nodes' public lobbies."""
    code, body = http(port, "/mm/game-list")
    if code != 200:
        return {}
    return {g[0]: g[2] for g in body.get("games", []) if g[0].startswith("MMF")}


def seek(port, game=""):
    path = "/mm/seek-game?hostname=krunknative&region=local&autoChangeGame=true"
    return http(port, path + ("&game=" + game if game else ""))


def join(reply):
    """Connect with a seek-game reply and play until our SPAWN; returns (conn, ok)."""
    c = kn_proto.Conn("127.0.0.1", int(reply["port"]))
    c.hello()
    c.join(client_key=reply.get("clientId", ""), game_id=reply.get("gameId", ""))
    c.ent()
    end = time.time() + 8
    while time.time() < end:
        payload = c.read(until=kn_proto.SPAWN, timeout=max(0.1, end - time.time()))
        if payload is None:
            break
        sp = kn_proto.spawn_info(payload)
        if sp and sp["is_you"]:
            return c, True
    return c, False


def main():
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("mm_fill_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    procs, conns = [], []
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                       "--brain.region=local", "--brain.id=mmfill-%d" % os.getpid()], cwd=ROOT, env=env,
                                      stdout=open(brain_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))

        node_logs = []
        for i in range(NODES):
            gs = os.path.join(WORK, "gs%d.json" % i)
            with open(gs, "w") as f:
                json.dump({"gameserver": {"node_id": "MMF%d" % i, "name": "mm fill e2e %d" % i, "region": "local",
                                          "listen_port": GAME_PORTS[i], "public_host": "127.0.0.1",
                                          "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % port,
                                          "brain_secret": SECRET, "modes": ["ffa"], "max_players": 6}}, f)
            node_logs.append(os.path.join(WORK, "gs%d.log" % i))
            procs.append(subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", gs], cwd=ROOT, env=env,
                                          stdout=open(node_logs[i], "w"), stderr=subprocess.STDOUT,
                                          stdin=subprocess.DEVNULL))
        for i in range(NODES):
            check("node %d linked to the brain" % i, wait_for(lambda i=i: "welcomed" in read_log(node_logs[i]), 20))
        listed = wait_for(lambda: (lambda l: l if len(l) == NODES else None)(lobbies(port)), 15) or {}
        check("three empty public lobbies listed", len(listed) == NODES and not any(listed.values()), str(listed))
        if len(listed) != NODES:
            return 1
        last = sorted(listed)[-1]

        # 1. sequential seekers join the populated lobby (the last node's, not the first one in order)
        code, rep = seek(port, last)
        c, ok = join(rep) if code == 200 else (None, False)
        check("a player joins %s by id" % last, ok and rep.get("gameId") == last, "%s %s" % (code, rep))
        if c:
            conns.append(c)
        landed = []
        for k in range(3):
            code, rep = seek(port)  # immediately after the previous SPAWN: the brain's counts lag
            landed.append(rep.get("gameId") if code == 200 else "HTTP %d %s" % (code, rep))
            c, ok = join(rep) if code == 200 else (None, False)
            check("sequential seeker %d spawns" % (k + 1), ok)
            if c:
                conns.append(c)
        check("3 sequential seekers fill the populated lobby", landed == [last] * 3, str(landed))
        counts = wait_for(lambda: (lambda l: l if l.get(last) == 4 else None)(lobbies(port)), 5) or lobbies(port)
        check("the game list counts 4 players in %s and none elsewhere" % last,
              counts.get(last) == 4 and sum(counts.values()) == 4, str(counts))

        # 2. concurrent seekers: everyone leaves first, then three seek at the same moment
        for c in conns:
            c.close()
        conns.clear()
        check("players left (counts back to 0)", wait_for(lambda: (lambda l: len(l) == NODES and not any(l.values()))(lobbies(port)), 10),
              str(lobbies(port)))
        replies = [None] * 3
        gate = threading.Barrier(3)

        def seeker(k):
            gate.wait()
            replies[k] = seek(port)

        threads = [threading.Thread(target=seeker, args=(k,)) for k in range(3)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(15)
        ids = [rep[1].get("gameId") if rep and rep[0] == 200 else str(rep) for rep in replies]
        check("3 concurrent seekers get the same lobby", len(set(ids)) == 1 and ids[0] in listed, str(ids))
        joined = 0
        for rep in replies:
            if rep and rep[0] == 200:
                c, ok = join(rep[1])
                conns.append(c)
                joined += ok
        check("3 concurrent seekers spawn", joined == 3)
        target = ids[0]
        counts = wait_for(lambda: (lambda l: l if l.get(target) == 3 else None)(lobbies(port)), 5) or lobbies(port)
        check("the game list counts 3 players in one lobby, the others empty",
              counts.get(target) == 3 and sum(counts.values()) == 3, str(counts))
    finally:
        for c in conns:
            c.close()
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    brain = read_log(brain_log)
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

#!/usr/bin/env python3
"""/mm/hot-seek, the web hot loader's one-trip seek (doc/web-hot-loader.md), end to end.

  python3 -I server/tests/hot_seek_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite), two brain-linked server_bin nodes
(HSA: ffa on Burg, HSB: tdm on Sandstorm). No client: the page's GET is made here, as the hot loader
makes it (platform/web/hot-loader.js), and the answer's ticket is used over the game protocol.

  H1  no preferences: 200, "hot": true, a lobby of the public rotation with host / port / ticket, and
      the game server admits the ticket (a spawned player)
  H2  seek_modes (Settings > Quickplay, as knSettings saves it: "a,b"): the node playing that mode
  H3  seek_maps: the node playing that map
  H4  game=<id> (an invite / ?game= link): that lobby
  H5  only invalid entries switched on: the rotation, as the client's seek_filter_parse drops them
  H6  a preference nothing plays: NoMatchingServers / NoServersMatchQuery (the page falls back)
  H7  CORS preflight: 204, no ticket
  H8  a brain requiring validation tokens still serves it (the token is issued and spent inside)
  H9  a brain enforcing the seek captcha answers CaptchaRequired (the client seeks itself)

Exit 77 when the database is unavailable.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_hotseek_%d" % os.getpid()
SECRET = "kn-hotseek-e2e-secret-%d" % os.getpid()
PORT_A, PORT_B = kn_proto.free_ports(2)
WORK = tempfile.mkdtemp(prefix="kn-hotseek-", dir=os.path.expanduser("~/.cache"))
procs = []
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 160)
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


def http(port, path, method="GET"):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), method=method)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            data = r.read()
            return r.status, json.loads(data) if data else None
    except urllib.error.HTTPError as e:
        data = e.read() or b"null"
        try:
            return e.code, json.loads(data)
        except ValueError:
            return e.code, data.decode(errors="replace")


def hot(port, **params):
    q = {"proto": "1.0", "ruleset": "v10"}
    q.update(params)
    return http(port, "/mm/hot-seek?" + urllib.parse.urlencode(q))


def lobbies(port):
    code, body = http(port, "/mm/game-list")
    return body.get("games", []) if code == 200 else []


def start_brain(env, log, extra=()):
    p = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                          "--brain.id=hotseek-%d-%d" % (os.getpid(), len(procs))] + list(extra),
                         cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    procs.append(p)
    m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(log)), 15)
    return int(m.group(1)) if m else None


def start_node(brain_port, node_id, port, mode, map_name):
    log = os.path.join(WORK, node_id + ".log")
    cfg = os.path.join(WORK, node_id + ".json")
    with open(cfg, "w") as f:
        json.dump({"gameserver": {"node_id": node_id, "name": "hot seek e2e " + node_id, "region": "local", "listen_port": port,
                                  "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
                                  "brain_secret": SECRET, "modes": [mode], "maps": [map_name], "max_players": 6,
                                  "idle_kick_seconds": 0, "idle_menu_kick_seconds": 0}}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", cfg, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    wait_for(lambda: "welcomed" in read_log(log), 20)
    return wait_for(lambda: next((g for g in lobbies(brain_port) if g[0].startswith(node_id + ":")), None), 15)


def admitted(rep):
    """JOIN with the answer's ticket, then ENT: the joiner's active SPAWN (the load-in hold refuses the first ENTs)."""
    c = kn_proto.Conn("127.0.0.1", int(rep["port"]))
    c.hello()
    c.join(client_key=rep.get("clientId", ""), game_id=rep.get("gameId", ""))
    end, seen, sent = time.time() + 10, 0, 0.0
    try:
        while time.time() < end:
            if time.time() - sent > 0.7:
                c.ent()
                sent = time.time()
            c.read(timeout=0.2)
            for t, data in c.packets[seen:]:
                if t == kn_proto.ERROR:
                    return False, kn_proto.error_info(data)
                sp = kn_proto.spawn_info(data) if t == kn_proto.SPAWN else None
                if sp and sp["is_you"] and sp["active"]:
                    return True, None
            seen = len(c.packets)
        return False, "no active SPAWN"
    finally:
        c.close()


def main():
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("hot_seek_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        port = start_brain(env, os.path.join(WORK, "brain.log"))
        check("kn_brain starts", port, read_log(os.path.join(WORK, "brain.log"))[-300:])
        if not port:
            return 1
        game_a = start_node(port, "HSA", PORT_A, "ffa", "Burg")
        game_b = start_node(port, "HSB", PORT_B, "tdm", "Sandstorm")
        check("nodes up", game_a and game_b, "%s %s" % (game_a, game_b))
        if not (game_a and game_b):
            return 1
        mode_a, mode_b = game_a[4]["mode"], game_b[4]["mode"]
        map_a = game_a[4].get("mapIndex", -1)
        ids = {game_a[0], game_b[0]}

        code, rep = hot(port)
        ok = code == 200 and isinstance(rep, dict)
        check("H1: no preferences: 200 with a lobby, host, port and ticket, marked hot",
              ok and rep.get("hot") is True and rep.get("gameId") in ids and rep.get("host") and rep.get("port") and rep.get("clientId"),
              "%s %s" % (code, rep))
        if ok and rep.get("clientId"):
            yes, why = admitted(rep)
            check("H1: the game server admits the hot seek's ticket", yes, str(why))

        code, rep = hot(port, seek_modes="%d" % mode_b)
        check("H2: seek_modes picks the node playing that mode", code == 200 and rep.get("gameId") == game_b[0], "%s %s" % (code, rep))
        code, rep = hot(port, seek_modes="%d,%d" % (mode_b, mode_b))
        check("H2: a repeated entry counts once", code == 200 and rep.get("gameId") == game_b[0], "%s %s" % (code, rep))

        if map_a >= 0:
            code, rep = hot(port, seek_maps="%d" % map_a)
            check("H3: seek_maps picks the node playing that map", code == 200 and rep.get("gameId") == game_a[0], "%s %s" % (code, rep))
        else:
            check("H3: the game list names the map index", False, str(game_a))

        code, rep = hot(port, game=game_b[0])
        check("H4: game=<id> answers that lobby", code == 200 and rep.get("gameId") == game_b[0], "%s %s" % (code, rep))

        code, rep = hot(port, seek_modes="999,x,", seek_maps="-4")
        check("H5: only invalid entries: the rotation", code == 200 and rep.get("gameId") in ids, "%s %s" % (code, rep))

        code, rep = hot(port, seek_modes="%d" % mode_a, seek_maps="%d" % game_b[4].get("mapIndex", -1))
        check("H6: a preference no lobby plays is refused (the page falls back)", code >= 400 and isinstance(rep, (dict, str)),
              "%s %s" % (code, rep))

        code, body = http(port, "/mm/hot-seek", method="OPTIONS")
        check("H7: a CORS preflight is 204 with no answer (no ticket, no seat)", code == 204 and not body, "%s %s" % (code, body))

        strict = start_brain(env, os.path.join(WORK, "brain-strict.log"),
                             ["--brain.matchmaking.require_validation_token=true"])
        if strict:
            code, rep = http(strict, "/mm/seek-game?region=local")
            refused = code >= 400 and "Token" in json.dumps(rep)
            code, rep = hot(strict)  # (this brain has no node: past the token, it finds no server)
            check("H8: a brain requiring validation tokens serves the hot seek (seek-game without one is refused)",
                  refused and "Token" not in json.dumps(rep), "%s %s / seek-game refused: %s" % (code, rep, refused))
        else:
            check("H8: the strict brain starts", False, read_log(os.path.join(WORK, "brain-strict.log"))[-300:])

        cap = start_brain(env, os.path.join(WORK, "brain-captcha.log"), ["--captcha.enforce=true"])
        if cap:
            code, rep = hot(cap)
            check("H9: a brain enforcing the seek captcha answers CaptchaRequired", code == 400 and "CaptchaRequired" in json.dumps(rep),
                  "%s %s" % (code, rep))
        else:
            check("H9: the captcha brain starts", False, read_log(os.path.join(WORK, "brain-captcha.log"))[-300:])
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

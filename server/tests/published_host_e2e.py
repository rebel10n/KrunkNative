#!/usr/bin/env python3
"""Published maps hosted by server_bin (server/src/published_maps.c, accounts.c) and picked in the
Host Game window (client/src/ui/host_flow_ui.c: the Select banner, the last step's command), against a local kn_brain.

  python3 -I server/tests/published_host_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite). An account publishes GOOD and SECOND.
Fresh server_bin processes then host it by name, one at a time (every port from kn_proto.free_ports()):

  A  the command line's map argument published:<creator>/<GOOD> (brain-linked): fetched, checked,
     cached on disk, played; a headless client_bin seeks, joins A and plays GOOD, then opens the Host
     Game flow from the Games window's Host button: the flow's map step lists GOOD and its server
     command (the Server tab) names it (gameserver.maps=published:<creator>/<GOOD>);
  B  that command's --set gameserver.maps=published:<creator>/<GOOD>; then the brain's host request
     (/mm/host-game, customMap): a missing map is refused with the brain's answer; SECOND (another
     published map) is fetched, checked, cached and hosted;
  C  a server.cfg file's mapcycle "published:<creator>/<GOOD>,published:<creator>/<missing>": GOOD
     plays, the missing one is skipped with a log line;
  D  a published map the brain does not have: a log line, and the server plays its configured maps;
  E  a brain that cannot be reached, with A's disk cache: the cached copy is played.

Needs a headless GL device for the client (KRUNKNATIVE_HEADLESS=1: EGL). Exit 77 when the database
or python websockets is missing.
"""
import asyncio
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
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

try:
    import websockets
except ImportError:  # pragma: no cover
    print("python websockets missing: skip")
    sys.exit(77)

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_pubhost_%d" % os.getpid()
SECRET = "kn-pubhost-%d" % os.getpid()
WORK = tempfile.mkdtemp(prefix="kn-published-host-", dir=os.path.expanduser("~/.cache"))
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=20.0):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return None


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {}, method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def lobbies(port):
    code, body = http(port, "/mm/game-list?hostname=krunknative")
    return [g[0] for g in body.get("games", [])] if code == 200 else []


async def publish(port, account, mapnames):
    async with websockets.connect("ws://127.0.0.1:%d/social/ws?enc=json" % port, max_size=4 << 20) as ws:
        async def call(frame, labels):
            await ws.send(json.dumps(frame))
            end = time.time() + 10
            while True:
                f = json.loads(await asyncio.wait_for(ws.recv(), end - time.time()))
                if f[0] in labels:
                    return f
        r = await call(["a", 0, [account, "pubhostpass1", None], None], ("a",))
        pubs = []
        for mapname in mapnames:
            doc = {"name": mapname, "objects": [{"p": [0, 0, 0], "s": [200, 2, 200]}, {"p": [30, 2, 30], "s": [10, 10, 10]}],
                   "spawns": [[0, 2, 0, 0, 0, 0], [20, 2, 20, 0, 0, 0], [-20, 2, -20, 0, 0, 0]], "modes": [0]}
            pubs.append(await call(["uploadM", json.dumps(doc), None, "e2e", "", 1, False, False], ("pubMR", "upMp")))
        return r[2], r[5], pubs


def server(tag, brain_port, game_port, extra, cache, brain_url=None, cfg_text=None):
    """One server_bin (brain-linked unless brain_url says otherwise); its log path."""
    log = os.path.join(WORK, tag + ".log")
    gs = os.path.join(WORK, tag + ".json")
    cfg = {"node_id": "PUB" + tag, "name": "published host e2e " + tag, "region": "local", "listen_port": game_port,
           "public_host": "127.0.0.1", "brain_url": brain_url or "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
           "brain_secret": SECRET, "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0,
           "finder_url": "http://127.0.0.1:%d" % brain_port}
    with open(gs, "w") as f:
        json.dump({"gameserver": cfg}, f)
    cfg_arg = "none"
    if cfg_text:
        cfg_arg = os.path.join(WORK, tag + ".cfg")
        with open(cfg_arg, "w") as f:
            f.write(cfg_text)
    env = dict(os.environ, XDG_CACHE_HOME=cache)
    p = subprocess.Popen([os.path.join(BUILD, "server_bin"), "--headless", "--config", gs, "--cfg", cfg_arg] + extra, cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    return p, log


def stop(p):
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGTERM)
    try:
        p.wait(10)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()


def loaded_map(log):
    m = wait_for(lambda: re.search(r"Loaded map (.+?) \(-?\d+\), mode", read(log)), 25)
    return m.group(1) if m else None


def script_line(text, kind, label):
    m = re.search(r"\[input_script\] %s t=\d+ @%s (.*)" % (kind, re.escape(label)), text)
    return m.group(1) if m else ""


def grep_hits(text, label):
    m = re.search(r"hits=(\d+)", script_line(text, "grep", label))
    return int(m.group(1)) if m else -1


CLIENT_SCRIPT = """0 menu
1500 text @playing [id=mapInfo]
1600 call clearPops
1700 call selectHostMap "{good}" "" "1" "{account}" 0
4000 grep @cmd {spec} [id=hostFlowCommand]
4100 call knHostFlow "go" 0
5500 grep @card {good} [id=hostCMapPickr]
5600 call knHostFlow "map" "kanji"
6000 call knHostFlow "go" 3
6500 grep @cmd2 published:{account}/ [id=hostFlowCommand]
6600 grep @cmd3 gameserver.maps=kanji [id=hostFlowCommand]
6700 quit
"""


def run_client(brain_port, account, token, good, spec):
    cfg = os.path.join(WORK, "client-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    for k, v in (("krunker_username", account), ("krunker_token", token), ("knRuleset", "v10")):
        with open(os.path.join(cfg, "krunknative", k + ".json"), "w") as f:
            f.write(v)
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(CLIENT_SCRIPT.format(good=good, account=account, spec=spec))
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, "client-state"), "XDG_CACHE_HOME": os.path.join(WORK, "client-cache"),
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % brain_port})
    log = os.path.join(WORK, "client.log")
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", os.path.join(BUILD, "client_bin")], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    try:
        p.wait(180)
        check("client_bin exits cleanly", p.returncode == 0, "exit %d (%s)" % (p.returncode, log))
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
        check("client_bin finished its script", False, log)
    return read(log)


def main():
    for exe in ("client_bin", "server_bin", "services/brain/kn_brain"):
        if not os.path.exists(os.path.join(BUILD, exe)):
            print("published_host_e2e: %s not built: skipped" % exe)
            return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("published_host_e2e: %s unavailable: skipped" % DRIVER)
        return 77
    dbcfg = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()})
    brain_log = os.path.join(WORK, "brain.log")
    ports = kn_proto.free_ports(6)
    try:
        subprocess.run([os.path.join(BUILD, "services/brain/kn_brain"), "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([os.path.join(BUILD, "services/brain/kn_brain"), "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--accounts.signup_rewards.enabled=false", "--brain.region=local",
                                  "--brain.id=pubhost-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(brain_log)), 15)
        check("kn_brain starts", bool(m), read(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        stamp = str(int(time.time()) % 100000)
        account, good, second = "pubh" + stamp, "PubHost" + stamp, "PubTwo" + stamp
        acc_id, token, pubs = asyncio.run(publish(port, account, [good, second]))
        check("GOOD and SECOND published", all(p[0] == "pubMR" for p in pubs), str(pubs))
        spec = "published:%s/%s" % (account, good)
        cache_a = os.path.join(WORK, "cache-a")

        # A: the command line's map argument; a client joins it and opens the Host Game flow
        a, log_a = server("A", port, ports[0], [spec], cache_a)
        check("A: the map argument is fetched from the brain", wait_for(lambda: "[maps] published map '%s' by %s fetched from the brain" % (good, account) in read(log_a), 20),
              read(log_a)[-600:])
        fetched = re.search(r"\[maps\] published map '%s' by %s fetched from the brain \(map id (\d+)\)" % (re.escape(good), re.escape(account)), read(log_a))
        check("A: the brain gives the map's id (meta=1: v10.0.3 customMapData.id)", fetched and int(fetched.group(1)) > 0, read(log_a)[-600:])
        check("A: plays GOOD", loaded_map(log_a) == good, str(loaded_map(log_a)))
        cached = os.path.join(cache_a, "krunknative", "published-maps", "%s~%s.json" % (account.lower(), good.lower()))
        check("A: the checked copy is cached on disk", os.path.exists(cached), cached)
        check("A: its lobby is listed", wait_for(lambda: len(lobbies(port)) == 1, 20), str(lobbies(port)))
        text = run_client(port, account, token, good, spec)
        playing = script_line(text, "text", "playing")
        check("the client joined A and plays GOOD", playing.endswith("on " + good), playing)
        check("Games window Host button on GOOD: the Host Game window opens with it", "selectHostMap: %s -> host flow" % good in text)
        check("the flow's server command names it (gameserver.maps=%s)" % spec, grep_hits(text, "cmd") == 1, script_line(text, "grep", "cmd"))
        check("the Host window's Select banner names GOOD (the community map)", grep_hits(text, "card") == 1, script_line(text, "grep", "card"))
        check("clicking a published map's card in the map step puts it in the command (not the catalogue map before it)",
              grep_hits(text, "cmd2") == 1 and grep_hits(text, "cmd3") == 0, script_line(text, "grep", "cmd2") + " / " + script_line(text, "grep", "cmd3"))
        stop(a)
        wait_for(lambda: not lobbies(port), 20)

        # B: the flow's --set gameserver.maps=...; then the brain's host request
        b, log_b = server("B", port, ports[1], ["--set", "gameserver.maps=" + spec], os.path.join(WORK, "cache-b"))
        check("B: --set gameserver.maps=%s plays GOOD" % spec, loaded_map(log_b) == good, read(log_b)[-600:])
        check("B: its lobby is listed", wait_for(lambda: len(lobbies(port)) == 1, 20), str(lobbies(port)))
        code, reply = http(port, "/mm/host-game?token=" + token, {"config": {"customMap": "published:%s/Nope%s" % (account, stamp)}})
        check("host request for a missing published map: refused with the brain's answer", code != 200 and "map not found" in json.dumps(reply),
              "%s %s" % (code, str(reply)[:200]))
        code, reply = http(port, "/mm/host-game?token=" + token, {"config": {"customMap": second}})
        check("host request for SECOND by name: hosted", code == 200 and reply.get("clientId"), "%s %s" % (code, str(reply)[:200]))
        check("B fetched SECOND for the host request, checked it and cached it",
              "[brain] fetching published map '%s'" % second in read(log_b)
              and os.path.exists(os.path.join(WORK, "cache-b", "krunknative", "published-maps", "~%s.json" % second.lower())),
              read(log_b)[-600:])
        stop(b)

        # C: server.cfg's mapcycle (a missing entry is skipped with a log line)
        missing = "published:%s/Nope%s" % (account, stamp)
        c, log_c = server("C", port, ports[2], [], os.path.join(WORK, "cache-c"), cfg_text="mapcycle %s,%s\n" % (spec, missing))
        check("C: server.cfg mapcycle %s,%s plays GOOD" % (spec, missing), loaded_map(log_c) == good, read(log_c)[-600:])
        check("C: the missing entry is skipped with a log line", "gameserver.maps: published map '%s' skipped (map not found)" % missing in read(log_c),
              read(log_c)[-600:])
        stop(c)

        # D: a map the brain does not have
        d, log_d = server("D", port, ports[3], ["published:%s/Nope%s" % (account, stamp)], os.path.join(WORK, "cache-d"))
        played = loaded_map(log_d)
        check("D: a missing published map: a log line, then the configured maps",
              "unavailable: map not found" in read(log_d) and "playing the configured maps instead" in read(log_d) and played and played != good,
              read(log_d)[-600:])
        stop(d)

        # E: the brain cannot be reached; A's disk cache has the map
        dead = ports[5]
        e, log_e = server("E", port, ports[4], [spec], cache_a, brain_url="ws://127.0.0.1:%d/mm/internal/ws" % dead)
        check("E: the brain is unreachable: the cached copy plays",
              loaded_map(log_e) == good and "playing the cached copy" in read(log_e), read(log_e)[-600:])
        stop(e)
    finally:
        for p in procs:
            stop(p)
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

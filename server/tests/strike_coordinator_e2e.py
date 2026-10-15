#!/usr/bin/env python3
"""One coordinator for both apps (doc/backend.md "Flavor pools"): a Krunker Strike server_bin and a
Krunker Box Office server_bin register to the same kn_brain, and each app only ever sees, seeks and
joins its own lobbies.

  python3 -I server/tests/strike_coordinator_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default postgres from config/config.json of
KN_CONFIG_ROOT or the repository root; services/brain/tests/db_scratch.py) + two brain-linked
server_bin nodes in one region: SCBASE1 (the base game) and SCSTRIKE1 (--flavor strike, the Strike
rotation), both also heartbeating to the brain's finder. Every port from kn_proto.free_ports().

  1. the coordinator: /mm/game-list without ?flavor= lists only the base lobby (its tuple carries no
     flavor), ?flavor=strike only the Strike lobby (meta.flavor "strike"), ?flavor=all both; the
     finder's /servers the same way; /mm/ping-list, /mm/regions and /mm/game-info per pool
  2. seeks: a Box Office seek always lands on SCBASE1, a Strike seek on SCSTRIKE1 (although the
     base lobby has players); asking for the other app's lobby by id is InvalidGameId, or (with
     autoChangeGame) the app's own lobby
  3. joins with the right client flavor: the base ticket + a base HELLO spawns on SCBASE1, the Strike
     ticket + a Strike HELLO spawns on SCSTRIKE1 (the coordinator's signed ticket admits it); a Strike
     HELLO on the base server is refused
  4. client_bin (headless): a Strike client (KN_FLAVOR=strike) seeks through the coordinator, joins
     SCSTRIKE1 and its server list shows only Strike lobbies; a Box Office client joins SCBASE1 and
     lists only Box Office ones (skipped without assets/css/fonts: KRUNKNATIVE_ASSETS_DIR)
  5. the base server stops: a Box Office seek finds nothing (never the Strike lobby) while a Strike
     seek still lands on SCSTRIKE1

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
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
CONFIG_ROOT = os.environ.get("KN_CONFIG_ROOT", ROOT)
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "postgres")
NAME = "kn_test_strikecoord_%d" % os.getpid()
SECRET = "kn-strikecoord-%d" % os.getpid()
BASE_PORT, STRIKE_PORT = kn_proto.free_ports(2)
WORK = tempfile.mkdtemp(prefix="kn-strike-coord-e2e-", dir=os.environ.get("TMPDIR"))
INIT, SPAWN, ERROR = 2, 4, 30
# protocol 1.4.2 + the rulesets byte: the v10 bit, the flavor in the high nibble (shared.h NET_WIRE_FLAVOR)
HELLO_BASE = bytes([1, 4, 2, 1 << 2])
HELLO_STRIKE = bytes([1, 4, 2, (1 << 2) | (1 << 4)])
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(str(detail)) <= 120)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if show else ''}", flush=True)


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


def games(port, flavor=None):
    """{gameId: tuple} of the e2e nodes' lobbies in /mm/game-list (?flavor=)."""
    code, body = http(port, "/mm/game-list?hostname=krunknative" + ("&flavor=" + flavor if flavor is not None else ""))
    return {g[0]: g for g in body.get("games", []) if g[0].startswith("SC")} if code == 200 else {}


def seek(port, flavor=None, game="", auto=True):
    path = "/mm/seek-game?hostname=krunknative&region=local&autoChangeGame=%s" % ("true" if auto else "false")
    if flavor is not None:
        path += "&flavor=" + flavor
    if game:
        path += "&game=" + game
    return http(port, path)


def join(reply, hello):
    """Connect with a seek-game reply and the given HELLO; (conn, spawned, error payload)."""
    c = kn_proto.Conn("127.0.0.1", int(reply["port"]))
    c.send(kn_proto.HELLO, hello)
    c.join(client_key=reply.get("clientId", ""), game_id=reply.get("gameId", ""))
    if c.read(until=INIT, timeout=5) is None or c.read(until=ERROR, timeout=1.0) is not None:
        return c, False, next((p for t, p in c.packets if t == ERROR), b"no INIT")  # refused before any ENT (flavor_net_e2e)
    c.ent(class_index=0, lag_comp=1.0)
    end = time.time() + 8
    while time.time() < end:
        payload = c.read(until=SPAWN, timeout=max(0.1, end - time.time()))
        if payload is None:
            break
        sp = kn_proto.spawn_info(payload)
        if sp and sp["is_you"]:
            return c, True, None
    err = next((p for t, p in c.packets if t == ERROR), None)
    return c, False, err


def assets_dir():
    for cand in [os.environ.get("KRUNKNATIVE_ASSETS_DIR"), os.path.join(ROOT, "assets")]:
        if cand and os.path.exists(os.path.join(cand, "css", "fonts", "font2.ttf")):
            return os.path.abspath(cand)
    return None


# The Servers window (showWindow 2), each region group expanded in turn (setExpandedRegion): how many
# rows name the Strike node / the base node. Selectors as [id=...]: '#' starts a script comment.
GROUPS = 8
CLIENT_SCRIPT = "0 menu\n3000 state\n3100 call showWindow 2\n" + "".join(
    "%d call setExpandedRegion %d\n%d grep @list%d SCSTRIKE [id=serverHolder]\n%d grep @list%d SCBASE [id=serverHolder]\n"
    % (5000 + 400 * i, i, 5300 + 400 * i, i, 5300 + 400 * i, i) for i in range(GROUPS)) + "%d quit\n" % (5600 + 400 * GROUPS)


def client(name, brain_port, flavor, assets, secs=120):
    """One headless client_bin through the coordinator (KN_FLAVOR = the app); returns its log."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(CLIENT_SCRIPT)
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KN_FLAVOR", "KRUNKNATIVE_UI_DIR")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % brain_port, "KRUNKNATIVE_ASSETS_DIR": assets,
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": "off"})
    if flavor:
        env["KN_FLAVOR"] = flavor
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    try:
        p.wait(secs)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
        check(name + ": client finished its script", False, "timed out; log " + log)
    return read_log(log)


def grep_hits(text, needle):
    """(server lists probed, rows naming needle) over every expanded region group."""
    found = re.findall(r"\[input_script\] grep t=\d+ @list\d+ needle=%s n=(\d+) hits=(\d+)" % re.escape(needle), text)
    return sum(int(n) for n, _ in found), sum(int(h) for _, h in found)


def start_node(node_id, port, brain_port, flavor):
    gs = os.path.join(WORK, node_id + ".json")
    conf = {"node_id": node_id, "name": "strike coord e2e " + node_id, "region": "local", "listen_port": port,
            "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
            "brain_secret": SECRET, "finder_url": "http://127.0.0.1:%d" % brain_port, "max_players": 8}
    args = [f"{BUILD}/server_bin", "--headless", "--config", gs]
    if flavor:
        conf.update({"flavor": flavor, "modes": ["rotation"]})  # the deploy's Strike instance (strike.cfg.j2)
        args += ["--flavor", flavor]
    else:
        conf.update({"modes": ["ffa"]})
    with open(gs, "w") as f:
        json.dump({"gameserver": conf}, f)
    log = os.path.join(WORK, node_id + ".log")
    env = {k: v for k, v in os.environ.items() if k != "KRUNKNATIVE_ASSETS_DIR"}  # maps: this tree's assets/
    p = subprocess.Popen(args, cwd=ROOT, env=dict(env, KN_BRAIN_INTERNAL_SECRET=SECRET), stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    return p, log


def main():
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), CONFIG_ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("strike_coordinator_e2e: %s unavailable: skipped" % DRIVER)
        return 77
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    conns = []
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                       "--brain.region=local", "--brain.id=strikecoord-%d" % os.getpid(),
                                       "--finder.poll_interval_s=1"], cwd=ROOT, env=env,
                                      stdout=open(brain_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        base_proc, base_log = start_node("SCBASE1", BASE_PORT, port, None)
        _, strike_log = start_node("SCSTRIKE1", STRIKE_PORT, port, "strike")
        check("SCBASE1 linked to the coordinator", wait_for(lambda: "welcomed" in read_log(base_log), 20))
        check("SCSTRIKE1 (--flavor strike) linked to the coordinator", wait_for(lambda: "welcomed" in read_log(strike_log), 20))
        check("SCSTRIKE1 plays the Strike flavor", "Flavor: Krunker Strike (strike)" in read_log(strike_log),
              [l for l in read_log(strike_log).splitlines() if "lavor" in l][:3])

        # 1. listings per pool
        both = wait_for(lambda: (lambda g: g if len(g) == 2 else None)(games(port, "all")), 15) or {}
        check("?flavor=all lists both lobbies", len(both) == 2, list(both))
        base_games, strike_games = games(port), games(port, "strike")
        base_id = next(iter(base_games), "")
        strike_id = next(iter(strike_games), "")
        check("Box Office game list: only SCBASE1's lobby, no flavor in its tuple",
              list(base_games) == [base_id] and base_id.startswith("SCBASE1:") and "flavor" not in base_games[base_id][4], base_games)
        check("Strike game list: only SCSTRIKE1's lobby, meta.flavor strike",
              list(strike_games) == [strike_id] and strike_id.startswith("SCSTRIKE1:")
              and strike_games[strike_id][4].get("flavor") == "strike", strike_games)
        check("?flavor=base is the Box Office list", list(games(port, "base")) == [base_id])

        def finder(q):
            code, body = http(port, "/servers" + q)
            return sorted(s.get("port") for s in body.get("servers", [])) if code == 200 else None
        check("finder: Box Office /servers lists only the base server",
              wait_for(lambda: finder("") == [BASE_PORT], 20), finder(""))
        check("finder: /servers?flavor=strike lists only the Strike server",
              wait_for(lambda: finder("?flavor=strike") == [STRIKE_PORT], 20), finder("?flavor=strike"))
        code, pl = http(port, "/mm/ping-list?flavor=strike")
        code2, pl2 = http(port, "/mm/ping-list")
        check("ping-list: the region in both pools", code == 200 and "local" in pl and code2 == 200 and "local" in pl2, (pl, pl2))
        code, rg = http(port, "/mm/regions?flavor=strike")
        loc = next((x for x in rg if x.get("id") == "local"), {}) if code == 200 else {}
        code2, rg2 = http(port, "/mm/regions")
        loc2 = next((x for x in rg2 if x.get("id") == "local"), {}) if code2 == 200 else {}
        check("regions: one node per pool", loc.get("nodes") == 1 and loc2.get("nodes") == 1, (loc, loc2))
        code, gi = http(port, "/mm/game-info?game=" + strike_id)
        check("game-info: a Box Office client asking for the Strike lobby gets a Box Office one",
              code == 200 and gi[0] == base_id, (code, gi and gi[0]))
        code, gi = http(port, "/mm/game-info?flavor=strike&game=" + strike_id)
        check("game-info: the Strike app gets its lobby", code == 200 and gi[0] == strike_id, (code, gi and gi[0]))

        # 2. + 3. seeks and joins: a Box Office player first, so the base lobby is the fuller one
        code, rep = seek(port)
        check("Box Office seek -> SCBASE1", code == 200 and rep.get("gameId") == base_id and int(rep.get("port", 0)) == BASE_PORT,
              (code, rep.get("gameId") if isinstance(rep, dict) else rep))
        c, ok, err = join(rep, HELLO_BASE) if code == 200 else (None, False, None)
        check("Box Office player (base HELLO + coordinator ticket) spawns on SCBASE1", ok, err)
        if c:
            conns.append(c)
        landed = []
        for _ in range(3):
            code, rep = seek(port, "strike")
            landed.append(rep.get("gameId") if code == 200 else "HTTP %d %s" % (code, rep))
        check("Strike seeks -> SCSTRIKE1 every time (never the fuller base lobby)", landed == [strike_id] * 3, landed)
        code, rep = seek(port, "strike")
        c, ok, err = join(rep, HELLO_STRIKE) if code == 200 else (None, False, None)
        check("Strike player (Strike HELLO + coordinator ticket) spawns on SCSTRIKE1",
              ok and int(rep.get("port", 0)) == STRIKE_PORT, err)
        if c:
            conns.append(c)
        bo = [seek(port)[1].get("gameId") for _ in range(3)]
        check("Box Office seeks -> SCBASE1 every time", bo == [base_id] * 3, bo)
        code, rep = seek(port, None, strike_id, auto=False)
        check("Box Office seek for the Strike lobby by id: InvalidGameId", code == 404 and rep.get("error") == "InvalidGameId", (code, rep))
        code, rep = seek(port, None, strike_id, auto=True)
        check("... with autoChangeGame: its own lobby instead", code == 200 and rep.get("gameId") == base_id and rep.get("changeReason"),
              (code, rep.get("gameId"), rep.get("changeReason")))
        code, rep = seek(port, "strike", base_id, auto=False)
        check("Strike seek for the base lobby by id: InvalidGameId", code == 404 and rep.get("error") == "InvalidGameId", (code, rep))
        code, rep = seek(port, "strike", strike_id, auto=False)
        check("Strike seek for its own lobby by id", code == 200 and rep.get("gameId") == strike_id, (code, rep.get("gameId")))
        # a Strike client that would reach the base server anyway (a stale link) is refused there
        code, rep = seek(port)
        c, ok, err = join(rep, HELLO_STRIKE) if code == 200 else (None, False, None)
        check("a Strike HELLO on the base server is refused", not ok and err is not None, err)
        if c:
            c.close()
        counts = wait_for(lambda: (lambda g: g if g.get(base_id, [0, 0, 0])[2] == 1 and g.get(strike_id, [0, 0, 0])[2] == 1 else None)(
            games(port, "all")), 8) or games(port, "all")
        check("each lobby counts its own player", counts.get(base_id, [0, 0, 0])[2] == 1 and counts.get(strike_id, [0, 0, 0])[2] == 1,
              {k: v[2] for k, v in counts.items()})
        for c in conns:
            c.close()
        conns.clear()

        # 4. the two apps through the coordinator
        assets = assets_dir()
        if not os.access(f"{BUILD}/client_bin", os.X_OK) or not assets:
            print("NOTE  client_bin cases skipped (no client_bin or no assets/css/fonts: set KRUNKNATIVE_ASSETS_DIR)")
        else:
            text = client("strike-client", port, "strike", assets)
            check("Strike client: joins SCSTRIKE1 through the coordinator",
                  re.search(r"\[mm\] joining SCSTRIKE1:\S+ at 127\.0\.0\.1:%d" % STRIKE_PORT, text) and "connected to server" in text
                  and "flavor mismatch" not in text, [l for l in text.splitlines() if "[mm]" in l or "flavor" in l][:8])
            # the Strike page (FRVR's mobile UI, ui/strike/) has no Servers window: when it renders
            # none, the list data it would show (the coordinator's game list and the finder, fetched
            # by the same showWindow) must be the Strike pool's
            lists = re.findall(r"\[mm\] game list \((\w+)\): (\d+) lobbies (\S*)", text)
            finder = re.findall(r"\[finder\] GET (\S+)", text)
            dom = grep_hits(text, "SCSTRIKE")[0] or grep_hits(text, "SCBASE")[0]
            check("Strike client: its server list data is the Strike pool only (game list + finder)",
                  lists and all(f == "strike" and "SCBASE1" not in ids and "SCSTRIKE1" in ids for f, _, ids in lists)
                  and finder and all("flavor=strike" in u for u in finder), (lists, finder))
            check("Strike client: its server list shows the Strike lobby, not the base one" +
                  ("" if dom else " (no Servers window on the Strike page: data checked above)"),
                  (grep_hits(text, "SCSTRIKE")[1] and grep_hits(text, "SCBASE")[1] == 0) if dom else lists,
                  (grep_hits(text, "SCSTRIKE"), grep_hits(text, "SCBASE")))
            text = client("base-client", port, None, assets)
            check("Box Office client: joins SCBASE1 through the coordinator",
                  re.search(r"\[mm\] joining SCBASE1:\S+ at 127\.0\.0\.1:%d" % BASE_PORT, text) and "connected to server" in text,
                  [l for l in text.splitlines() if "[mm]" in l][:8])
            lists = re.findall(r"\[mm\] game list \((\w+)\): (\d+) lobbies (\S*)", text)
            check("Box Office client: its server list data is the base pool only (game list + finder)",
                  lists and all(f == "base" and "SCSTRIKE1" not in ids and "SCBASE1" in ids for f, _, ids in lists)
                  and all("flavor=" not in u for u in re.findall(r"\[finder\] GET (\S+)", text)), lists)
            check("Box Office client: its server list shows the base lobby, not the Strike one",
                  grep_hits(text, "SCBASE")[1] and grep_hits(text, "SCSTRIKE")[1] == 0,
                  (grep_hits(text, "SCBASE"), grep_hits(text, "SCSTRIKE")))

        # 5. only Strike lobbies left: Box Office finds nothing, Strike still plays
        base_proc.terminate()
        try:
            base_proc.wait(15)
        except subprocess.TimeoutExpired:
            base_proc.kill()
        check("SCBASE1 leaves the game list", wait_for(lambda: not games(port) and games(port, "strike"), 15), list(games(port, "all")))
        code, rep = seek(port)
        check("Box Office seek with only a Strike lobby up: NoAvailableServers (never the Strike lobby)",
              code == 503 and rep.get("error") == "NoAvailableServers", (code, rep))
        code, rep = seek(port, "strike")
        check("Strike seek still -> SCSTRIKE1", code == 200 and rep.get("gameId") == strike_id, (code, rep))
    finally:
        for c in conns:
            c.close()
        for p in procs:
            if p.poll() is None:
                p.terminate()
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    bad = [l for l in read_log(brain_log).splitlines() if re.search(r"failed:|ERROR|FATAL", l)]
    check("no statement errors in the brain log", not bad, str(bad[:3]))
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

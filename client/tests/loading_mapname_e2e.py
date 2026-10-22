#!/usr/bin/env python3
"""The loading screen of a Box Office join names the lobby's map, as a krunker.io join does ("Loading Burg ...").

  python3 -I client/tests/loading_mapname_e2e.py BUILD_DIR      (run from the repository root)

A local kn_brain (sqlite, temp dir) with a kn_fakenode game server that lists one lobby on Burg, and a headless
client_bin that starts with its launch seek (knSeekOnLaunch: the Quick Play path, the Servers window never
opened, so the client holds no game list). The lobby's game server is a TCP listener that accepts and never
answers, so the connecting spinner stays up to be read. Nothing reaches production: krunker.io, its matchmaker
and finder point at a closed local port.

  M0 the brain's seek answer (the join answer, /mm/seek-game) names the lobby's map: "map": "Burg"
  M1 the client's join of the Box Office lobby is told the map ("connecting (Burg)") and its loading screen's
     text (#instructions, textof) reads "Loading Burg ..."
  M2 the screenshot of that loading screen is written (viewed by hand: the spinner and "Loading Burg ...")

Prints PASS/FAIL lines; exit code 0 only when everything passed. 77 = skipped (no kn_brain / kn_fakenode / client_bin).
"""
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

BUILD = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath("cmake-build-debug")
REPO = os.getcwd()
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
FAKE = os.path.join(BUILD, "services", "brain", "kn_fakenode")
CLIENT = os.path.join(BUILD, "client_bin")
if not all(os.path.exists(p) for p in (BRAIN, FAKE, CLIENT)):
    print(f"needs kn_brain, kn_fakenode and client_bin in {BUILD}: skip")
    sys.exit(77)

WORK = tempfile.mkdtemp(prefix="kn-loading-mapname.", dir=os.path.expanduser("~/.cache"))
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR") or (
    os.path.join(REPO, "assets") if os.path.isdir(os.path.join(REPO, "assets", "models"))
    else os.path.expanduser("~/src/KrunkNative/assets"))
SECRET = "loading-mapname-%d" % os.getpid()
LOBBY_MAP = "Burg"                 # the lobby's map: the catalogue name of its map (assets/maps/*_burg.json)
LOBBY_MAP_INDEX = 0                # Krunker's mapIndex of Burg (shared/src/map_catalog.c OFFICIAL "burg_new"), in the public rotation
NODE = "TBURG1"
results = []
procs = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if detail else ''}", flush=True)


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for(fn, seconds):
    end = time.time() + seconds
    while time.time() < end:
        try:
            if fn():
                return True
        except OSError:
            pass
        time.sleep(0.2)
    return False


def get_json(port, path):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=5) as r:
        return json.loads(r.read().decode() or "null")


def get_json_status(port, path):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=5) as r:
            return r.status, json.loads(r.read().decode() or "null")
    except urllib.error.HTTPError as e:
        return e.code, None


def env_base():
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null",
                "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_ASSET_FETCH": "off", "KRUNKNATIVE_KRUNKER_MATCHMAKER": "off",
                "KRUNKNATIVE_ASSETS_DIR": ASSETS, "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    # no production: krunker.io, its matchmaker and finder point at a closed local port
    for k in ("KRUNKNATIVE_KRUNKER_URL", "KRUNKNATIVE_KRUNKER_MATCHMAKER", "KRUNKNATIVE_FINDER_URL", "KRUNKNATIVE_KRUNKER_API",
              "KRUNKNATIVE_KRUNKER_AUTH", "KRUNKNATIVE_KRUNKER_SOCIAL", "KRUNKNATIVE_KRUNKER_EVENT_BUS",
              "KRUNKNATIVE_KRUNKER_DATA_API"):
        env[k] = "http://127.0.0.1:9"
    return env


def spawn(name, argv, env=None, log_name=None, cwd=None):
    out = open(os.path.join(WORK, log_name or (name + ".log")), "w")
    p = subprocess.Popen(argv, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         env=env, cwd=cwd or REPO, start_new_session=True)
    out.close()
    procs.append(p)
    return p


def stop_all():
    for p in procs:
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGTERM)  # only the groups this test started
            except ProcessLookupError:
                pass
    for p in procs:
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def start_brain(port):
    cfg = os.path.join(WORK, "brain.json")  # its own config: never the developer's config/config.json
    with open(cfg, "w") as f:
        json.dump({"database": {"driver": "sqlite", "path": os.path.join(WORK, "coordinator.db"),
                                "migrations_dir": os.path.join(REPO, "services", "db", "migrations")},
                   "brain": {"host": "127.0.0.1", "port": port, "federation": False, "internal_secret": SECRET}}, f)
    spawn("brain", [BRAIN, "--config", cfg, "--migrate"], log_name="brain.log", cwd=REPO)
    return wait_for(lambda: get_json(port, "/mm/game-list") is not None, 30)


def start_game_listener():
    """The lobby's game server: a TCP port that accepts and never answers, so the join waits on its INIT."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(8)
    held = []

    def accept_forever():
        while True:
            try:
                conn, _ = srv.accept()
                held.append(conn)  # kept open, nothing sent
            except OSError:
                return

    threading.Thread(target=accept_forever, daemon=True).start()
    return srv, srv.getsockname()[1]


def find_lobby_id(brain_port):
    games = get_json(brain_port, "/mm/game-list") or {}
    for g in games.get("games", []):
        if isinstance(g, list) and g and isinstance(g[4], dict) and g[4].get("map") == LOBBY_MAP:
            return g[0]
    return None


def run_client(brain_port, shot):
    cfg = os.path.join(WORK, "client-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative", "knSettings.json"), "w") as f:
        json.dump({"knSeekOnLaunch": True, "defaultRegion": "us-nj"}, f)
    # the UI commands are read from a file the client polls (loading_screens_e2e.py's pattern): the picture and the
    # text of #instructions are taken while the connecting spinner is up
    cmds = os.path.join(WORK, "ui.cmds")
    open(cmds, "w").close()
    env = env_base()
    env.update({"XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": cfg + "-state", "KRUNKNATIVE_UI_CMDS": cmds,
                "KRUNKNATIVE_BRAIN_URL": f"http://127.0.0.1:{brain_port}"})
    log = os.path.join(WORK, "client.log")
    p = spawn("client", ["stdbuf", "-oL", "-eL", CLIENT, "--finder", "off"], env=env, log_name="client.log")

    def text():
        try:
            with open(log, errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def cmd(line):
        with open(cmds, "a") as f:
            f.write(line + "\n")

    if wait_for(lambda: ("[ui log] connecting (%s)" % LOBBY_MAP) in text(), 120):
        time.sleep(2.0)  # the spinner is up: its first frames are drawn
        cmd("shot " + shot)
        cmd("textof instructions")
        wait_for(lambda: os.path.exists(shot) and "[ui_cmds] textof instructions" in text(), 15)
    cmd("quit")
    try:
        p.wait(timeout=30)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        p.wait()
    return text().splitlines()


def instructions_text(lines):
    """The #instructions text the client printed for the textof probe (whitespace folded), None when not printed."""
    for l in lines:
        m = re.search(r"\[ui_cmds\] textof instructions = '(.*)'", l)
        if m:
            return m.group(1)
    return None


def main():
    brain_port = free_port()
    if not start_brain(brain_port):
        check("the local brain starts", False, os.path.join(WORK, "brain.log"))
        return False
    game_srv, game_port = start_game_listener()
    spawn("fakenode", [FAKE, "--secret", SECRET, "--brain", f"ws://127.0.0.1:{brain_port}/mm/internal/ws",
                       "--node", NODE, "--region", "us-nj", "--host", "127.0.0.1", "--port", str(game_port),
                       "--players", "1", "--map", LOBBY_MAP, "--map-index", str(LOBBY_MAP_INDEX), "--mode", "ffa",
                       "--quiet"], log_name="fakenode.log")
    listed = wait_for(lambda: find_lobby_id(brain_port) is not None, 30)
    check("the fake game server's lobby on %s is listed by the brain" % LOBBY_MAP, listed, find_lobby_id(brain_port))
    if not listed:
        return False

    # M0: the join answer. A Box Office seek answers the lobby's host, port, ticket and region; its map too.
    status, answer = get_json_status(brain_port, "/mm/seek-game?hostname=krunknative&region=any&autoChangeGame=false")
    check("M0 the brain's seek answer is a join (HTTP 200 with host, port and a ticket)",
          status == 200 and isinstance(answer, dict) and answer.get("host") and answer.get("clientId"), (status, answer))
    got = answer.get("map") if isinstance(answer, dict) else None
    check("M0 the seek answer names the lobby's map (\"map\": %r)" % LOBBY_MAP, got == LOBBY_MAP, repr(got))

    # M1/M2: a client's Quick Play join of the same lobby; the loading screen names its map
    shot = os.path.join(WORK, "loading-mapname.png")
    lines = run_client(brain_port, shot)
    joined = any("[mm] joining " in l for l in lines)
    check("M1 the client joins the Box Office lobby", joined, os.path.join(WORK, "client.log"))
    connecting = [l for l in lines if "[ui log] connecting (" in l]
    check("M1 the loading screen is told the map (\"connecting (%s)\")" % LOBBY_MAP,
          any(("connecting (%s)" % LOBBY_MAP) in l for l in connecting), connecting[-1:] or "no connecting line")
    shown = instructions_text(lines)
    check("M1 the loading screen's text reads \"Loading %s ...\" (#instructions, textof)" % LOBBY_MAP,
          shown is not None and ("Loading %s" % LOBBY_MAP) in shown, repr(shown))
    check("M2 the loading screen screenshot is written", os.path.exists(shot) and os.path.getsize(shot) > 0, shot)
    return all(results)


if __name__ == "__main__":
    try:
        ok = main()
    finally:
        stop_all()
    print(f"\n{sum(results)}/{len(results)} passed  (work: {WORK})")
    sys.exit(0 if ok else 1)

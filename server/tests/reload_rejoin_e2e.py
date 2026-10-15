#!/usr/bin/env python3
"""Reload (F5, the console `reload`) keeps the player in the same lobby, end to end
(client/src/game_reload.c, client/src/ui/servers_ui.c servers_ui_reload_resume).

  python3 -I server/tests/reload_rejoin_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite: no server needed), two
brain-linked server_bin nodes (RLA: ffa on Burg, RLB: tdm on Sandstorm), every port from
kn_proto.free_ports(). Every client saves Settings > Quickplay > Seek Mode = RLA's mode, so its
startup seek lands in RLA; the console's `connect <RLB id>` moves it to RLB.

  R1: in RLB, F5 relaunches the client (the same pid: exec); the new process rejoins RLB (the saved
      game id, never a seek by the Quickplay settings or one excluding the lobby); then the
      console's `reload` (in place) rejoins RLB too
  R2: in RLB, F5; while the new process starts, RLB goes away (its server stops): the reload falls
      back to a seek (lands in RLA) and says so ("[mm] reload: ... is gone")
  R3: in RLB, F5; while the new process starts, RLB fills up: the reload falls back to a seek
      (lands in RLA) and says so ("[mm] reload: ... is full")

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). Exit 77 when the database is unavailable.
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
NAME = "kn_test_reload_%d" % os.getpid()
SECRET = "kn-reload-e2e-secret-%d" % os.getpid()
PORT_A, PORT_B, PORT_B2, PORT_B3 = kn_proto.free_ports(4)
MAX_PLAYERS = 4
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-reload-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
procs = []


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
        time.sleep(0.05)
    return None


def http(port, path):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=10) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


def lobbies(port):
    code, body = http(port, "/mm/game-list")
    return body.get("games", []) if code == 200 else []


def lobby(port, game_id):
    return next((g for g in lobbies(port) if g[0] == game_id), None)


def start_brain(env, log):
    p = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                          "--brain.join_ttl_s=5", "--brain.id=reload-%d" % os.getpid()],
                         cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    procs.append(p)
    m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(log)), 15)
    return int(m.group(1)) if m else None


def start_node(brain_port, node_id, port, mode, map_name):
    log = os.path.join(WORK, node_id + ".log")
    cfg = os.path.join(WORK, node_id + ".json")
    with open(cfg, "w") as f:
        json.dump({"gameserver": {"node_id": node_id, "name": "reload e2e " + node_id, "region": "local", "listen_port": port,
                                  "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
                                  "brain_secret": SECRET, "modes": [mode], "maps": [map_name], "max_players": MAX_PLAYERS,
                                  "idle_kick_seconds": 0, "idle_menu_kick_seconds": 0}}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", cfg, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    wait_for(lambda: "welcomed" in read_log(log), 20)
    game = wait_for(lambda: next((g for g in lobbies(brain_port) if g[0].startswith(node_id + ":")), None), 15)
    return p, game


def stop_node(p):
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGTERM)
    try:
        p.wait(15)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()


def client(name, brain_port, script, relaunch_script, mode):
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    rpath = os.path.join(WORK, name + "-relaunch.script")
    with open(rpath, "w") as f:
        f.write(relaunch_script)
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative", "knSettings.json"), "w") as f:
        json.dump({"seek_modes": str(mode)}, f)  # Settings > Quickplay > Seek Mode: the startup seek lands in RLA
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_INPUT_SCRIPT_RELAUNCH": rpath,
                "KRUNKNATIVE_WINDOW_SIZE": "640x360", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port,
                "KRUNKNATIVE_NET_TRANSPORT": "tcp", "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:9",
                "KRUNKNATIVE_KRUNKER_URL": "https://krunker.io"})
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    return p, log


def finish(p, secs):
    try:
        p.wait(secs)
        return True
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
        return False


def joined(text):
    return re.findall(r"\[mm\] joining (\S+) at ", text)


def states(text):
    return re.findall(r"\[input_script\] state t=\d+ slot=-?\d+/(\d+) weapon=(.+?) menu=(\d)", text)


def in_game(st):
    return bool(st) and st[-1][2] == "0" and int(st[-1][0]) > 0 and st[-1][1] != "-"


def after_relaunch(text):
    i = text.find("[app] reload: restarting")
    return text[i:] if i >= 0 else ""


# the first process: the startup seek (RLA by the Quickplay mode), then RLB by its id, F5
def first_script(game_b):
    return "0 play\n1500 state\n2000 con connect %s\n7000 state\n7500 down F5\n7550 up F5\n90000 quit\n" % game_b


def hold_relaunch(p, log):
    """SIGSTOP the client once it relaunches (exec keeps the pid): the new process waits while the test changes RLB."""
    if not wait_for(lambda: "[reload] restart: F5: relaunch" in read_log(log), 40):
        return False
    os.killpg(p.pid, signal.SIGSTOP)
    return True


def fill(game, brain_port, conns):
    """Protocol clients take RLB's seats until the matchmaker calls it full."""
    for _ in range(MAX_PLAYERS + 2):
        code, rep = http(brain_port, "/mm/seek-game?hostname=krunknative&region=local&autoChangeGame=false&game=" +
                         urllib.parse.quote(game, safe=""))
        if code != 200:
            return rep and rep.get("error")
        c = kn_proto.Conn("127.0.0.1", int(rep["port"]))
        c.hello()
        c.join(client_key=rep.get("clientId", ""), game_id=rep.get("gameId", ""))
        c.read(timeout=0.5)
        conns.append(c)
    return None


def run_same(brain_port, game_a, game_b):
    p, log = client("same", brain_port, first_script(game_b[0]),
                    "0 play\n2000 state\n3000 con reload\n9000 state\n9500 quit\n", game_a[4]["mode"])
    pid = p.pid
    done = finish(p, 120)
    text = read_log(log)
    js = joined(text)
    check("R1: the startup seek joined RLA (Quickplay Seek Mode)", js[:1] == [game_a[0]], str(js))
    check("R1: `connect` moved the client to RLB", js[1:2] == [game_b[0]], str(js))
    check("R1: F5 relaunched the same process (pid %d, exit 0)" % pid,
          "[app] reload: restarting" in text and done and p.returncode == 0, "exit %s" % p.returncode)
    after = after_relaunch(text)
    rjs = joined(after)
    print("R1: lobbies joined after the relaunch: %s" % rjs, flush=True)
    check("R1: the relaunched client rejoined RLB (its saved game id)", rjs[:1] == [game_b[0]], str(rjs))
    check("R1: the reload never seeks by excludeGame or the launch seek",
          "excluding" not in after and "[mm] launch seek" not in after, after[-400:])
    check("R1: the console's `reload` (in place) rejoined RLB too", rjs[1:2] == [game_b[0]] and len(rjs) == 2, str(rjs))
    check("R1: and plays", in_game(states(after)), str(states(after)))


def run_fallback(brain_port, game_a, node_b, label, why):
    p_b, game_b = node_b
    p, log = client("fallback-" + why, brain_port, first_script(game_b[0]), "0 play\n7000 state\n7500 quit\n", game_a[4]["mode"])
    held = hold_relaunch(p, log)
    check(f"{label}: the client relaunched for F5 (held while RLB {'goes away' if why == 'gone' else 'fills up'})", held,
          read_log(log)[-400:])
    conns = []
    if held and why == "gone":
        stop_node(p_b)
        check(f"{label}: RLB is gone from the matchmaker", wait_for(lambda: lobby(brain_port, game_b[0]) is None, 15), "")
    elif held:
        err = fill(game_b[0], brain_port, conns)
        check(f"{label}: RLB is full", err == "GameFull", str(err))
    if held:
        os.killpg(p.pid, signal.SIGCONT)
    finish(p, 90)
    for c in conns:
        try:
            c.close()
        except Exception:  # noqa: BLE001 - best effort
            pass
    text = read_log(log)
    after = after_relaunch(text)
    rjs = joined(after)
    print(f"{label}: lobbies joined after the relaunch: {rjs}", flush=True)
    check(f"{label}: the reload asked for RLB first", "[reload] resume after the restart: game\t%s" % game_b[0] in after
          or "[reload] resume after the restart: game %s" % game_b[0] in after, after[:600])
    check(f"{label}: and says the lobby is {why}, then seeks another",
          re.search(r"\[mm\] reload: lobby %s is %s" % (re.escape(game_b[0]), why), after) is not None, after[-800:])
    check(f"{label}: the fallback seek landed in RLA", rjs == [game_a[0]], str(rjs))
    check(f"{label}: and plays", in_game(states(after)), str(states(after)))


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("reload_rejoin_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("reload_rejoin_e2e: %s unavailable: skipped" % DRIVER)
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
        _, game_a = start_node(port, "RLA", PORT_A, "ffa", "Burg")
        node_b = start_node(port, "RLB", PORT_B, "tdm", "Sandstorm")
        check("two lobbies up", game_a and node_b[1], "%s %s" % (game_a, node_b[1]))
        if not (game_a and node_b[1]):
            return 1
        run_same(port, game_a, node_b[1])
        run_fallback(port, game_a, node_b, "R2", "gone")
        node_b3 = start_node(port, "RLC", PORT_B3, "tdm", "Sandstorm")  # RLB stopped in R2: a new B for R3
        check("RLC (the next B) up", node_b3[1], "")
        if node_b3[1]:
            run_fallback(port, game_a, node_b3, "R3", "full")
    finally:
        for p in procs:
            if p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGCONT)
                    os.killpg(p.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
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

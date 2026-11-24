#!/usr/bin/env python3
"""The end screen's Return seeks a game, as v10's backToOrigin (deob-10.0.7.js:275828), and never disconnects.

hud_endscreen_v10.c's knReturnToKrunker runs `if (!servers_ui_quick_match()) menu_play();`. It used to call
servers_ui_disconnect, so the menu said Disconnected.

  offline: `con end`, then Return once the results show. The sandbox stays up (the same local player, no
           "[mm] disconnected by the user"), the results give way to the menu, and Play enters the next round.
  brain:   kn_brain on a scratch sqlite database and one server_bin node. The guest quick-matches into the
           node's lobby, the node's `round end` ends it, and Return starts a Quick Match seek that excludes this
           lobby ("[mm] quick match: seeking a lobby other than <id>"), not a disconnect.

  python3 -I client/tests/end_return_e2e.py BUILD_DIR [SRC]

Headless (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG_CONFIG_HOME). 77 = skip (no client_bin;
the brain case also needs server_bin, kn_brain and the sqlite scratch database).
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
import urllib.request

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
ROOT = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.getcwd())
CLIENT = os.path.join(BUILD, "client_bin")
SERVER = os.path.join(BUILD, "server_bin")
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
if not os.access(CLIENT, os.X_OK):
    print("end_return_e2e: no client_bin: skipped")
    sys.exit(77)
WORK = tempfile.mkdtemp(prefix="kn-end-return.", dir=os.path.expanduser("~/.cache"))
FAILS = []
PROCS = []
SKIPPED = []


def check(what, ok, detail=""):
    print(("  ok   " if ok else "  FAIL ") + what + ("" if ok or not detail else "  [%s]" % str(detail)[:400]), flush=True)
    if not ok:
        FAILS.append(what)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs, step=0.3):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


def client_env(tag, script, extra=None):
    cfg = os.path.join(WORK, tag + "-cfg")
    os.makedirs(cfg, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "1280x720",
                "KRUNKNATIVE_EGL_DEVICE": os.environ.get("KRUNKNATIVE_EGL_DEVICE", "0"), "LIBGL_ALWAYS_SOFTWARE": "1",
                "KRUNKNATIVE_DEBUG": "2", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, tag + "-state")})
    env.update(extra or {})
    return env


def write_script(tag, lines):
    path = os.path.join(WORK, tag + ".script")
    with open(path, "w") as f:
        f.write("".join(l + "\n" for l in lines))
    return path


def probe(text, kind, label):
    m = re.search(r"\[input_script\] %s t=\d+ @%s (.*)" % (kind, label), text)
    return dict(kv.split("=", 1) for kv in m.group(1).split() if "=" in kv) if m else {}


def state_after(text, t_min):
    for m in re.finditer(r"\[input_script\] state t=(\d+) (.*)", text):
        if int(m.group(1)) >= t_min:
            return dict(kv.split("=", 1) for kv in m.group(2).split() if "=" in kv)
    return {}


def players_at(text, t_min):
    for m in re.finditer(r"\[input_script\] players t=(\d+) (.*)", text):
        if int(m.group(1)) >= t_min:
            return m.group(2)
    return ""


def offline():
    # the round ends at once; its results are up (pointer free) by 14 s, Return at 14.1 s
    script = write_script("offline", [
        "0 play", "500 con end",
        "14000 vis @results endUI menuHolder", "14000 players", "14000 state",
        "14100 call knReturnToKrunker",
        "15000 vis @after endUI menuHolder inGameUI", "15000 players", "15000 state",
        "15000 grep @disc Disconnected body",
        "16000 play",
        "30000 vis @next endUI menuHolder inGameUI", "30000 state",
        "30100 quit"])
    log = os.path.join(WORK, "offline.log")
    with open(log, "w") as out:
        p = subprocess.Popen([CLIENT, "--offline", "--map", "kanji"], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=client_env("offline", script), start_new_session=True)
        try:
            rc = p.wait(timeout=180)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            rc = "timeout"
    text = read(log)
    check("offline: client_bin exits 0", rc == 0, "rc=%s, log %s" % (rc, log))
    res, after, nxt = probe(text, "vis", "results"), probe(text, "vis", "after"), probe(text, "vis", "next")
    s_res, s_after, s_next = state_after(text, 14000), state_after(text, 15000), state_after(text, 30000)
    check("offline: the results show before Return (endUI, round ended, pointer free)",
          res.get("endUI") == "1" and s_res.get("ended") == "1" and s_res.get("locked") == "0", "%r %r" % (res, s_res))
    check("offline: Return is dispatched", re.search(r"\[input_script\] call t=\d+ knReturnToKrunker", text) is not None)
    check("offline: Return does not disconnect", "[mm] disconnected by the user" not in text,
          [l for l in text.splitlines() if "disconnect" in l.lower()][:3])
    def me(players):  # "n=1 0:Guest_1@x,y,z*": the local player's uid:name
        m = re.search(r"(\d+:[^@\s]+)@\S+\*", players)
        return m.group(1) if m else None
    before_players, after_players = players_at(text, 14000), players_at(text, 15000)
    check("offline: the sandbox keeps the same local player", me(before_players) is not None and me(before_players) == me(after_players),
          "%r -> %r" % (before_players, after_players))
    disc = re.search(r"\[input_script\] grep t=\d+ @disc needle=Disconnected n=(\d+) hits=(\d+)", text)
    check("offline: no Disconnected on the page", disc is not None and disc.group(1) != "0" and disc.group(2) == "0",
          disc.group(0) if disc else "no grep line")
    check("offline: the results give way to the menu", after.get("endUI") == "0" and after.get("menuHolder") == "1" and
          s_after.get("menu") == "1", "%r %r" % (after, s_after))
    check("offline: Play then enters the next round", nxt.get("inGameUI") == "1" and s_next.get("ended") == "0" and
          s_next.get("menu") == "0", "%r %r" % (nxt, s_next))


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {}, method="POST" if data else "GET")
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.status, json.loads(r.read() or b"null")


def brain():
    for b in (SERVER, BRAIN):
        if not os.access(b, os.X_OK):
            print("  skip brain: no %s" % b)
            SKIPPED.append("brain")
            return
    sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
    from kn_proto import free_ports
    name, secret = "kn_test_endreturn_%d" % os.getpid(), "kn-endreturn-%d" % os.getpid()
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", "sqlite", name], capture_output=True)
    if r.returncode:
        print("  skip brain: no sqlite scratch database")
        SKIPPED.append("brain")
        return
    db = json.loads(r.stdout)
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=secret, **{"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()})
    try:
        subprocess.run([BRAIN, "--migrate-only"], cwd=ROOT, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain_log = os.path.join(WORK, "brain.log")
        PROCS.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                                       "--brain.id=endreturn-%d" % os.getpid(), "--finder.poll_interval_s=1"],
                                      cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, start_new_session=True))
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(brain_log)), 20)
        check("brain: kn_brain starts", bool(m), read(brain_log)[-300:])
        if not m:
            return
        port = int(m.group(1))
        (node_port,) = free_ports(1)
        node_log, node_cfg = os.path.join(WORK, "node.log"), os.path.join(WORK, "node.json")
        gs = {"node_id": "ENDRETURN", "name": "end return e2e", "listen_port": node_port, "public_host": "127.0.0.1",
              "region": "local", "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0,
              "ruleset": "v10", "bans_file": os.path.join(WORK, "bans.json"),
              "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % port, "brain_secret": secret,
              "finder_url": "http://127.0.0.1:%d" % port}
        with open(node_cfg, "w") as f:
            json.dump({"gameserver": gs}, f)
        node = subprocess.Popen([SERVER, "--headless", "--config", node_cfg, "--cfg", "none"], cwd=ROOT,
                                stdout=open(node_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.PIPE,
                                start_new_session=True)
        PROCS.append(node)
        check("brain: the node links to the brain", bool(wait_for(lambda: "welcomed" in read(node_log) or "linked" in read(node_log), 30)),
              read(node_log)[-300:])
        # the round is ended from the node once we are in; Return when the results are up (the pointer is free)
        script = write_script("brain", ["0 play", "100 state", "14000 vis @results endUI", "14000 state",
                                        "14100 call knReturnToKrunker", "17000 grep @disc Disconnected body", "17000 state",
                                        "20000 quit"])
        log = os.path.join(WORK, "brain-client.log")
        cenv = client_env("brain", script, {"KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % port, "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                                            "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % port})
        p = subprocess.Popen(["stdbuf", "-oL", "-eL", CLIENT], cwd=ROOT, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=cenv, start_new_session=True)
        PROCS.append(p)
        joined = wait_for(lambda: re.search(r"\[input_script\] state t=\d+ ", read(log)), 120, 0.5)
        check("brain: the guest quick-matches into the node's lobby", bool(joined), read(log)[-400:])
        if not joined:
            return
        node.stdin.write(b"round end\n")
        node.stdin.flush()
        try:
            rc = p.wait(timeout=120)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            rc = "timeout"
        text = read(log)
        check("brain: client_bin exits 0", rc == 0, "rc=%s, log %s" % (rc, log))
        res, s_res = probe(text, "vis", "results"), state_after(text, 14000)
        check("brain: the results show before Return", res.get("endUI") == "1" and s_res.get("ended") == "1", "%r %r" % (res, s_res))
        seek = re.search(r"\[mm\] quick match: seeking a lobby other than (\S+)", text)
        check("brain: Return starts a Quick Match seek that excludes this lobby", seek is not None and seek.group(1) != "(none)",
              [l for l in text.splitlines() if l.startswith("[mm]")][-6:])
        check("brain: Return does not disconnect", "[mm] disconnected by the user" not in text,
              [l for l in text.splitlines() if "disconnect" in l.lower()][:3])
        disc = re.search(r"\[input_script\] grep t=\d+ @disc needle=Disconnected n=(\d+) hits=(\d+)", text)
        check("brain: no Disconnected on the page", disc is not None and disc.group(1) != "0" and disc.group(2) == "0",
              disc.group(0) if disc else "no grep line")
    finally:
        for proc in PROCS:
            if proc.poll() is None:
                try:
                    proc.terminate()
                    proc.wait(5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
        subprocess.run(scratch + ["drop", "sqlite", name], capture_output=True)


def main():
    offline()
    brain()
    if FAILS:
        print("end_return_e2e: %d failure(s), logs in %s" % (len(FAILS), WORK))
        return 1
    shutil.rmtree(WORK, ignore_errors=True)
    if SKIPPED:
        print("end_return_e2e: offline passed, %s skipped" % ", ".join(SKIPPED))
        return 77
    print("end_return_e2e: passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

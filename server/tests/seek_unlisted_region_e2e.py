#!/usr/bin/env python3
"""A saved Default Region the matchmaker does not list is an error, not a silent fallback, end to end
(client/src/ui/servers_ui.c on_ping_list, seek_choice.c).

  python3 -I server/tests/seek_unlisted_region_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch sqlite database with one brain-linked server_bin node in region "local": the
brain's /mm/ping-list lists "local" only. Two headless clients, each started twice on one config
dir (the first run sets Settings > Default Region, the second boots and seeks):

  1. Default Region "VA" (Ashburn, not listed): the client logs that the region is not listed and
     shows the error; it sends no seek to the brain (no join)
  2. Default Region "local" (listed): the startup seek joins the node

Exit 77 when sqlite scratch databases or the binaries are unavailable.
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

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
NAME = "kn_test_unlisted_%d" % os.getpid()
SECRET = "kn-unlisted-e2e-%d" % os.getpid()
WORK = tempfile.mkdtemp(prefix="kn-unlisted-e2e-")
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 160)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=15.0):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return None


def lobbies(port):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/mm/game-list" % port, timeout=5) as r:
            return json.loads(r.read()).get("games", [])
    except Exception:  # noqa: BLE001 - the brain may not be up yet
        return []


def start_brain(env, port_log):
    p = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                          "--brain.join_ttl_s=5", "--brain.id=unlisted-e2e-%d" % os.getpid()],
                         cwd=ROOT, env=env, stdout=open(port_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    procs.append(p)
    m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(port_log)))
    return int(m.group(1)) if m else None


def start_node(brain_port):
    cfg = os.path.join(WORK, "node.json")
    port = kn_proto.free_ports(1)[0]
    with open(cfg, "w") as f:
        json.dump({"gameserver": {"node_id": "LOC1", "name": "unlisted e2e", "region": "local", "listen_port": port,
                                  "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
                                  "brain_secret": SECRET, "modes": ["ffa"], "maps": [0], "max_players": 6,
                                  "idle_kick_seconds": 0, "idle_menu_kick_seconds": 0}}, f)
    log = os.path.join(WORK, "node.log")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", cfg, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    wait_for(lambda: "welcomed" in read_log(log), 20)
    return wait_for(lambda: next((g for g in lobbies(brain_port) if g[0].startswith("LOC1:")), None), 15)


def run_client(name, cfg, script, brain_port):
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_NO_BROWSER": "1",
                "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, name + "-state"),
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "640x360",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:9", "KRUNKNATIVE_KRUNKER_URL": "https://krunker.io"})
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    try:
        p.wait(60)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGKILL)
        p.wait()
    return read_log(log)


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("seek_unlisted_region_e2e: no client_bin: skipped")
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", "sqlite", NAME], capture_output=True)
    if r.returncode:
        print("seek_unlisted_region_e2e: sqlite unavailable: skipped")
        return r.returncode
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain_port = start_brain(env, os.path.join(WORK, "brain.log"))
        check("kn_brain starts", brain_port is not None)
        if not brain_port:
            return 1
        listed = start_node(brain_port)
        check("the node in region local is listed", listed is not None)
        if not listed:
            return 1

        # 1. Ashburn (VA) is not listed: an error, no seek
        cfg = os.path.join(WORK, "cfg-va")
        run_client("set-va", cfg, '0 call setSetting "defaultRegion" "VA"\n1500 quit\n', brain_port)
        log = run_client("boot-va", cfg, "0 boot\n6000 quit\n", brain_port)
        check("unlisted default region VA is reported, not sought in another region",
              "[mm] default region VA is not in the matchmaker's list: no seek" in log, [l for l in log.splitlines() if "[mm]" in l][:4])
        check("no join with an unlisted default region", "[mm] joining " not in log)

        # 2. local is listed: the startup seek joins the node
        cfg2 = os.path.join(WORK, "cfg-local")
        run_client("set-local", cfg2, '0 call setSetting "defaultRegion" "local"\n1500 quit\n', brain_port)
        log2 = run_client("boot-local", cfg2, "0 boot\n8000 quit\n", brain_port)
        check("a listed default region is sought and joined", "[mm] joining LOC1" in log2,
              [l for l in log2.splitlines() if "[mm]" in l][:4])
        return 0 if all(results) else 1
    finally:
        for p in procs:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except OSError:
                pass
        shutil.rmtree(WORK, ignore_errors=True)
        subprocess.run(scratch + ["drop", "sqlite", NAME], capture_output=True)


if __name__ == "__main__":
    rc = main()
    if rc == 77:
        sys.exit(77)
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(rc)

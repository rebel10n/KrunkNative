#!/usr/bin/env python3
"""A seek finds its region's node with brain.matchmaking.cross_region OFF (services/brain/registry.c region_eq):
the official client asks with the v10 id the brain listed (us-ca-sv), the native client with the id its list gave
it, or with a KrunkNative code a saved setting still holds (SV). Each side, each region, the real brain:

  python3 -I server/tests/seek_cross_region_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch sqlite database with cross_region=false and four brain-linked server_bin nodes, one per
KrunkNative edge: SV1 (SV, Santa Clara), LA1 (LA), CH1 (CH), VA1 (VA). Their ids (doc/v10/regions.md):
  us-ca-sv holds SV and LA, us-nj holds CH and VA; SV / LA / CH / VA are the edge codes.
Checks:
  - each region's seek answers with a node of that region: SV -> SV1, LA -> LA1, CH -> CH1, VA -> VA1,
    us-ca-sv -> SV1 or LA1, us-nj -> CH1 or VA1; from the official side (v10 id), the native side (v10 id)
    and the native side's saved code (KN code)
  - control: a region no node holds (de-fra) finds nothing: cross_region is really off

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
import urllib.error
import urllib.parse
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
NAME = "kn_test_xregion_%d" % os.getpid()
SECRET = "kn-xregion-e2e-%d" % os.getpid()
WORK = tempfile.mkdtemp(prefix="kn-xregion-e2e-")
results = []
procs = []

# (node id, KN edge code, the v10 id the brain lists it under)
NODES = [("SV1", "SV", "us-ca-sv"), ("LA1", "LA", "us-ca-sv"), ("CH1", "CH", "us-nj"), ("VA1", "VA", "us-nj")]


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail != "" and (not ok or len(str(detail)) <= 200)
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


def get_json(port, path):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=5) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None
    except Exception:  # noqa: BLE001 - the brain may not be up yet
        return 0, None


def lobbies(port):
    status, body = get_json(port, "/mm/game-list")
    return body.get("games", []) if status == 200 and isinstance(body, dict) else []


def start_brain(env, log):
    p = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local",
                          "--brain.join_ttl_s=5", "--brain.matchmaking.cross_region=false",
                          "--brain.id=xregion-e2e-%d" % os.getpid()],
                         cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    procs.append(p)
    m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(log)))
    return int(m.group(1)) if m else None


def start_node(brain_port, node_id, region):
    cfg = os.path.join(WORK, node_id + ".json")
    port = kn_proto.free_ports(1)[0]
    with open(cfg, "w") as f:
        json.dump({"gameserver": {"node_id": node_id, "name": "xregion e2e " + node_id, "region": region, "listen_port": port,
                                  "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
                                  "brain_secret": SECRET, "modes": ["ffa"], "maps": [0], "max_players": 6,
                                  "idle_kick_seconds": 0, "idle_menu_kick_seconds": 0}}, f)
    log = os.path.join(WORK, node_id + ".log")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", cfg, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)
    wait_for(lambda: "welcomed" in read_log(log), 20)
    return wait_for(lambda: next((g for g in lobbies(brain_port) if g[0].startswith(node_id + ":")), None), 15)


def seek(brain_port, region):
    """The seek a client makes: GET /mm/seek-game?region=...; (status, the node id of the lobby it answers)."""
    status, body = get_json(brain_port, "/mm/seek-game?" + urllib.parse.urlencode({"region": region, "autoChangeGame": "false"}))
    node = body.get("gameId", "").split(":")[0] if status == 200 and isinstance(body, dict) else None
    return status, node if node else (body.get("error") if isinstance(body, dict) else None)


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK) and not os.access(f"{BUILD}/services/brain/kn_brain", os.X_OK):
        print("seek_cross_region_e2e: no binaries: skipped")
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", "sqlite", NAME], capture_output=True)
    if r.returncode:
        print("seek_cross_region_e2e: sqlite unavailable: skipped")
        return r.returncode
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in json.loads(r.stdout).items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain_port = start_brain(env, os.path.join(WORK, "brain.log"))
        check("kn_brain starts with cross_region off", brain_port is not None)
        if not brain_port:
            return 1
        for node_id, code, _ in NODES:
            listed = start_node(brain_port, node_id, code)
            check(f"node {node_id} (region {code}) is listed", listed is not None)
        if not all(any(g[0].startswith(n + ":") for g in lobbies(brain_port)) for n, _, _ in NODES):
            return 1

        # (side, the region the seek names, the nodes that may answer). The official and the native client name
        # a region by its v10 id (what /mm/ping-list gave them); a Default Region an older build saved holds
        # the KrunkNative code of the edge.
        cases = [
            ("official", "us-ca-sv", {"SV1", "LA1"}), ("official", "us-nj", {"CH1", "VA1"}),
            ("native", "us-ca-sv", {"SV1", "LA1"}), ("native", "us-nj", {"CH1", "VA1"}),
            ("native, saved code", "SV", {"SV1"}), ("native, saved code", "LA", {"LA1"}),
            ("native, saved code", "CH", {"CH1"}), ("native, saved code", "VA", {"VA1"}),
        ]
        for side, region, nodes in cases:
            status, node = seek(brain_port, region)
            check(f"{side}: a seek in {region} finds its node ({'/'.join(sorted(nodes))})", status == 200 and node in nodes, (status, node))
        # the control: no node holds de-fra, and cross_region is off: nothing is found
        status, node = seek(brain_port, "de-fra")
        check("control: a region no node holds finds nothing (cross_region is off)", status != 200 or node is None, (status, node))
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

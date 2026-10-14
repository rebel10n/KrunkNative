#!/usr/bin/env python3
"""Bot navigation on a dedicated server (shared/src/bot_nav.c + bot_path.c, doc/console.md "Bots").

  python3 -I server/tests/bot_nav_e2e.py BUILD_DIR [seconds]   (run from the repository root)

A standalone server_bin (rcon only, no coordinator) runs 8 NextBots that roam and fight on
Kanji, Sandstorm, Industry and v7's SkyTemple (skytemple_v2) in turn (rcon `map`); a second one
with --flavor strike runs them on FRVR Strike's SkyTemple (assets/maps/strike/skytemple.json, a
floating island: main lost 227 bots off its ledge in 30 s). On every map, after `seconds`
(default 60) the `bot_nav` counters must show the bots reaching their randomly chosen walkable
goals without wall-stuck events: goals reached >= 1 per bot-minute (they fight too), stuck recoveries <= 3,
replans below a storm (<= 4 per bot-second), no unreachable goals on the closed maps (the
islands have pits the bots may chase a target into: a few), at most one bot falling out of the
map; the route line lists every bot ("id:dead" while it respawns). Prints PASS/FAIL lines; exit code 0 only
when everything passed.
"""
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
BOTS = 8
# (server flavor, maps in turn): base plays skytemple_v2 as "SkyTemple", strike FRVR's own file
RUNS = [("base", ["Kanji", "Sandstorm", "Industry", "SkyTemple"]), ("strike", ["SkyTemple"])]
ISLANDS = {"SkyTemple"}
WORK = tempfile.mkdtemp(prefix="kn-bot-nav-e2e-")
results = []
procs = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def query(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", port))
        data, _ = s.recvfrom(2048)
    return json.loads(data[11:])


RCON_PORT = 0


def rcon(*cmd):
    out = subprocess.run([f"{BUILD}/server_bin", "--rcon", f"127.0.0.1:{RCON_PORT}", "--rcon-password", "rc-pw", *cmd],
                         capture_output=True, text=True, timeout=10)
    return out.returncode, out.stdout + out.stderr


def wait_for(fn, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.2)
    return None


def nav_stats():
    """The bot_nav counters: {stuck, replans, forced, goals, unreachable, nodes, routes}."""
    rc, out = rcon("bot_nav")
    m = re.search(r"Bot nav: stuck (\d+), replans (\d+) \(forced (\d+)\), goals (\d+), unreachable (\d+), falls (\d+); grid (\d+) nodes", out)
    if rc != 0 or not m:
        return None
    routes = re.search(r"Routes \(id:state:waypoint/count\): (.*)", out)
    return {"stuck": int(m[1]), "replans": int(m[2]), "forced": int(m[3]), "goals": int(m[4]), "unreachable": int(m[5]),
            "falls": int(m[6]), "nodes": int(m[7]), "routes": routes[1].split(", ") if routes else [], "raw": out.strip()}


def run_server(flavor, maps):
    global RCON_PORT
    RCON_PORT, port = kn_proto.free_ports(2)  # ephemeral: concurrent runs must not share servers
    cfg = os.path.join(WORK, f"{flavor}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Bot nav"\nport {port}\nregion local\nmaxplayers 10\nmapcycle {maps[0]}\nmode ffa\n'
                f"round_seconds 3000\nrcon_password rc-pw\nrcon_port {RCON_PORT}\n")
    log = open(os.path.join(WORK, f"{flavor}.log"), "w")
    args = [f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")]
    if flavor != "base":
        args += ["--flavor", flavor]
    p = subprocess.Popen(args, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    info = wait_for(lambda: query(port))
    check(f"{flavor}: server up", bool(info), str(info))

    rc, out = rcon("bot_add", str(BOTS), "difficulty=normal")
    check(f"{flavor}: rcon bot_add {BOTS}", rc == 0 and f"Spawned {BOTS} nextbot bots." in out, out.strip()[-200:])
    before = wait_for(nav_stats, 10)
    check(f"{flavor}: rcon bot_nav replies", bool(before), str(before and before["raw"])[-300:])
    if not before:
        return

    for i, name in enumerate(maps):
        if i:
            rc, out = rcon("map", name)
            check(f"rcon map {name}", rc == 0, out.strip()[-200:])
            ok = wait_for(lambda: (nav_stats() or {}).get("nodes", 0) > 0 and (rcon("map")[1].strip().endswith(name) or name in rcon("map")[1]), 30)
            check(f"{name} loaded with a grid", bool(ok), rcon("map")[1].strip()[-100:])
            before = nav_stats()
        time.sleep(SECONDS)
        after = nav_stats()
        label = f"{flavor} {name}"
        check(f"{label}: bot_nav after {SECONDS:.0f} s", bool(after), str(after and after["raw"])[-300:])
        if not after:
            continue
        d = {k: after[k] - before[k] for k in ("stuck", "replans", "forced", "goals", "unreachable", "falls")}
        minutes = SECONDS / 60.0
        print(f"      {label}: grid {after['nodes']} nodes; {BOTS} bots x {SECONDS:.0f} s: stuck {d['stuck']}, replans {d['replans']} "
              f"(forced {d['forced']}), goals reached {d['goals']}, unreachable {d['unreachable']}, falls {d['falls']}", flush=True)
        check(f"{label}: the bots reach their goals ({d['goals']})", d["goals"] >= BOTS * minutes)
        check(f"{label}: no wall-stuck events ({d['stuck']})", d["stuck"] <= 3)
        check(f"{label}: no replan storm ({d['replans']})", d["replans"] <= 4 * BOTS * SECONDS)
        if name in ISLANDS:
            check(f"{label}: few unreachable goals ({d['unreachable']})", d["unreachable"] <= 2 * BOTS * minutes)
        else:
            check(f"{label}: no unreachable goals ({d['unreachable']})", d["unreachable"] == 0)
        check(f"{label}: no bot falls out of the map ({d['falls']})", d["falls"] <= 1)
        check(f"{label}: a route entry per bot ({len(after['routes'])})", len(after["routes"]) == BOTS, after["raw"][-200:])

    rc, out = rcon("bot_kick", "all")
    check(f"{flavor}: rcon bot_kick all", rc == 0 and "Removing" in out, out.strip()[-100:])
    p.kill()
    p.wait(timeout=5)


def main():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    for flavor, maps in RUNS:
        run_server(flavor, maps)


if __name__ == "__main__":
    try:
        main()
    finally:
        for p in procs:
            p.kill()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        shutil.rmtree(WORK, ignore_errors=True)
    failed = results.count(False)
    print(f"{len(results) - failed}/{len(results)} passed")
    sys.exit(1 if failed or not results else 0)

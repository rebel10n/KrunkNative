#!/usr/bin/env python3
"""Krunker v10.0.2 is the only version (doc/v7/decisions.md, shared/include/ruleset.h): server_bin
and client_bin play it with no choice.

  python3 -I server/tests/ruleset_e2e.py BUILD_DIR      (run from the repository root)

No database or coordinator: standalone server_bin processes from generated server.cfg files.
  1. a default server plays v10: the log says so, INIT is 18 bytes with ruleset 3 (the wire id the
     version toggle gave v10.0.2), the server-info query answers "ruleset":"v10"; nothing a client
     sends is refused for its version: a HELLO without the ruleset mask (net-bridge, old builds) and
     one with the v7 bit only both spawn.
  2. the version toggle's knobs are gone: server.cfg `ruleset v7` is ignored with an error line and
     the server still plays v10; `--ruleset` is not an option (exit 2).
  3. client_bin (headless) with a stale knRuleset "v7" saved: it logs Krunker v10.0.2, removes the
     entry, has no `ruleset` console command, plays offline, and joins the server and plays there.
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
PORTS = kn_proto.free_ports(3)
CLIENT_TIMEOUT = 90
WORK = tempfile.mkdtemp(prefix="kn-ruleset-e2e-", dir=os.path.expanduser("~/.cache"))
INIT, ERROR, SPAWN = 2, 30, 4
MASK_V7 = 1 << 1
results = []
procs = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def start_server(i, extra, args=()):
    cfg = os.path.join(WORK, f"s{i}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Ruleset #{i}"\nport {PORTS[i]}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\nmode ffa\n{extra}')
    log = os.path.join(WORK, f"s{i}.log")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json"), *args],
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    return p, log


def wait_port(port, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 0.5).close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def query(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", port))
        data, _ = s.recvfrom(2048)
    start = data.find(b"{")
    return json.loads(data[start:].split(b"\0", 1)[0].decode()) if start >= 0 else {}


def connect(port, hello):
    c = kn_proto.Conn("127.0.0.1", port)
    c.send(kn_proto.HELLO, bytes(hello))
    c.join(name="rs")
    init = c.read(until=INIT, timeout=5)
    return c, init


def spawned(c):
    c.ent(class_index=0, lag_comp=1.0)
    end = time.time() + 5
    while time.time() < end:
        p = c.read(until=SPAWN, timeout=1)
        if p is not None and kn_proto.spawn_info(p)["is_you"]:
            return True
    return False


def read_log(path):
    time.sleep(0.3)
    with open(path, errors="replace") as f:
        return f.read()


def run_client(name, args, script):
    """client_bin headless with an input script, a private config and a stale knRuleset "v7"."""
    cfg = os.path.join(WORK, f"{name}-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    with open(os.path.join(cfg, "krunknative", "knRuleset.json"), "w") as f:
        f.write("v7")
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write(script)
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_UI_DIR")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script_path, "KRUNKNATIVE_WINDOW_SIZE": "640x360"})
    log = os.path.join(WORK, f"{name}.log")
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", *args], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env)
        procs.append(p)
        try:
            p.wait(timeout=CLIENT_TIMEOUT)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            check(f"{name}: client_bin finished its script", False, log)
    stale = os.path.exists(os.path.join(cfg, "krunknative", "knRuleset.json"))
    with open(log, errors="replace") as f:
        return f.read(), stale


def main():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    # 1. the default (and only) ruleset
    p, log = start_server(0, "")
    check("server up", wait_port(PORTS[0]))
    c, init = connect(PORTS[0], [1, 3, 1])  # no ruleset mask: net-bridge, builds before the toggle
    check("INIT: 18 bytes, ruleset 3 (v10.0.2)", init is not None and len(init) == 18 and init[17] == 3, repr(init and init[:18]))
    check("a HELLO without the mask plays", spawned(c))
    c.close()
    c, init = connect(PORTS[0], [1, 4, 1, MASK_V7])  # a version-toggle build set to v7.1.4
    check("a HELLO announcing v7.1.4 only is not refused", spawned(c) and not any(t == ERROR for t, _ in c.packets))
    c.close()
    info = query(PORTS[0])
    check("server-info ruleset v10", info.get("ruleset") == "v10", str(info.get("ruleset")))
    check("the log says Krunker v10.0.2", "Plays Krunker v10.0.2" in read_log(log))

    # 2. no choice: server.cfg `ruleset` is ignored, --ruleset is no option
    p1, log1 = start_server(1, "ruleset v7\n")
    check("server.cfg ruleset v7: up", wait_port(PORTS[1]))
    c, init = connect(PORTS[1], [1, 4, 1, MASK_V7])
    check("server.cfg ruleset v7: still INIT ruleset 3", init is not None and len(init) == 18 and init[17] == 3, repr(init and init[:18]))
    c.close()
    text = read_log(log1)
    check("server.cfg ruleset v7: ignored with an error line", "gameserver.ruleset is ignored" in text and "Plays Krunker v10.0.2" in text)
    p2, log2 = start_server(2, "", ("--ruleset", "v10"))
    try:
        rc = p2.wait(timeout=10)
    except subprocess.TimeoutExpired:
        rc = None
    check("--ruleset is not an option (exit 2)", rc == 2 and "--ruleset" not in read_log(log2).split("Usage", 1)[-1], f"rc {rc}")

    # 3. the client: no picker, no console command, plays offline and online
    text, stale = run_client("offline", ["--offline", "assets/maps/official/10_kanji.json"],
                             "0 play\n1500 state\n1600 con help ruleset\n1650 con help official_sync\n2500 quit\n")
    check("client: logs Krunker v10.0.2", "[client] Krunker v10.0.2" in text)
    check("client: the stale knRuleset entry is removed", not stale)
    check("client: no `ruleset` console command", "unknown command: ruleset" in text)
    check("client: no `official_sync` console command (removed)", "unknown command: official_sync" in text)
    check("client: plays offline", "weapon=Assault Rifle menu=0" in text)
    check("client: no version notices", "[ruleset]" not in text and "Game version" not in text)
    text, _ = run_client("join", ["--connect", f"127.0.0.1:{PORTS[0]}"], "0 play\n2500 state\n3500 quit\n")
    check("client: joins the server and plays", "connected to server" in text and "weapon=Assault Rifle menu=0" in text)


try:
    main()
finally:
    for p in procs:
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
if results and all(results):
    shutil.rmtree(WORK, ignore_errors=True)
    print(f"{sum(results)}/{len(results)} passed")
else:
    print(f"{sum(results)}/{len(results)} passed  (logs: {WORK})")
sys.exit(0 if results and all(results) else 1)

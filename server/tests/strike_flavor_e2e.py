#!/usr/bin/env python3
"""Krunker Strike is a product FLAVOR of Krunker Box Office (doc/v10/krunker-strike.md,
shared/include/flavor.h): the same server core and client, a distinct brand and a bounded set of
gameplay deltas, chosen at startup and advertised by the server.

  python3 -I server/tests/strike_flavor_e2e.py BUILD_DIR      (run from the repository root)

No database or coordinator: standalone server_bin processes from generated server.cfg files.
  1. a default (base) server: no "Flavor:" log line, the server-info query has no `flavor` field, a
     client joins and spawns (base wire is unchanged); --version prints no flavor line.
  2. a Strike server (--flavor strike): the log says Flavor: Krunker Strike, the server-info query
     answers "flavor":"strike", and a client still joins and spawns (direct-connect works).
  3. server.cfg `flavor strike` does the same; an unknown `flavor` is ignored with an error line and
     the server stays base; `--flavor bogus` is rejected (exit 2).
  4. branding: server_bin / client_bin --version print "Krunker Strike" under the flavor, and
     KN_FLAVOR=strike brands client_bin --version too (no GL / display needed).
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
PORTS = kn_proto.free_ports(2)
WORK = tempfile.mkdtemp(prefix="kn-strike-e2e-", dir=os.path.expanduser("~/.cache"))
INIT, SPAWN = 2, 4
results = []
procs = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def start_server(i, extra, args=()):
    cfg = os.path.join(WORK, f"s{i}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Strike #{i}"\nport {PORTS[i]}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\nmode ffa\n{extra}')
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


def spawned(port):
    """A direct connect: HELLO, join, read INIT, send ENTER_GAME, see our own spawn."""
    c = kn_proto.Conn("127.0.0.1", port)
    c.send(kn_proto.HELLO, bytes([1, 3, 1]))
    c.join(name="st")
    init = c.read(until=INIT, timeout=5)
    ok = init is not None
    if ok:
        c.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 5
        ok = False
        while time.time() < end:
            p = c.read(until=SPAWN, timeout=1)
            if p is not None and kn_proto.spawn_info(p)["is_you"]:
                ok = True
                break
    c.close()
    return ok


def read_log(path):
    time.sleep(0.3)
    with open(path, errors="replace") as f:
        return f.read()


def run_version(binary, args, env_extra=None):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    if env_extra:
        env.update(env_extra)
    r = subprocess.run([f"{BUILD}/{binary}", *args, "--version"], capture_output=True, text=True, env=env, timeout=30)
    return r.returncode, r.stdout + r.stderr


def main():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")

    # 1. the default (base) server: unchanged
    p, log = start_server(0, "")
    check("base server up", wait_port(PORTS[0]))
    info = query(PORTS[0])
    check("base server-info has no flavor field", "flavor" not in info, str(info.get("flavor")))
    check("base server plays (direct connect spawns)", spawned(PORTS[0]))
    text = read_log(log)
    check("base server: no Flavor: log line", "Flavor:" not in text)

    # 2. a Strike server via --flavor
    p1, log1 = start_server(1, "", ("--flavor", "strike"))
    check("strike server up", wait_port(PORTS[1]))
    info = query(PORTS[1])
    check("strike server-info flavor=strike", info.get("flavor") == "strike", str(info.get("flavor")))
    check("strike server plays (direct connect spawns)", spawned(PORTS[1]))
    check("strike server: Flavor: Krunker Strike in the log", "Flavor: Krunker Strike (strike)" in read_log(log1))
    p1.terminate(); p1.wait(timeout=5)

    # 3. server.cfg flavor, and the bad values
    p2, log2 = start_server(1, "flavor strike\n")
    check("server.cfg flavor strike: up", wait_port(PORTS[1]))
    check("server.cfg flavor strike: server-info flavor=strike", query(PORTS[1]).get("flavor") == "strike")
    p2.terminate(); p2.wait(timeout=5)
    p3, log3 = start_server(1, "flavor nope\n")
    check("server.cfg flavor nope: up (ignored)", wait_port(PORTS[1]))
    check("server.cfg flavor nope: stays base", "flavor" not in query(PORTS[1]))
    check("server.cfg flavor nope: error line", "gameserver.flavor 'nope' is unknown" in read_log(log3))
    p3.terminate(); p3.wait(timeout=5)
    rc, out = run_version("server_bin", ["--flavor", "bogus"])
    check("--flavor bogus is rejected (exit 2)", rc == 2, f"rc {rc}")

    # 4. branding on --version (no GL / display needed)
    rc, out = run_version("server_bin", [])
    check("server base --version: no flavor line", rc == 0 and "flavor " not in out)
    rc, out = run_version("server_bin", ["--flavor", "strike"])
    check("server --flavor strike --version: flavor strike (Krunker Strike)", rc == 0 and "flavor strike (Krunker Strike)" in out)
    rc, out = run_version("client_bin", [])
    check("client base --version: Krunker Box Office", rc == 0 and "Krunker Box Office (KrunkNative client)" in out)
    rc, out = run_version("client_bin", ["--flavor", "strike"])
    check("client --flavor strike --version: Krunker Strike", rc == 0 and "Krunker Strike (KrunkNative client)" in out)
    rc, out = run_version("client_bin", [], {"KN_FLAVOR": "strike"})
    check("client KN_FLAVOR=strike --version: Krunker Strike", rc == 0 and "Krunker Strike (KrunkNative client)" in out)


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

#!/usr/bin/env python3
"""Build Tool placement end to end: placed builds are drawn and collide, the local player's build
shows at once (client prediction, shared/src/build.c build_predict) and is replaced by the
server's, Object Manager undo removes what it spawned, and many builds keep their meshes.

  python3 -I client/tests/build_place_e2e.py BUILD_DIR      (run from the repository root)

  1. a server_bin on loopback (port from kn_proto.free_ports), the client with
     KRUNKNATIVE_NET_DELAY_MS=150 (a 300 ms round trip, so the prediction is seen before the
     server's answer): the Build Tool places a Build Block. `placewatch` sees it drawn within 2
     frames of the click; at once it is the client's prediction (predicted=1), after the round
     trip the server's build in its place (predicted=0, one build, one mesh, same spot).
  2. offline Lostworld (the client's own sandbox server, no added delay: its debug latency stalls
     the in-process join): the build is drawn and is the server's (one build, one mesh). Walking
     into it stops at its face (it collides). An Object Manager object spawned and then undone (U)
     leaves the scene; the build stays. `build_fill 300` (the Build Tool's server path,
     doc/console.md): 301 builds, 301 meshes shown (map_post.c's indexed build meshes); the frame
     times are logged, not asserted.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "server", "tests"))
from kn_proto import free_ports  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-build-place-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
ASSETS = os.environ.get("KRUNKNATIVE_ASSETS_DIR", os.path.abspath("assets"))
MAP = os.path.join(ASSETS, "maps", "lostworld_s6.json")
SPOT = "-19 50.5 140"  # Lostworld's first spawn: the floor ahead along +x (yaw -PI/2)
DELAY_MS = "150"
results = []
procs = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def run_client(name, args, script, extra_env=None, timeout=180):
    """client_bin headless with an input script (events sorted by time); its log lines."""
    events = [line for line in script.splitlines() if line.strip()]
    events.sort(key=lambda line: int(line.split()[0]))
    script_path = os.path.join(WORK, f"{name}.script")
    with open(script_path, "w") as f:
        f.write("\n".join(events) + "\n")
    log = os.path.join(WORK, f"{name}.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, f"{name}-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_INPUT_SCRIPT": script_path,
                "KRUNKNATIVE_FIXED_DELTA": "16.667", "KRUNKNATIVE_WINDOW_SIZE": "1280x720"})
    env.update(extra_env or {})
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin"] + args, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        procs.append(p)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    return read(log).splitlines()


BUILDS_RE = re.compile(r"\[input_script\] builds t=\d+ (@\S+) n=(\d+) predicted=(\d+) meshes=(\d+) visible=(\d+) last=(\S+) spawned=(\d+)")
PLACE_RE = re.compile(r"\[input_script\] place t=\d+ (@\S+) frames=(\d+) ms=(\S+) visible=(\d+)")
STATE_RE = re.compile(r"\[input_script\] state t=\d+ .*?pos=(\S+),(\S+),(\S+) ")


def builds(lines):
    out = {}
    for line in lines:
        m = BUILDS_RE.search(line)
        if m:
            last = tuple(map(float, m.group(6).split(","))) if m.group(6) != "-" else None
            out[m.group(1)] = {"n": int(m.group(2)), "predicted": int(m.group(3)), "meshes": int(m.group(4)),
                               "visible": int(m.group(5)), "last": last, "spawned": int(m.group(7))}
    return out


def places(lines):
    return {m.group(1): (int(m.group(2)), float(m.group(3))) for m in map(PLACE_RE.search, lines) if m}


# Spawn, the Build Tool in hand (slot 4), looking down at the floor 17 units ahead along +x; the
# click at t=3000 (placewatch from that frame), `builds` 50 ms later (inside the round trip) and
# 1500 ms later (the server's answer is in)
PLACE = f"""
0 play
300 pose {SPOT} -1.571 -0.5
500 con give build tool
1800 down 4
1850 up 4
2600 pose {SPOT} -1.571 -0.5
2900 builds @before
3000 placewatch @place
3000 mdown left
3060 mup left
3050 builds @early
4500 builds @confirmed
"""


def check_placement(label, lines, predicted):
    b, p = builds(lines), places(lines)
    before, early, confirmed = b.get("@before"), b.get("@early"), b.get("@confirmed")
    check(f"{label}: no build before the click", before and before["n"] == 0 and before["meshes"] == 0, str(before))
    frames = p.get("@place")
    if predicted:
        check(f"{label}: the build is drawn within 2 frames of the click (predicted, not a round trip later)",
              frames and frames[0] <= 2, f"frames, ms: {frames}")
        check(f"{label}: inside the round trip it is the client's prediction, drawn",
              early and early["n"] == 1 and early["predicted"] == 1 and early["visible"] == 1, str(early))
    else:
        check(f"{label}: the build is drawn", frames is not None, f"frames, ms: {frames}")
    check(f"{label}: then it is the server's build (one build, one mesh{', the predicted spot' if predicted else ''})",
          confirmed and confirmed["n"] == 1 and confirmed["predicted"] == 0 and confirmed["meshes"] == 1 and confirmed["visible"] == 1 and
          (not predicted or (early and confirmed["last"] == early["last"])), f"{early} -> {confirmed}")
    return confirmed["last"] if confirmed else None


def stop(p):
    if p.poll() is None:
        try:
            os.killpg(p.pid, signal.SIGTERM)
        except OSError:
            pass
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()


try:
    # ------------------------------------------------------------------ 1. server_bin on loopback
    port = free_ports(1)[0]
    cfg = os.path.join(WORK, "server.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Build place e2e"\nport {port}\nregion local\nmaxplayers 8\nmapcycle Lostworld\nmode ffa\nround_seconds 900\n')
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    slog = os.path.join(WORK, "server.log")
    senv = dict(os.environ, KRUNKNATIVE_ASSETS_DIR=ASSETS)
    with open(slog, "w") as out:
        server = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                                  stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=senv, start_new_session=True)
    procs.append(server)
    end = time.time() + 20
    while time.time() < end and "Listening on port" not in read(slog):
        time.sleep(0.2)
    check("server_bin listening", "Listening on port" in read(slog), slog)
    online = PLACE.replace(" pose ", " tp ") + "\n6000 quit\n"
    lines = run_client("online", ["--connect", f"127.0.0.1:{port}"], online, {"KRUNKNATIVE_NET_DELAY_MS": DELAY_MS})
    check_placement("online", lines, True)
    stop(server)

    # ------------------------------------------------------------------ 2. offline
    script = PLACE + f"""
5000 pose {SPOT} -1.571 0
5200 down W
6700 up W
7000 state
7200 pose {SPOT} 1.571 -0.4
7500 om object 10
8500 builds @spawned
8600 down U
8650 up U
9600 builds @undone
9700 pose {SPOT} -1.571 -0.15
9800 con build_fill 300
12500 builds @filled
12600 ft @300builds start
14600 ft @300builds stop
14700 quit
"""
    lines = run_client("offline", ["--offline", MAP], script)
    at = check_placement("offline", lines, False)
    states = [m for m in map(STATE_RE.search, lines) if m]
    pos = tuple(map(float, states[-1].groups())) if states else None
    # the player's feet start at x -19; the block's face is at its x - 5 (the player's own radius
    # keeps it a little short of that). Without the block the same walk ends near x 20.
    check("offline: walking into the build stops at its face (it collides)",
          at and pos and pos[0] < at[0] - 5.0 + 0.05 and pos[0] > -19.0 + 3.0, f"block at {at}, player at {pos}")
    b = builds(lines)
    spawned, undone = b.get("@spawned"), b.get("@undone")
    check("offline: an Object Manager object spawned is drawn", spawned and spawned["spawned"] >= 1, str(spawned))
    check("offline: Object Manager undo removes it from the scene; the build stays",
          spawned and undone and undone["spawned"] == spawned["spawned"] - 1 and undone["n"] == 1 and undone["visible"] == 1,
          f"{spawned} -> {undone}")
    filled = b.get("@filled")
    check("offline: build_fill 300: 301 builds, each drawn once",
          filled and filled["n"] == 301 and filled["meshes"] == 301 and filled["visible"] == 301 and filled["predicted"] == 0, str(filled))
    for line in lines:
        if "] ft " in line:
            print("      " + line.split("] ", 1)[1], flush=True)
finally:
    for proc in procs:
        stop(proc)

failed = results.count(False)
print(f"build_place_e2e: {len(results) - failed}/{len(results)} passed", flush=True)
if not failed:
    shutil.rmtree(WORK, ignore_errors=True)
else:
    print(f"logs kept in {WORK}", flush=True)
sys.exit(1 if failed else 0)

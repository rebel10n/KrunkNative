#!/usr/bin/env python3
"""Bots play every Krunker v7.1.4 game mode on a dedicated server (doc/v7/map-logic.md §4.1, WP-D).

  python3 -I server/tests/modes_bots_e2e.py BUILD_DIR [MODE...]      (run from the repository root)

For each mode id 0..36 (37 / 38 are reserved for ranked: no mode): one standalone server_bin on Sandstorm (the v7.1.4
sandstorm_v3: objectives, team + neutral flags, Domination zones) with `bot_quota 4`, a guest that
announces protocol 1.3 (NET_PROTOCOL_MINOR_V7) and watches for a while. Checks per mode: the guest
spawns in that mode, the server keeps ticking (STATE packets) and survives, and the mode's own
packets show up where the mode makes them certain (objective / dom / spawnable / rounds blocks).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ONLY = [int(m) for m in sys.argv[2:]]
WORK = tempfile.mkdtemp(prefix="kn-modes-e2e-")
HELLO, INIT, SPAWN, STATE, KILL, MODE_STATE = 1, 2, 4, 6, 10, 40
OBJECTIVE_V7, DOM_UPDATE, SPAWNABLE_V7, MODE_SYNC_V7 = 140, 142, 148, 149
WATCH = float(os.environ.get("KN_MODES_WATCH", "8"))

MODES = ["ffa", "tdm", "point", "ctf", "bhop", "hide", "infect", "race", "lms", "simon", "gun", "prop", "boss", "clas",
         "depo", "stalk", "king", "oinc", "trade", "kc", "def", "shrp", "trai", "raid", "blitz", "dom", "sdm", "krank",
         "tdf", "depoffa", "car", "aon", "md", "chs", "bhffa", "zom", "imp"]
# what a joiner gets for sure: MODE_SYNC_V7 always (objective modes: an objective uid in it),
# Domination its zones ("uct" sync.doms)
OBJECTIVE_MODES = {2, 14, 16, 29}
EXPECT = {25: DOM_UPDATE}
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def query(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", port))
        import json
        data, _ = s.recvfrom(2048)
    return json.loads(data[11:])


def run_mode(mode_id, alias, port):
    cfg = os.path.join(WORK, f"m{mode_id}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Modes #{mode_id}"\nport {port}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\nmode {alias}\n'
                "round_seconds 40\nbot_quota 4\nbot_difficulty easy\n")
    log_path = os.path.join(WORK, f"m{mode_id}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                            stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    try:
        info = None
        end = time.time() + 15
        while time.time() < end and not info:
            try:
                info = query(port)
            except Exception:  # noqa: BLE001 - polling
                time.sleep(0.2)
        if not info:
            check(f"mode {mode_id} {alias}: server up", False)
            return
        conn = kn_proto.Conn("127.0.0.1", port)
        conn.send(HELLO, bytes([1, 3, 0]))  # protocol 1.3: the v7 mode packets
        conn.join(name="Watcher")
        conn.ent(class_index=0, lag_comp=1.0)
        conn.read(timeout=WATCH)
        types = {}
        for t, _ in conn.packets:
            types[t] = types.get(t, 0) + 1
        inits = [p for t, p in conn.packets if t == INIT]
        mode_index = struct.unpack("<ii", inits[0][:8])[1] if inits and len(inits[0]) >= 8 else -1
        alive = proc.poll() is None
        detail = f"modeId {info.get('modeId')} spawns {types.get(SPAWN, 0)} states {types.get(STATE, 0)} kills {types.get(KILL, 0)}"
        ok = alive and info.get("modeId") == mode_id and types.get(SPAWN, 0) >= 1 and types.get(STATE, 0) >= 10 and types.get(MODE_SYNC_V7, 0) >= 1
        syncs = [p for t, p in conn.packets if t == MODE_SYNC_V7]
        if mode_id in OBJECTIVE_MODES:
            uid = struct.unpack("<i", syncs[-1][57:61])[0] if syncs and len(syncs[-1]) >= 65 else -1
            ok = ok and uid >= 0
            detail += f" objective uid {uid}"
        want = EXPECT.get(mode_id)
        if want is not None:
            ok = ok and types.get(want, 0) >= 1
            detail += f" packet {want}: {types.get(want, 0)}"
        check(f"mode {mode_id:2d} {alias}: bots play", ok, detail)
        conn.close()
        try:
            query(port)
            check(f"mode {mode_id:2d} {alias}: server still answers", proc.poll() is None)
        except Exception:  # noqa: BLE001
            check(f"mode {mode_id:2d} {alias}: server still answers", False)
        del mode_index
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        log.close()
        with open(log_path) as f:
            text = f.read()
        bad = [line for line in text.splitlines() if "AddressSanitizer" in line or "Segmentation" in line or "assert" in line.lower()]
        if bad:
            check(f"mode {mode_id:2d} {alias}: clean log", False, bad[0][:200])


def main():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    ids = ONLY or list(range(len(MODES)))
    for mode_id in ids:
        run_mode(mode_id, MODES[mode_id], kn_proto.free_ports(1)[0])  # ephemeral: concurrent runs


try:
    main()
except Exception as e:  # noqa: BLE001
    check("exception", False, repr(e))
print(f"\n{sum(results)}/{len(results)} passed")
sys.exit(0 if results and all(results) else 1)

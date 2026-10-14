#!/usr/bin/env python3
"""A v10.0.2 match smoke on server_bin (Krunker v10.0.2, the only version; doc/v10/gameplay.md).

  python3 -I server/tests/v10_match_e2e.py BUILD_DIR [MODE...]      (run from the repository root)

No database or coordinator: one standalone server_bin per mode (Sandstorm, `bot_quota 4`), and a
raw guest that announces v10 (HELLO 1.4 + the ruleset mask) and picks
Spray N Pray with the Compressor as its secondary. Per mode:
  - the guest spawns with v10's class table (Spray N Pray 170 HP, v7 180; Gun Game and Raid force
    the Runner);
  - the server keeps ticking with bots fighting under the v10 hit model (KILL packets in FFA:
    every bot hit goes through shared/src/hitboxes.c), and survives the mode's v10 rules
    (Gun Game's new order, the Stalker rework, Raid's 25 min clock);
  - a clean log (no sanitizer / segfault / assert lines).
Ports come from kn_proto.free_ports(). Prints PASS/FAIL lines; exit code 0 only when all passed.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ONLY = sys.argv[2:]
WORK = tempfile.mkdtemp(prefix="kn-v10-match-", dir=os.path.expanduser("~/.cache"))
HELLO, INIT, SPAWN, STATE, KILL = 1, 2, 4, 6, 10
MASK_V7, MASK_V10 = 1 << 1, 1 << 2
SPRAY_N_PRAY, COMPRESSOR = 3, 30
WATCH = float(os.environ.get("KN_V10_WATCH", "12"))
WATCH_FFA = float(os.environ.get("KN_V10_WATCH_FFA", "40"))  # long enough for bot kills (2-4 in 40 s, as under v7)
MODES = {"ffa": 0, "gun": 10, "stalk": 15, "raid": 23}
NO_WEAPONS = {"gun", "raid"}  # noWeap: class 9 (Runner) for everyone
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def query(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", port))
        data, _ = s.recvfrom(2048)
    start = data.find(b"{")
    return json.loads(data[start:].split(b"\0", 1)[0].decode()) if start >= 0 else {}


def spawn_health(payload):
    """NetSpawnPacket: uid i, is_you B, active B, class i, team i, health f, max_health i."""
    import struct
    if len(payload) < 22:
        return None
    uid, is_you, _active, cls, _team, _health, max_health = struct.unpack("<iBBiifi", payload[:22])
    return {"uid": uid, "is_you": is_you, "class": cls, "max_health": max_health}


def run_mode(alias, mode_id, port):
    cfg = os.path.join(WORK, f"{alias}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "v10 {alias}"\nport {port}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\nmode {alias}\n'
                "round_seconds 60\nbot_quota 4\nbot_difficulty hard\n")
    log_path = os.path.join(WORK, f"{alias}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config",
                             os.path.join(WORK, "none.json")], stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    try:
        info = None
        end = time.time() + 15
        while time.time() < end and not info:
            try:
                info = query(port)
            except Exception:  # noqa: BLE001 - polling
                time.sleep(0.2)
        if not info:
            check(f"{alias}: server up", False)
            return
        check(f"{alias}: the server plays v10 in mode {mode_id}", info.get("ruleset") == "v10" and info.get("modeId") == mode_id,
              f"ruleset {info.get('ruleset')} modeId {info.get('modeId')}")
        conn = kn_proto.Conn("127.0.0.1", port)
        conn.send(HELLO, bytes([1, 4, 1, MASK_V7 | MASK_V10]))
        conn.join(name="V10Watcher")
        conn.ent(class_index=SPRAY_N_PRAY, secondary=COMPRESSOR, lag_comp=1.0)
        conn.read(timeout=WATCH_FFA if alias == "ffa" else WATCH)
        types = {}
        for t, _ in conn.packets:
            types[t] = types.get(t, 0) + 1
        mine = [s for s in (spawn_health(p) for t, p in conn.packets if t == SPAWN) if s and s["is_you"]]
        detail = f"spawns {types.get(SPAWN, 0)} states {types.get(STATE, 0)} kills {types.get(KILL, 0)}"
        if alias in NO_WEAPONS:
            # noWeap modes force class 9 (Runner, 120 HP in v7 and v10)
            check(f"{alias}: the guest spawns as the forced Runner", bool(mine) and mine[0]["class"] == 9 and mine[0]["max_health"] == 120,
                  f"{mine[:1]} {detail}")
        else:
            check(f"{alias}: the guest spawns as v10's Spray N Pray (170 HP)", bool(mine) and mine[0]["max_health"] == 170,
                  f"{mine[:1]} {detail}")
        check(f"{alias}: the server keeps ticking", proc.poll() is None and types.get(STATE, 0) >= 20, detail)
        if alias == "ffa":
            check(f"{alias}: bots kill each other under the v10 hit model", types.get(KILL, 0) >= 1, detail)
        conn.close()
        try:
            query(port)
            check(f"{alias}: server still answers", proc.poll() is None)
        except Exception:  # noqa: BLE001
            check(f"{alias}: server still answers", False)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        log.close()
        with open(log_path, errors="replace") as f:
            text = f.read()
        bad = [line for line in text.splitlines() if "AddressSanitizer" in line or "Segmentation" in line or "assert" in line.lower()]
        check(f"{alias}: clean log", not bad and "Plays Krunker v10.0.2" in text, bad[0][:200] if bad else "")


def main():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    for alias, mode_id in MODES.items():
        if ONLY and alias not in ONLY:
            continue
        run_mode(alias, mode_id, kn_proto.free_ports(1)[0])


try:
    main()
except Exception as e:  # noqa: BLE001
    check("exception", False, repr(e))
ok = bool(results) and all(results)
if ok:
    import shutil
    shutil.rmtree(WORK, ignore_errors=True)
print(f"\n{sum(results)}/{len(results)} passed{'' if ok else '  (logs: ' + WORK + ')'}")
sys.exit(0 if ok else 1)

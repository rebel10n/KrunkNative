#!/usr/bin/env python3
"""Krunker v10.0.4 "Matches played alone now end with ENDED instead of DEFEAT" on server_bin.

  python3 -I server/tests/solo_ended_e2e.py BUILD_DIR      (run from the repository root)

No database or coordinator: a standalone server_bin playing Candy Heist (mode 37, map 46), the solo event
mode whose config has noWinner (deob-10.0.4.js:105918), with --round-seconds 6 and no bots. One raw guest
joins, spawns and waits for the round's END. 10.0.4's endGame sets winner = -1 for a noWinner mode
(deob-10.0.4.js:194728), so every player's didWin is false and END_GAME carries it as 0 (:194893); the client
then shows "end" (ENDED) instead of "def" because its mode has noWinner (:223533-223534, vic_title_test).
Checks: the END's result is 0 (not a victory, not a draw), its winner uid is -1, and the server keeps running.
Prints PASS/FAIL lines; exit code 0 only when all passed.
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
WORK = tempfile.mkdtemp(prefix="kn-solo-ended-", dir=os.path.expanduser("~/.cache"))
HELLO, SPAWN, END = 1, 4, 16
MASK_V7, MASK_V10 = 1 << 1, 1 << 2
results = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def query(port):
    import json
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", port))
        data, _ = s.recvfrom(2048)
    start = data.find(b"{")
    return json.loads(data[start:].split(b"\0", 1)[0].decode()) if start >= 0 else {}


def main():
    port = kn_proto.free_ports(1)[0]
    cfg = os.path.join(WORK, "candy.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "solo ended"\nport {port}\nregion local\nmaxplayers 8\nmapcycle "Candy Heist"\nmode candy\n'
                "bot_quota 0\n")
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    log_path = os.path.join(WORK, "server.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json"),
                             "--round-seconds", "6"], stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    try:
        info = None
        deadline = time.time() + 15
        while time.time() < deadline and not info:
            try:
                info = query(port)
            except Exception:  # noqa: BLE001 - polling
                time.sleep(0.2)
        check("server up", bool(info))
        if not info:
            return
        check("the server plays Candy Heist (mode 37)", info.get("modeId") == 37, f"modeId {info.get('modeId')}")
        conn = kn_proto.Conn("127.0.0.1", port)
        conn.send(HELLO, bytes([1, 4, 1, MASK_V7 | MASK_V10]))
        conn.join(name="Solo")
        conn.ent(class_index=9, lag_comp=1.0)
        end = conn.read(until=END, timeout=30)
        spawned = any(t == SPAWN and len(p) >= 6 and p[4] == 1 and p[5] == 1 for t, p in conn.packets)
        check("the guest plays alone", spawned)
        check("the round ends (END)", end is not None and len(end) >= 9)
        if end is not None and len(end) >= 9:
            result, _next_round, winner_uid = struct.unpack("<bii", end[:9])
            check("END result 0: no victory and no draw (didWin false, the client shows ENDED)", result == 0, f"result {result}")
            check("END winner uid -1: no winner (winner = -1, deob-10.0.4.js:194728)", winner_uid == -1, f"winner {winner_uid}")
        conn.close()
        try:
            query(port)
            check("server still answers", proc.poll() is None)
        except Exception:  # noqa: BLE001
            check("server still answers", False)
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
        check("clean log", not bad, bad[0][:200] if bad else "")


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

#!/usr/bin/env python3
"""KrunkScript server scripts on a dedicated server_bin with bots (doc/v7/krunkscript.md WP3).

  python3 -I server/tests/ks_e2e.py BUILD_DIR [PORT]      (run from the repository root)

One standalone server_bin (bot_quota 3) on a map whose scripts.server is the corpus "Basic
Networking" + "Triggers & Scripts" server code plus logging hooks, then:
  1. the script loads, start() and onPlayerSpawn (bots included) run with no runtime error
  2. two protocol-1.3 TCP clients: GAME.log reaches both as SCRIPT_LOG; a 1.0 client gets none
  3. client A: NETWORK.send("check", {age: 20}) -> SCRIPT_MSG "check" {status: "Passed"} to A only
  4. client A: "relay" -> broadcast to both v7 clients (two-client message test)
  5. chat -> onChatMessage; the server half of `scripts` is never streamed (MAP_DATA)
  6. bots playing under onPlayerUpdate / onPlayerDamage / onPlayerDeath: no runtime errors
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import base64
import json
import os
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 22611
WORK = tempfile.mkdtemp(prefix="kn-ks-e2e-")
SCRIPT_MSG, SCRIPT_LOG, SCRIPT_SYNC, SCRIPT_CHAT = 200, 201, 202, 203
MAP_INFO, MAP_REQUEST, MAP_DATA, CHAT_SEND = 25, 26, 27, 8
results = []

SERVER_SCRIPT = """
num spawns = 0;
public action start() { GAME.log("ks start"); }
public action update(num delta) { }
public action onPlayerSpawn(str id) { spawns += 1; GAME.log("spawn", id); obj p = GAME.PLAYERS.findByID(id); p.coins = spawns; p.registerSyncValues("coins"); }
public action onPlayerUpdate(str id, num delta, static obj inputs) { obj p = GAME.PLAYERS.findByID(id); if (!!p && inputs.jump) { p.jumped = true; } }
public action onPlayerDamage(str id, str doerID, num amount) { }
public action onPlayerDeath(str id, str killerID) { GAME.log("death", id, killerID); }
public action onCustomTrigger(str playerID, str customParam) {
    if (customParam == "1") { GAME.log("First Trigger"); }
    if (customParam == "2") { GAME.log("Second Trigger"); }
}
public action onChatMessage(str msg, str playerID) { GAME.log("chat", msg, playerID); }
public action onNetworkMessage(str id, obj data, str playerID) {
    if (id == "check") {
        num age = (num) data.age;
        bool valid = age >= 13;
        GAME.NETWORK.send("check", { status: valid ? "Passed" : "Failed" }, playerID);
    }
    if (id == "relay") { GAME.NETWORK.broadcast("relayed", {from: playerID, text: (str) data.text}); }
}
public action onPlayerLeave(str playerID) { GAME.log("leave", playerID); }
"""


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


# ---- KsValue binary encoding (shared/src/ks/ks_serial.c)
def varint(n):
    out = b""
    while True:
        b = n & 0x7F
        n >>= 7
        out += bytes([b | (0x80 if n else 0)])
        if not n:
            return out


def enc(v):
    if v is None:
        return b"\x01"
    if isinstance(v, bool):
        return b"\x03" if v else b"\x02"
    if isinstance(v, (int, float)):
        return b"\x04" + struct.pack("<d", float(v))
    if isinstance(v, str):
        b = v.encode()
        return b"\x05" + varint(len(b)) + b
    if isinstance(v, list):
        return b"\x06" + varint(len(v)) + b"".join(enc(x) for x in v)
    out = b"\x07" + varint(len(v))
    for k, x in v.items():
        kb = k.encode()
        out += varint(len(kb)) + kb + enc(x)
    return out


def dec(b, i=0):
    t = b[i]
    i += 1
    if t in (0, 1):
        return None, i
    if t in (2, 3):
        return t == 3, i
    if t == 4:
        return struct.unpack("<d", b[i:i + 8])[0], i + 8

    def rv(i):
        n, s = 0, 0
        while True:
            c = b[i]
            i += 1
            n |= (c & 0x7F) << s
            s += 7
            if not c & 0x80:
                return n, i
    if t == 5:
        n, i = rv(i)
        return b[i:i + n].decode(), i + n
    if t == 6:
        n, i = rv(i)
        out = []
        for _ in range(n):
            x, i = dec(b, i)
            out.append(x)
        return out, i
    n, i = rv(i)
    out = {}
    for _ in range(n):
        kn, i = rv(i)
        k = b[i:i + kn].decode()
        i += kn
        out[k], i = dec(b, i)
    return out, i


def send_msg(conn, mid, data, seq=[0]):
    body = bytes([len(mid)]) + mid.encode() + enc(data)
    chunk = 508
    count = (len(body) + chunk - 1) // chunk
    for c in range(count):
        conn.send(SCRIPT_MSG, struct.pack("<HBB", seq[0] & 0xFFFF, c, count) + body[c * chunk:(c + 1) * chunk])
    seq[0] += 1


def messages(conn):
    out, parts = [], {}
    for t, p in conn.packets:
        if t != SCRIPT_MSG:
            continue
        seq, c, count = struct.unpack("<HBB", p[:4])
        parts.setdefault(seq, b"")
        parts[seq] += p[4:]
        if c + 1 == count:
            body = parts.pop(seq)
            n = body[0]
            data, _ = dec(body, 1 + n)
            out.append((body[1:1 + n].decode(), data))
    return out


def logs(conn):
    return [p[1:].decode(errors="replace") for t, p in conn.packets if t == SCRIPT_LOG]


def client(minor):
    c = kn_proto.Conn("127.0.0.1", PORT)
    c.send(kn_proto.HELLO, bytes([1, minor, 0]))
    c.join()
    c.ent()
    return c


def main():
    with open("assets/maps/sandstorm.json") as f:
        m = json.load(f)
    m["scripts"] = {"server": base64.b64encode(SERVER_SCRIPT.encode()).decode(), "client": "public action start() { }"}
    map_path = os.path.join(WORK, "ks_map.json")
    with open(map_path, "w") as f:
        json.dump(m, f)
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    cfg = os.path.join(WORK, "s.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "KS e2e"\nport {PORT}\nregion local\nmaxplayers 8\nmode ffa\nround_seconds 600\nbot_quota 3\n')
    log_path = os.path.join(WORK, "server.log")
    out = open(log_path, "w")
    proc = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json"), map_path],
                            stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    try:
        time.sleep(1.5)
        a, b, old = client(3), client(3), client(0)
        for c in (a, b, old):
            c.read(timeout=2.0)
        server_log = open(log_path).read()
        check("server script loaded", "Loading the server script" in server_log and "[KRUNKSCRIPT] ks start" in server_log)
        check("SCRIPT_LOG to v7 peers only", len(logs(a)) > 0 and not any(t in (200, 201, 202, 203) for t, _ in old.packets),
              f"a {len(logs(a))} lines")

        send_msg(a, "check", {"age": 20})
        send_msg(b, "check", {"age": 9})
        for c in (a, b):
            c.read(timeout=1.0)
        ma = [d for i, d in messages(a) if i == "check"]
        mb = [d for i, d in messages(b) if i == "check"]
        check("Basic Networking reply to the sender", ma == [{"status": "Passed"}] and mb == [{"status": "Failed"}], f"{ma} {mb}")

        send_msg(a, "relay", {"text": "x" * 1200})  # three SCRIPT_MSG chunks each way
        for c in (a, b, old):
            c.read(timeout=1.0)
        ra = [d for i, d in messages(a) if i == "relayed"]
        rb = [d for i, d in messages(b) if i == "relayed"]
        check("two-client relay (chunked)", len(ra) == 1 and ra == rb and ra[0]["text"] == "x" * 1200, f"{len(ra)} {len(rb)}")
        check("custom sync values (SCRIPT_SYNC)", any(t == SCRIPT_SYNC for t, _ in a.packets))

        a.send(CHAT_SEND, b"hi there\x00")
        a.read(timeout=1.0)
        check("onChatMessage", any(line.startswith("chat hi there ") for line in logs(a)), str(logs(a)[-3:]))

        info = [p for t, p in a.packets if t == MAP_INFO]
        if info:
            a.send(MAP_REQUEST, info[-1][4:8])  # NetMapInfoPacket {map_index, hash, ...}
            a.read(timeout=2.0)
        data = b"".join(p[10:] for t, p in a.packets if t == MAP_DATA)
        check("scripts.server is not streamed", b'"client"' in data and b'"server"' not in data, f"{len(data)} bytes")

        time.sleep(4.0)  # the bots fight under the script hooks
        for c in (a, b):
            c.read(timeout=0.5)
        server_log = open(log_path).read()
        check("no runtime errors", "RUNTIME ERROR" not in server_log)
        b.close()
        a.read(timeout=1.5)
        check("onPlayerLeave", any(line.startswith("leave ") for line in logs(a)), str(logs(a)[-3:]))
    finally:
        proc.terminate()
        time.sleep(0.5)
        proc.terminate()  # a second signal stops now instead of finishing the round
        try:
            proc.wait(15)
        except subprocess.TimeoutExpired:
            proc.kill()
    server_log = open(log_path).read()
    check("server exits cleanly", proc.returncode == 0, str(proc.returncode))
    if not all(results):
        print("--- server.log (tail)\n" + "\n".join(server_log.splitlines()[-25:]))
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

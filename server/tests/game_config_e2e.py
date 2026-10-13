#!/usr/bin/env python3
"""v7.1.4 custom game config on a dedicated server_bin (doc/v7/map-logic.md §1.9, §4.3, §5.3, WP-E).

  python3 -I server/tests/game_config_e2e.py BUILD_DIR [PORT]      (run from the repository root)

One standalone server_bin with the server presets `map_config 1` and `game.jumpMlt 2` on the
community maps eterno_jump (embedded config: classes [9], gameTime 0, maxPlayers 20, headClipFix,
cls 9 wallJ false) and Krunk_Plaza, then:
  1. a protocol-1.3 client gets GAME_CONFIG (chunked JSON) + NETWORK_DATA: the map's settings, the
     server's game.* on top, config.classes and the cls overrides; a 1.0 client gets none of
     the lobby packets (block 180-189)
  2. ENT with class 0 spawns the only listed class (9)
  3. the round ends: MATCH_VOTE_STATE with the picks; a vote (MATCH_VOTE) is counted for the voter,
     the next INIT plays the voted map / mode
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
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
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 22631
WORK = tempfile.mkdtemp(prefix="kn-gc-e2e-")
MATCH_VOTE, MATCH_VOTE_STATE, NETWORK_DATA, GAME_CONFIG = 180, 181, 182, 183
END = 16
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def client(minor):
    c = kn_proto.Conn("127.0.0.1", PORT)
    c.send(kn_proto.HELLO, bytes([1, minor, 0]))
    c.join(name=f"gc{minor}")
    return c


def game_config(conn):
    """The last complete GAME_CONFIG JSON the connection received."""
    text, done = b"", None
    for t, p in conn.packets:
        if t != GAME_CONFIG:
            continue
        total, offset = struct.unpack("<II", p[:8])
        if offset == 0:
            text = b""
        text += p[8:]
        if len(text) == total:
            done = json.loads(text.decode())
    return done


def vote_states(conn):
    """MATCH_VOTE_STATE packets: (voted, [(mode, map, votes, name)])."""
    out = []
    for t, p in conn.packets:
        if t != MATCH_VOTE_STATE:
            continue
        count, voted = struct.unpack("<Bb", p[:2])
        entries = []
        for i in range(count):
            base = 2 + i * 42
            mode, mp, votes = struct.unpack("<iiH", p[base: base + 10])
            entries.append((mode, mp, votes, kn_proto.uncstr(p[base + 10: base + 42])))
        out.append((voted, entries))
    return out


def inits(conn):
    return [struct.unpack("<BBBiiHi", p[:17]) for t, p in conn.packets if t == kn_proto.INIT]  # 18 bytes from the ruleset byte


def main():
    cfg = os.path.join(WORK, "s.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "GC e2e"\nport {PORT}\nregion local\nmaxplayers 8\nmode ffa\n'
                'mapcycle eterno_jump,Krunk_Plaza\nround_seconds 3\nmap_config 1\ngame.jumpMlt 2\n')
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    log_path = os.path.join(WORK, "server.log")
    out = open(log_path, "w")
    proc = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                            stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    try:
        time.sleep(1.5)
        a, old = client(3), client(0)
        for c in (a, old):
            c.read(timeout=1.5)
        gc = game_config(a)
        check("GAME_CONFIG to the v7 client", gc is not None)
        check("no lobby packets to a 1.0 client", not any(180 <= t < 190 for t, _ in old.packets))
        check("NETWORK_DATA", any(t == NETWORK_DATA for t, _ in a.packets))
        init = inits(a)
        map_index = init[-1][3] if init else -1
        if gc:
            s = gc.get("settings", {})
            if map_index == 0:  # eterno_jump
                check("map settings (gameTime 0, maxPlayers 20, headClipFix)", s.get("gameTime") == 0 and s.get("maxPlayers") == 20 and s.get("headClipFix") is True,
                      f"{s.get('gameTime')} {s.get('maxPlayers')} {s.get('headClipFix')}")
                check("config.classes [9]", gc.get("classes") == [9], str(gc.get("classes")))
                check("cls 9 wallJ false", gc.get("cls", {}).get("9", {}).get("wallJ") is False, str(gc.get("cls")))
            else:  # Krunk_Plaza
                check("map settings (gameTime 0, maxPlayers 20)", s.get("gameTime") == 0 and s.get("maxPlayers") == 20, str(s.get("gameTime")))
                check("13 classes", len(gc.get("classes", [])) == 13, str(gc.get("classes")))
                check("cls overrides", gc.get("cls", {}).get("0", {}).get("health") == 120, "")
            check("server game.jumpMlt over the map", abs(s.get("jumpMlt", 0) - 2) < 1e-6, str(s.get("jumpMlt")))

        a.ent(class_index=0)
        a.read(timeout=1.5)
        mine = [struct.unpack_from("<iBBi", p) for t, p in a.packets if t == kn_proto.SPAWN and len(p) >= 10 and p[4] == 1]
        if mine:
            want = 9 if map_index == 0 else 0
            check("ENT class 0 -> a listed class", mine[-1][3] == want, f"class {mine[-1][3]} on map {map_index}")
        else:
            check("SPAWN", False)

        a.read(until=END, timeout=6.0)
        a.read(timeout=0.5)
        votes = vote_states(a)
        check("MATCH_VOTE_STATE at the end", len(votes) > 0 and len(votes[-1][1]) >= 1, str(votes[-1:] if votes else None))
        check("no MATCH_VOTE_STATE to a 1.0 client", not any(t == MATCH_VOTE_STATE for t, _ in old.packets))
        if votes and votes[-1][1]:
            picks = votes[-1][1]
            target = picks[-1]
            a.send(MATCH_VOTE, struct.pack("<ii", target[0], target[1]))
            a.read(timeout=1.0)
            after = vote_states(a)[-1]
            check("the vote is counted for the voter", after[0] == len(picks) - 1 and after[1][-1][2] == 1, str(after))
            n_inits = len(inits(a))
            a.read(until=kn_proto.INIT, timeout=30.0)
            nxt = inits(a)
            check("next INIT plays the voted match", len(nxt) > n_inits and nxt[-1][3] == target[1] and nxt[-1][4] == target[0],
                  f"{nxt[-1] if nxt else None} vs {target}")
            a.read(timeout=0.5)
            check("GAME_CONFIG before the next INIT", game_config(a) is not None)
        a.close()
        old.close()
    finally:
        proc.terminate()
        try:
            proc.wait(5)
        except subprocess.TimeoutExpired:
            proc.kill()
        out.close()
    if not all(results):
        print(open(log_path).read()[-3000:])
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

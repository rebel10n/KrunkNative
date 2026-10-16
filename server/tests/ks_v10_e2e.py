#!/usr/bin/env python3
"""KrunkScript v10.0.2 server API on a dedicated server_bin (doc/v10/map-format.md section 6).

  python3 -I server/tests/ks_v10_e2e.py BUILD_DIR [PORT]      (run from the repository root)

A standalone server_bin (bot_quota 3, maxplayers 8) on sandstorm whose scripts.server exercises the
v10 members added by this package, then:
  1. CONFIG.getMatch: {players, capacity, teams}; capacity is the configured maxplayers (8)
  2. TRIGGERS.list / getByInterface, OBJECTS.getByInterface, TRIGGERS.execute: answer arrays / nothing
  3. ENTITIES.list / findById / findByTag: empty / undefined; spawn, clone, despawn, LIVE_OBJECTS.addCube
     answer undefined (no entity manager and no runtime blocks on this server yet)
  4. PAYMENTS: the v10 validation messages ("Invalid player", "Missing callback", "Invalid currency
     name", "Calling giveCurrency too much", "Monetization is not enabled for this studio") and the
     callbacks with success false, once per call, for every bot that spawns
  5. ANIM / URLS / CONFIG.getSettings answer undefined; AI.removeBySid runs without an error
  6. no runtime errors on the bots' rounds
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import base64
import json
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 22641
WORK = tempfile.mkdtemp(prefix="kn-ks-v10-e2e-")
SCRIPT_LOG = 201
results = []

SERVER_SCRIPT = """
num cashedCount = 0;
public action start() {
    obj match = GAME.CONFIG.getMatch();
    GAME.log("capacity", toStr match.capacity);
    GAME.log("match", toStr match.players, toStr match.teams);
    obj[] triggers = GAME.TRIGGERS.list();
    GAME.log("triggers", toStr lengthOf triggers);
    obj[] byInterface = GAME.TRIGGERS.getByInterface(999);
    GAME.log("byInterface", toStr lengthOf byInterface);
    obj[] objs = GAME.OBJECTS.getByInterface(999);
    GAME.log("objectsByInterface", toStr lengthOf objs);
    GAME.TRIGGERS.execute(999, "nobody");
    GAME.TRIGGERS.execute(999);
    GAME.log("entities", toStr lengthOf GAME.ENTITIES.list(), toStr lengthOf GAME.ENTITIES.findByTag("crate"));
    GAME.log("findById", toStr GAME.ENTITIES.findById(3));
    GAME.log("spawn", toStr GAME.ENTITIES.spawn("crate", 0, 0, 0, {tag: "crate"}));
    GAME.log("addCube", toStr GAME.LIVE_OBJECTS.addCube(0, 0, 0, 1, 1, 1, 0, {color: "#fff"}));
    GAME.log("settings", toStr GAME.CONFIG.getSettings());
    GAME.log("playClip", toStr GAME.ANIM.playClip(0, 0, 1));
    GAME.AI.removeBySid(4);
    GAME.PAYMENTS.giveCurrency("nobody", "gold", 5, cashedNobody);
    GAME.PAYMENTS.getCurrencyBalance("nobody", "gold");
    GAME.PAYMENTS.giveCurrency("1", 7, 5, cashedNobody);
    GAME.PAYMENTS.giveSkin("nobody", 10000001, cashedNobody);
    GAME.PAYMENTS.authorizeSpend("nobody", 5, cashedNobody, true);
    GAME.PAYMENTS.charge("nobody", 5, cashedNobody);
}
public action onPlayerSpawn(str id) {
    obj p = GAME.PLAYERS.findByID(id);
    if (!p) { return; }
    GAME.PAYMENTS.giveCurrency(id, "gold", 5, cashed);
    GAME.PAYMENTS.takeCurrency(id, "gold", 5, cashed);
    GAME.PAYMENTS.getCurrencyBalance(id, "gold", cashed);
    GAME.PAYMENTS.giveSkin(id, 12, cashed);
    GAME.PAYMENTS.authorizeSpend(id, 5, cashedAuth, true);
    GAME.PAYMENTS.charge(id, 5, cashed);
    obj[] ops = obj[];
    GAME.PAYMENTS.transact(id, ops, "auth", cashed);
    GAME.PAYMENTS.giveCurrency(id, "gold", 5, cashed);
    GAME.PAYMENTS.giveCurrency(id, "gold", 5, cashed);
    GAME.PAYMENTS.giveCurrency(id, "gold", 5, cashed);
    GAME.PAYMENTS.giveCurrency(id, "gold", 5, cashed);
    GAME.PAYMENTS.giveCurrency(id, "gold", 5, cashed);
}
action cashed(str playerID, bool success, num value) {
    cashedCount += 1;
    GAME.log("cashed", playerID, toStr success, toStr value);
}
action cashedNobody(str playerID, bool success, num value) {
    GAME.log("cashedNobody", toStr playerID, toStr success);
}
action cashedAuth(str playerID, bool success, str value) {
    GAME.log("cashedAuth", toStr success, value);
}
"""


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def script_lines(log_text):
    """The script console lines of the server log ("[KRUNKSCRIPT] <text>"), in order."""
    prefix = "[KRUNKSCRIPT] "
    return [ln[len(prefix):] for ln in log_text.splitlines() if ln.startswith(prefix)]


def client(minor):
    c = kn_proto.Conn("127.0.0.1", PORT)
    c.send(kn_proto.HELLO, bytes([1, minor, 0]))
    c.join()
    c.ent()
    return c


def main():
    with open("assets/maps/311_sandstorm.json") as f:
        m = json.load(f)
    m["scripts"] = {"server": base64.b64encode(SERVER_SCRIPT.encode()).decode(), "client": "public action start() { }"}
    map_path = os.path.join(WORK, "ks_v10_map.json")
    with open(map_path, "w") as f:
        json.dump(m, f)
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    cfg = os.path.join(WORK, "s.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "KS v10 e2e"\nport {PORT}\nregion local\nmaxplayers 8\nmode ffa\nround_seconds 600\nbot_quota 3\n')
    log_path = os.path.join(WORK, "server.log")
    out = open(log_path, "w")
    proc = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json"), map_path],
                            stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    try:
        time.sleep(1.5)
        a = client(3)
        a.read(timeout=2.0)
        time.sleep(4.0)  # the bots spawn and the spawn hooks run
        a.read(timeout=1.0)
        server_log = open(log_path).read()
        lines = script_lines(server_log)

        check("server script loaded and start() ran", "Loading the server script" in server_log and "capacity 8" in lines,
              str(lines[:3]))
        check("CONFIG.getMatch capacity is maxplayers", "capacity 8" in lines, str(lines[:2]))
        check("CONFIG.getMatch players and teams", any(x.startswith("match ") for x in lines), str(lines[:4]))
        check("TRIGGERS.list answers an array", any(x.startswith("triggers ") for x in lines))
        check("TRIGGERS.getByInterface / OBJECTS.getByInterface: none on interface 999",
              "byInterface 0" in lines and "objectsByInterface 0" in lines, str(lines))
        check("ENTITIES.list / findByTag: empty", "entities 0 0" in lines, str(lines))
        check("ENTITIES.findById / spawn / LIVE_OBJECTS.addCube answer undefined",
              "findById undefined" in lines and "spawn undefined" in lines and "addCube undefined" in lines, str(lines))
        check("CONFIG.getSettings and ANIM.playClip answer undefined", "settings undefined" in lines and "playClip undefined" in lines)
        check("Invalid player is reported and its callback runs with success false (id = the argument's own id: none)",
              "Invalid player" in lines and "cashedNobody  false" in lines, str(lines[:12]))
        check("getCurrencyBalance without a callback: Missing callback", "Missing callback" in server_log)
        check("giveSkin below the custom skin base: Invalid skin id", "Invalid skin id" in server_log)
        check("authorizeSpend with a value on a studio-less server: Monetization is not enabled",
              "Monetization is not enabled for this studio" in server_log)
        check("authorizeSpend callback answers the empty string", "cashedAuth false " in lines,
              str([x for x in lines if x.startswith("cashedAuth")][:2]))
        check("giveCurrency rate limit (5 per 10 s): Calling giveCurrency too much",
              "Calling giveCurrency too much" in server_log)
        cashed_lines = [x for x in lines if x.startswith("cashed ")]
        check("every spawned bot's payment callbacks run with success false", len(cashed_lines) >= 6 and
              all(x.split()[2] == "false" for x in cashed_lines), f"{len(cashed_lines)} callbacks")
        check("no runtime errors on the bots' rounds", "RUNTIME ERROR" not in server_log)
        a.close()
    finally:
        proc.terminate()
        time.sleep(0.5)
        proc.terminate()
        try:
            proc.wait(15)
        except subprocess.TimeoutExpired:
            proc.kill()
    server_log = open(log_path).read()
    check("server exits cleanly", proc.returncode == 0, str(proc.returncode))
    if not all(results):
        print("--- server.log (tail)\n" + "\n".join(server_log.splitlines()[-40:]))
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

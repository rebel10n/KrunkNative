#!/usr/bin/env python3
"""Bots play every Krunker v7.1.4 game mode on a dedicated server (doc/v7/map-logic.md §4.1, WP-D).

  python3 -I server/tests/modes_bots_e2e.py BUILD_DIR [MODE...]      (run from the repository root)

For each mode id 0..36 (37 / 38 are reserved for ranked: no mode): one standalone server_bin on Sandstorm (the v7.1.4
sandstorm_v3: objectives, team + neutral flags, Domination zones) with `bot_quota 4`, a guest that
announces protocol 1.3 (NET_PROTOCOL_MINOR_V7) and watches for a while. Checks per mode: the guest
spawns in that mode, the server keeps ticking (STATE packets) and survives, and the mode's own
packets show up where the mode makes them certain (objective / dom / spawnable / rounds blocks).
Objective play (bots-objectives, shared/src/bot_objective.c): in the modes with an objective the
watcher keeps reading for up to the mode's budget (PLAY: 150 s for the flag modes, else 90;
KN_MODES_OBJECTIVE_BUDGET=<s> sets every one) and checks the bots score it on the real map: a flag
capture (CTF / Blitz), a bot standing on the active hardpoint / hill / a Domination
zone / a bomb site for KN_MODES_HOLD_SECONDS (5) without leaving (STATE positions against the map's
volumes), a zone capture ("cct"), a carried neutral flag (tdf / car), a confirm / deposit (team
score), the round decided (Last Man Standing). KN_MODES_OBJECTIVE_BUDGET=0 skips that phase. The
watcher respawns when killed (a dead player gets no positions).
KN_MODES_FLAVOR=strike runs every server as Krunker Strike (server_bin --flavor strike: FRVR's
class / weapon / mode tables, shared/include/strike_rules.h) and skips the modes Strike does not
have (Trade, Impulsed: the server refuses them).
"""
import json
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
TEAM_SCORE, FLAG, ACTION, MODE_END = 43, 44, 47, 51
OBJECTIVE_V7, OBJECTIVE_OCCUPANCY, DOM_UPDATE, DOM_CAPTURE, SPAWNABLE_V7, MODE_SYNC_V7 = 140, 141, 142, 143, 148, 149
WATCH = float(os.environ.get("KN_MODES_WATCH", "8"))
BUDGET_ENV = os.environ.get("KN_MODES_OBJECTIVE_BUDGET")  # seconds for every mode (unset: the PLAY budgets)
HOLD_SECONDS = float(os.environ.get("KN_MODES_HOLD_SECONDS", "5"))
FLAVOR = os.environ.get("KN_MODES_FLAVOR", "")  # "strike": server_bin --flavor strike
FLAVOR_ARGS = ["--flavor", FLAVOR] if FLAVOR else []
STRIKE_ABSENT = {"trade", "imp"}  # v7 modes FRVR's Strike table leaves out (strike_data.json modes)
PREFAB_OBJECTIVE, PREFAB_DOMINATION_ZONE, PREFAB_BOMB_SITE = 14, 77, 45

MODES = ["ffa", "tdm", "point", "ctf", "bhop", "hide", "infect", "race", "lms", "simon", "gun", "prop", "boss", "clas",
         "depo", "stalk", "king", "oinc", "trade", "kc", "def", "shrp", "trai", "raid", "blitz", "dom", "sdm", "krank",
         "tdf", "depoffa", "car", "aon", "md", "chs", "bhffa", "zom", "imp"]
# what a joiner gets for sure: MODE_SYNC_V7 always (objective modes: an objective uid in it),
# Domination its zones ("uct" sync.doms)
OBJECTIVE_MODES = {2, 14, 16, 29}
EXPECT = {25: DOM_UPDATE}
# mode id -> (what the bots must do, the stat that proves it, the count it must reach, the map, the
# budget in seconds). Easy bots, 3 bots and the idle watcher: a flag run (get, carry home, capture)
# takes the longest.
PLAY = {
    2: ("hold the hardpoint", "held_objective", 1, "Sandstorm", 90),
    3: ("capture a flag", "captures", 1, "Sandstorm", 150),
    8: ("decide the round", "ends", 1, "Sandstorm", 90),
    14: ("deposit KR Tags", "team_score", 1, "Sandstorm", 90),
    16: ("hold the hill", "held_objective", 1, "Sandstorm", 90),
    19: ("confirm a kill", "team_score", 1, "Sandstorm", 90),
    20: ("hold a bomb site", "held_site", 1, "Kanji", 90),
    24: ("capture a flag", "team_score", 1, "Sandstorm", 150),
    25: ("capture a zone", "dom_captures", 1, "Sandstorm", 90),
    28: ("carry the flag", "carried", 1, "Sandstorm", 90),
    30: ("carry the flag", "carried", 1, "Sandstorm", 90),
}
results = []


def map_volumes(name):
    """The objective / domination zone / bomb site boxes of a map: (prefab, cx, y0, cz, hx, sy, hz)."""
    path = {"Sandstorm": "assets/maps/official/02_sandstorm.json", "Kanji": "assets/maps/official/10_kanji.json"}[name]
    with open(path) as f:
        data = json.load(f)
    xyz = data.get("xyz") or []  # v7 pooled sizes: "si" indexes a triple
    out = []
    for o in data.get("objects", []):
        if o.get("i") not in (PREFAB_OBJECTIVE, PREFAB_DOMINATION_ZONE, PREFAB_BOMB_SITE) or len(o.get("p", [])) < 3:
            continue
        p, s = o["p"], o.get("s") or xyz[3 * o.get("si", -1): 3 * o.get("si", -1) + 3]
        if len(s) >= 3:
            out.append((o["i"], p[0], p[1], p[2], s[0] / 2.0, s[1], s[2] / 2.0))
    return out


def inside(volume, x, y, z):
    _, cx, y0, cz, hx, sy, hz = volume
    return abs(x - cx) <= hx and y0 - 1.0 <= y <= y0 + sy and abs(z - cz) <= hz


class Play:
    """Objective evidence from the packets the watcher reads: scores, captures, flags, and how long
    each bot stands inside an objective / zone / site without leaving (STATE positions)."""

    def __init__(self, volumes):
        self.volumes = volumes
        self.bots = {}  # uid -> name of every other player seen spawning
        self.stay = {}  # (uid, prefab) -> [since, last]
        self.stats = {"team_score": 0, "captures": 0, "dom_captures": 0, "carried": 0, "ends": 0, "kills": 0,
                      "held_objective": 0, "held_site": 0, "held_zone": 0, "occupied": 0}

    def feed(self, t, p, now):
        st = self.stats
        if t == SPAWN:
            info = kn_proto.spawn_info(p)
            if info and not info["is_you"]:
                self.bots[info["uid"]] = info["name"]
        elif t == TEAM_SCORE and len(p) >= 8:
            st["team_score"] = max(st["team_score"], struct.unpack("<ii", p[:8])[1])
        elif t == ACTION and b"captured the Flag" in p:
            st["captures"] += 1
        elif t == DOM_CAPTURE:
            st["dom_captures"] += 1
        elif t == FLAG and len(p) >= 24 and struct.unpack("<i", p[16:20])[0] >= 0:
            st["carried"] += 1
        elif t == OBJECTIVE_OCCUPANCY and len(p) >= 8 and struct.unpack("<ii", p[:8])[1] >= 1:
            st["occupied"] += 1
        elif t == MODE_END:
            st["ends"] += 1
        elif t == KILL:
            st["kills"] += 1
        elif t == STATE and len(p) >= 25:  # NetStatePacket (packed, 57 bytes): uid, ack, active, loadout, position...
            uid, _, active, _, x, y, z = struct.unpack("<iiBi3f", p[:25])
            if uid not in self.bots or not active:
                return
            for v in self.volumes:
                key = (uid, v[0])
                if inside(v, x, y, z):
                    span = self.stay.get(key)
                    if span and now - span[1] < 1.5:  # STATE comes ~10 Hz per player; reads end every 0.25 s
                        span[1] = now
                    else:
                        self.stay[key] = span = [now, now]
                    held = span[1] - span[0]
                    name = {PREFAB_OBJECTIVE: "held_objective", PREFAB_DOMINATION_ZONE: "held_zone", PREFAB_BOMB_SITE: "held_site"}[v[0]]
                    if held >= HOLD_SECONDS:
                        st[name] = max(st[name], 1)


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
        play = PLAY.get(mode_id)
        budget = float(BUDGET_ENV) if BUDGET_ENV is not None else play[4] if play else 0.0
        if budget <= 0:
            play = None
        map_name = play[3] if play else "Sandstorm"
        round_seconds = int(budget + WATCH + 30) if play else 40
        f.write(f'hostname "Modes #{mode_id}"\nport {port}\nregion local\nmaxplayers 8\nmapcycle {map_name}\nmode {alias}\n'
                f"round_seconds {round_seconds}\nbot_quota 4\nbot_difficulty easy\n")
    log_path = os.path.join(WORK, f"m{mode_id}.log")
    log = open(log_path, "w")
    proc = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json"), *FLAVOR_ARGS],
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
        if play and ok:
            # objective play: keep reading until the bots did it or the budget runs out
            what, stat, want = play[:3]
            evidence = Play(map_volumes(map_name))
            fed = 0
            started = time.time()
            deadline = started + budget
            last_state = respawn_at = started
            while time.time() < deadline and evidence.stats[stat] < want:
                conn.read(timeout=min(0.25, max(0.05, deadline - time.time())))  # packets of one read share its end time
                now = time.time()
                for t, p in conn.packets[fed:]:
                    evidence.feed(t, p, now)
                    if t == STATE:
                        last_state = now
                fed = len(conn.packets)
                # a dead watcher gets no player positions (the "k" sync): respawn it like a player clicking in
                if now - last_state > 2.0 and now - respawn_at > 3.0:
                    respawn_at = now
                    conn.ent(class_index=0, lag_comp=1.0)
                if proc.poll() is not None:
                    break
            took = time.time() - started
            st = evidence.stats
            summary = " ".join(f"{k} {v}" for k, v in st.items() if v)
            check(f"mode {mode_id:2d} {alias}: bots {what} ({stat} >= {want} within {budget:.0f} s)", st[stat] >= want,
                  f"{took:.0f} s, {len(evidence.bots)} others: {summary or 'nothing'}")
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
        if FLAVOR == "strike" and MODES[mode_id] in STRIKE_ABSENT:
            print(f"SKIP  mode {mode_id:2d} {MODES[mode_id]}: not a Krunker Strike mode", flush=True)
            continue
        run_mode(mode_id, MODES[mode_id], kn_proto.free_ports(1)[0])  # ephemeral: concurrent runs


try:
    main()
except Exception as e:  # noqa: BLE001
    check("exception", False, repr(e))
print(f"\n{sum(results)}/{len(results)} passed")
sys.exit(0 if results and all(results) else 1)

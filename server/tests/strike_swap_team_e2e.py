#!/usr/bin/env python3
"""Krunker Strike's Tap-to-Play "Switch Teams" end to end: NET_PACKET_EXT subtype NET_EXT_KN_SWAP_TEAM
(0x8110, shared/include/net_ext.h), handled by server/src/net.c and mode_swap_team (shared/src/modes/
mode_actions.c). The client's button (client/src/ui/strike_menu.c) sends the same packet after its count.

  python3 -I server/tests/strike_swap_team_e2e.py BUILD_DIR      (run from the repository root)

Standalone server_bin processes (no brain, perms off and cheats "all" so /kill is open to a guest). Two
protocol 1.4 clients per server: A switches, B watches the scoreboard's broadcasts and /teams.
  1. team mode (tdm) with selTeam on: A alive -> the switch is refused (no NET_PACKET_TEAM, team kept);
     A's /kill (a death between lives) -> the switch is accepted: B gets NET_PACKET_TEAM (uid A, the other
     team), /teams lists A on that side, A's next spawn is on the other team; a further switch while
     alive, or to the team it is on, changes nothing.
  2. tdm with selTeam off: the switch between lives is refused.
  3. ffa (no teams) with selTeam on: the switch between lives is refused and the spawn stays teamless.
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
WORK = tempfile.mkdtemp(prefix="kn-swapteam-e2e-", dir=os.path.expanduser("~/.cache"))
PORTS = kn_proto.free_ports(3)
os.makedirs(WORK, exist_ok=True)
with open(os.path.join(WORK, "none.json"), "w") as f:  # no coordinator config: a standalone server
    f.write("{}")
HELLO_1_4 = bytes([1, 4, 2, 1])  # protocol 1.4: NET_PACKET_EXT peers
INIT, SPAWN, KILL, CHAT, CHAT_SEND, EXT = 2, 4, 10, 9, 8, 254
TEAM = 42                        # NET_PACKET_TEAM: NetTeamPacket "<iifi" (uid, team, health, max_health)
EXT_SWAP_TEAM = 0x8110           # NET_EXT_KN_SWAP_TEAM: C -> S NetSwapTeamPacket { uint8 team }
SPAWN_FMT = "<iBBiifi3f2f32si7i"  # kn_proto.spawn_info's layout; index 4 is the team
CHAT_FMT = "<iB32s288s"           # NetChatPacket: the text is field 3
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def start_server(i, mode, select_team):
    port = PORTS[i]
    cfg = os.path.join(WORK, f"s{i}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Swap #{i}"\nport {port}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\n'
                f'mode {mode}\nperms 0\ncheats all\ngame.selTeam {1 if select_team else 0}\n')
    log = os.path.join(WORK, f"s{i}.log")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    end = time.time() + 20
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 0.5).close()
            return port
        except OSError:
            time.sleep(0.2)
    return None


class Player(kn_proto.Conn):
    """A guest on protocol 1.4: joins, spawns (ENT), and reads the broadcasts."""

    def __init__(self, port, name):
        super().__init__("127.0.0.1", port)
        self.send(kn_proto.HELLO, HELLO_1_4)
        self.join(name=name)
        self.read(until=INIT, timeout=8)

    def spawn(self):
        """ENT, then our own spawn: (uid, team, active) or None."""
        self.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 6
        while time.time() < end:
            p = self.read(until=SPAWN, timeout=1)
            if p is not None and len(p) >= struct.calcsize(SPAWN_FMT):
                v = struct.unpack(SPAWN_FMT, p[: struct.calcsize(SPAWN_FMT)])
                if v[1]:  # is_you
                    return {"uid": v[0], "active": v[2], "team": v[4], "name": kn_proto.uncstr(v[12])}
        return None

    def swap_team(self, team):
        self.send(EXT, struct.pack("<HB", EXT_SWAP_TEAM, team))

    def say(self, text):
        self.send(CHAT_SEND, text.encode() + b"\0")

    def team_msgs(self, mark, uid):
        """The teams NET_PACKET_TEAM sent for `uid` since packets[mark]."""
        out = []
        for t, p in self.packets[mark:]:
            if t == TEAM and len(p) >= 16:
                v = struct.unpack("<iifi", p[:16])
                if v[0] == uid:
                    out.append(v[1])
        return out

    def chat_lines(self, mark):
        n = struct.calcsize(CHAT_FMT)
        return [kn_proto.uncstr(struct.unpack(CHAT_FMT, p[:n])[3]) for t, p in self.packets[mark:] if t == CHAT and len(p) >= n]

    def kills(self, mark):
        return sum(1 for t, _ in self.packets[mark:] if t == KILL)


def settle(client, seconds=1.0):
    client.read(timeout=seconds)


def scenario_tdm(port):
    print("-- team mode (tdm), selTeam on", flush=True)
    a = Player(port, "SwA")
    b = Player(port, "SwB")
    sa, sb = a.spawn(), b.spawn()
    ok = sa is not None and sb is not None and sa["active"] and sb["active"] and sa["team"] in (1, 2)
    check("tdm: both players spawn alive on a team", ok, f"A {sa} B {sb}")
    if not ok:
        return
    uid, orig = sa["uid"], sa["team"]
    other = 3 - orig

    mark = len(b.packets)
    a.swap_team(other)
    settle(b)
    check("alive: the switch is refused (no NET_PACKET_TEAM for A)", not b.team_msgs(mark, uid))

    mark = len(b.packets)
    a.say("/kill")
    settle(b)
    check("A's /kill is a death: a KILL reaches B", b.kills(mark) >= 1, f"kills {b.kills(mark)}")

    mark = len(b.packets)
    a.swap_team(other)
    settle(b)
    msgs = b.team_msgs(mark, uid)
    check("between lives: B gets NET_PACKET_TEAM (uid A, the other team)", msgs and msgs[-1] == other, f"teams {msgs}")

    mark = len(b.packets)
    b.say("/teams")
    settle(b)
    label = "Red" if other == 1 else "Blue"
    lines = b.chat_lines(mark)
    name = sa["name"]  # the server names guests (Guest_N); the spawn carries the name the list shows
    check("/teams (B's scoreboard) lists A on the new team", any(line.startswith(label) and name in line for line in lines), str(lines))

    sa2 = a.spawn()
    check("the next spawn (Play) is on the new team", sa2 is not None and sa2["team"] == other and sa2["active"], f"{sa2}")

    mark = len(b.packets)
    a.swap_team(orig)
    settle(b)
    check("alive again: a second switch is refused", not b.team_msgs(mark, uid))

    a.say("/kill")
    settle(a, 0.5)
    mark = len(b.packets)
    a.swap_team(other)
    settle(b)
    check("the team it is on already: nothing changes", not b.team_msgs(mark, uid))

    a.close()
    b.close()


def scenario_no_select(port):
    print("-- team mode (tdm), selTeam off", flush=True)
    a = Player(port, "SwC")
    b = Player(port, "SwD")
    sa, sb = a.spawn(), b.spawn()
    if not (sa and sb and sa["active"]):
        check("selTeam off: players spawn", False, f"{sa} {sb}")
        return
    uid, orig = sa["uid"], sa["team"]
    a.say("/kill")
    settle(b)
    mark = len(b.packets)
    a.swap_team(3 - orig)
    settle(b)
    check("selTeam off: the switch between lives is refused", not b.team_msgs(mark, uid))
    a.close()
    b.close()


def scenario_ffa(port):
    print("-- ffa (no teams), selTeam on", flush=True)
    a = Player(port, "SwE")
    b = Player(port, "SwF")
    sa, sb = a.spawn(), b.spawn()
    if not (sa and sb and sa["active"]):
        check("ffa: players spawn", False, f"{sa} {sb}")
        return
    uid = sa["uid"]
    a.say("/kill")
    settle(b)
    mark = len(b.packets)
    a.swap_team(1)
    settle(b)
    check("ffa: the switch between lives is refused (no NET_PACKET_TEAM)", not b.team_msgs(mark, uid))
    settle(a, 3.0)  # FFA's respawn waits a moment after a death
    sa2 = a.spawn()
    check("ffa: the spawn stays teamless", sa2 is not None and sa2["team"] == 0, f"{sa2}")
    a.close()
    b.close()


def main():
    ports = {}
    try:
        ports["tdm"] = start_server(0, "tdm", True)
        ports["off"] = start_server(1, "tdm", False)
        ports["ffa"] = start_server(2, "ffa", True)
        if not all(ports.values()):
            check("servers start", False, str(ports))
        else:
            scenario_tdm(ports["tdm"])
            scenario_no_select(ports["off"])
            scenario_ffa(ports["ffa"])
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
        shutil.rmtree(WORK, ignore_errors=True)
    passed = sum(results)
    print(f"{passed} of {len(results)} checks passed", flush=True)
    return 0 if results and passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())

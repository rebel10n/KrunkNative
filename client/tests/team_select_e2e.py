#!/usr/bin/env python3
"""The menu's team pick on a selTeam game end to end (client/src/ui/comp_ui.c: toggleTeam, #selectT, the page's ?tm=).

  python3 -I client/tests/team_select_e2e.py BUILD_DIR      (run from the repository root)

v10.0.7 keeps the menu's pick in iïïíìïí, 1 until a toggleTeam (deob-10.0.7.js:217234, :226069), and sends it with
every ENTER_GAME ([8], :215052). The page's ?tm= (iiiîíìï = parseInt(getParam("tm", 0)) || 0, :204367) is applied
on the first game with config.selTeam: tm 2 clicks #selectT (toggleTeam(2)), then it is cleared (:226421, :228977).
On a KrunkNative server the pick travels as NET_EXT_KN_SWAP_TEAM, kept before the first spawn.

Standalone server_bin (tdm, game.selTeam 1) per run; a protocol 1.4 peer (server/tests/kn_proto.py) picks a team
and spawns first, so the server's balance would put the client on the other team; then the headless client_bin
joins (--connect), and its input script presses Play. The peer sees the client's SPAWN and its team.
  A. no ?tm=: the peer on team 1; the client spawns on team 1 (the default pick, not the balance's 2) and
     #selectT is unchecked.
  B. --game "tm=2": the peer on team 2; the client spawns on team 2 (not the balance's 1) and #selectT is checked.
Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
REPO = os.getcwd()
sys.path.insert(0, os.path.join(REPO, "server", "tests"))
import kn_proto  # noqa: E402

CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-team-select-", dir=CACHE if os.path.isdir(CACHE) else None)
HELLO_1_4 = bytes([1, 4, 2, 1])  # protocol 1.4: NET_PACKET_EXT peers
INIT, SPAWN, EXT = 2, 4, 254
EXT_SWAP_TEAM = 0x8110           # NET_EXT_KN_SWAP_TEAM: NetSwapTeamPacket { uint8 team }
SPAWN_FMT = "<iBBiifi3f2f32si7i"  # kn_proto.spawn_info's layout: 0 uid, 1 is_you, 2 active, 4 team
SELECTED = "[id=selectT]:checked"  # "#" starts a comment in an input script
results = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


class Peer(kn_proto.Conn):
    def __init__(self, port, name):
        super().__init__("127.0.0.1", port)
        self.send(kn_proto.HELLO, HELLO_1_4)
        self.join(name=name)
        self.read(until=INIT, timeout=8)
        self.uid = None

    def pick_and_spawn(self, team):
        self.send(EXT, struct.pack("<HB", EXT_SWAP_TEAM, team))
        self.read(timeout=0.5)
        self.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 6
        while time.time() < end:
            for t, p in self.spawn_packets():
                if p[1] and p[2]:
                    self.uid = p[0]
                    return p[4]
            self.read(until=SPAWN, timeout=1)
        return None

    def spawn_packets(self):
        n = struct.calcsize(SPAWN_FMT)
        return [(t, struct.unpack(SPAWN_FMT, p[:n])) for t, p in self.packets if t == SPAWN and len(p) >= n]

    def other_spawns(self):
        return {v[0]: v[4] for _, v in self.spawn_packets() if not v[1] and v[0] != self.uid}

    def pump(self, secs):
        self.read(timeout=secs)


def start_server(name):
    (port,) = kn_proto.free_ports(1)
    cfg = os.path.join(WORK, f"{name}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Team {name}"\nport {port}\nregion local\nmaxplayers 8\nmapcycle Sandstorm\nmode tdm\n'
                'perms 0\ngame.selTeam 1\n')
    none = os.path.join(WORK, "none.json")
    with open(none, "w") as f:
        f.write("{}")
    log = open(os.path.join(WORK, f"{name}-server.log"), "w")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", none], cwd=REPO, stdout=log,
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    end = time.time() + 20
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 0.5).close()
            return p, port
        except OSError:
            time.sleep(0.2)
    return p, None


def stop(p):
    if p and p.poll() is None:
        try:
            os.killpg(p.pid, signal.SIGTERM)
            p.wait(timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()


def run(name, peer_team, query):
    srv, port = start_server(name)
    client = None
    try:
        if not port:
            check(f"{name}: server_bin is listening", False)
            return
        peer = Peer(port, "Watcher")
        team = peer.pick_and_spawn(peer_team)
        check(f"{name}: the peer spawned on team {peer_team}", team == peer_team, f"team {team}")
        script = os.path.join(WORK, f"{name}.script")
        with open(script, "w") as f:
            # the page first (the script clock runs from the start); with ?tm=2, the click on the selTeam round itself
            wait = f"100 waitfor {SELECTED}\n" if query else ""
            f.write(f"0 waitfor [id=teamSelector]\n{wait}3000 nodes @{name}-sel {SELECTED}\n3000 nodes @{name}-el [id=selectT]\n"
                    f"3200 play\n6000 nodes @{name}-done {SELECTED}\n6100 quit\n")
        cfg = os.path.join(WORK, f"{name}-cfg")
        env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_MODE")}
        env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": cfg, "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"),
                    "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "KRUNKNATIVE_UPDATES_URL": "off",
                    "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_FIXED_DELTA": "16.667"})
        env.setdefault("KRUNKNATIVE_ASSETS_DIR", os.path.join(REPO, "assets"))
        args = [f"{BUILD}/client_bin"] + (["--game", query] if query else []) + ["--connect", f"127.0.0.1:{port}"]
        log = os.path.join(WORK, f"{name}-client.log")
        with open(log, "w") as out:
            client = subprocess.Popen(args, cwd=REPO, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                                      start_new_session=True)
            end = time.time() + 240
            while time.time() < end and client.poll() is None:
                peer.pump(0.2)
            if client.poll() is None:
                check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
        text = open(log, errors="replace").read()
        counts = {m.group(1): int(m.group(2)) for m in re.finditer(r"\[input_script\] nodes t=\d+ (@\S+) elements=\d+ match=(\d+)", text)}
        peer.pump(0.5)
        spawns = peer.other_spawns()
        want = 2 if query else 1
        check(f"{name}: #selectT is {'checked' if want == 2 else 'unchecked'} on the selTeam game",
              counts.get(f"@{name}-el") == 1 and counts.get(f"@{name}-sel") == (1 if want == 2 else 0), f"{counts}")
        check(f"{name}: the client spawned on team {want} (the balance would give {3 - peer_team})",
              list(spawns.values()) == [want], f"spawns {spawns}, log {log}")
        peer.close()
    finally:
        stop(client)
        stop(srv)


def main():
    if not os.path.exists(f"{BUILD}/server_bin") or not os.path.exists(f"{BUILD}/client_bin"):
        print(f"skipped: no server_bin / client_bin in {BUILD}")
        return 77
    run("default", 1, None)
    run("tm2", 2, "tm=2")
    ok = results and all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok:
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

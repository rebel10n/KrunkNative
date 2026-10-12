#!/usr/bin/env python3
"""Host idle kick after a map change, and the tp / bring console commands, on server_bin.

  python3 -I server/tests/host_tp_e2e.py BUILD_DIR      (run from the repository root)

No database or coordinator: standalone server_bin processes from generated server.cfg files.
The host connects over loopback (the server's own machine: a DEV peer, like whoever runs a
local server); the guest writes a NET_PACKET_FORWARDED first (a bridged remote browser), so it
is neither DEV nor host.
  1. idle_kick_seconds 3: the host dies, switches the map in chat (/map) and idles in the menu
     and then in game well past the timeout: never kicked; the idle guest is kicked (000005).
     (Before the fix the host was kicked right after the map change: the new map did not
     respawn it and its idle clock still ran from the connect.)
  2. tp / bring (shared/src/chat_commands.c, doc/console.md "Teleports"): tp x y z, tp
     <player>, bring <player> [x y z], bring all; POSITION to the moved player and
     CHANGE_POSITION to the others; a guest is refused ("Host or developer required");
     rcon bring all <x y z> moves everyone; rcon tp is refused (no player).
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
RCON_PORT, *PORTS = kn_proto.free_ports(3)  # ephemeral: concurrent runs must not share servers
WORK = tempfile.mkdtemp(prefix="kn-host-tp-e2e-")
SPAWN, CHAT_SEND, CHAT, INIT, ERROR = 4, 8, 9, 2, 30
NOTIFY, FORWARDED, POSITION, CHANGE_POSITION = 82, 85, 104, 127
results = []
procs = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def query(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", port))
        data, _ = s.recvfrom(2048)
    return data


def rcon(*cmd):
    out = subprocess.run([f"{BUILD}/server_bin", "--rcon", f"127.0.0.1:{RCON_PORT}", "--rcon-password", "rc-pw", *cmd],
                         capture_output=True, text=True, timeout=10)
    return out.returncode, out.stdout + out.stderr


def wait_for(fn, timeout=15.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.2)
    return None


def start_server(i, extra):
    cfg = os.path.join(WORK, f"s{i}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Host TP #{i}"\nport {PORTS[i]}\nregion local\nmaxplayers 8\nmapcycle Sandstorm,Kanji\nmode ffa\n'
                f"round_seconds 600\n{extra}")
    out = open(os.path.join(WORK, f"s{i}.log"), "w")
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                         stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    return p


class Peer(kn_proto.Conn):
    """A v7 (1.3.0) peer: CHANGE_POSITION (map-logic block) reaches it. `closed` once the server hung up."""

    def __init__(self, port, remote_address=None):
        super().__init__("127.0.0.1", port)
        self.closed = False
        if remote_address:
            self.send(FORWARDED, remote_address.encode() + b"\0")
        self.send(kn_proto.HELLO, bytes([1, 3, 0]))
        self.join()
        self.me = None

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end and not self.closed:
            if self.read(timeout=max(0.05, end - time.time())) is None and time.time() < end - 0.05:
                self.closed = True  # read() returned early: the socket closed

    def spawn(self):
        self.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 5
        while time.time() < end and self.me is None:
            p = self.read(until=SPAWN, timeout=1)
            if p is not None and kn_proto.spawn_info(p)["is_you"]:
                self.me = kn_proto.spawn_info(p)
        return self.me

    def say(self, text):
        self.send(CHAT_SEND, text.encode() + b"\0")
        self.pump(0.4)  # the chat burst limit is 3 lines per second

    def mark(self):
        return len(self.packets)

    def idle_kicked(self):
        return any(t == ERROR and kn_proto.error_info(p)[1] == "000005" for t, p in self.packets)

    def got_text(self, text, since=0):
        needle = text.encode()
        return any(t in (CHAT, NOTIFY) and needle in p for t, p in self.packets[since:])

    def moved(self, uid, xyz, since=0):
        """CHANGE_POSITION (uid, x, y, z, recon, stop) for uid at xyz, or POSITION (own) at xyz when uid is None."""
        for t, p in self.packets[since:]:
            if uid is not None and t == CHANGE_POSITION and len(p) >= 19:
                u, x, y, z, recon, stop = struct.unpack("<i3fBB", p[:18])
                if u == uid and (x, y, z) == xyz and recon and stop:
                    return True
            if uid is None and t == POSITION and len(p) >= 13:
                x, y, z, stop = struct.unpack("<3fB", p[:13])
                if (x, y, z) == xyz and stop:
                    return True
        return False


def pump_all(peers, seconds):
    end = time.time() + seconds
    while time.time() < end:
        for p in peers:
            p.pump(0.1)


def idle_after_map_change():
    start_server(0, "idle_kick_seconds 3\n")
    check("idle server up", bool(wait_for(lambda: query(PORTS[0]))))
    host = Peer(PORTS[0])
    check("host spawned", host.spawn() is not None)
    guest = Peer(PORTS[0], "203.0.113.7")
    check("guest spawned", guest.spawn() is not None)

    host.say("/kill")  # dead at the map change: the new map does not respawn the host
    pump_all([host, guest], 1.5)
    inits = sum(1 for t, _ in host.packets if t == INIT)
    host.say("/map Kanji")
    pump_all([host, guest], 1.0)
    check("host switched the map (INIT)", sum(1 for t, _ in host.packets if t == INIT) == inits + 1)
    pump_all([host, guest], 6.0)  # 2x the timeout in the menu
    check("host not kicked after the map change (in the menu)", not host.closed and not host.idle_kicked(),
          f"closed={host.closed} kicked={host.idle_kicked()}")
    check("the idle guest is kicked (000005)", guest.idle_kicked(), f"closed={guest.closed}")
    host.me = None
    check("host respawns on the new map", host.spawn() is not None)
    pump_all([host], 5.0)  # idles in game past the timeout
    check("host idles in game past the timeout", not host.closed and not host.idle_kicked())
    host.close()
    guest.close()


def tp_bring():
    start_server(1, f"rcon_password rc-pw\nrcon_port {RCON_PORT}\n")
    check("tp server up", bool(wait_for(lambda: query(PORTS[1]))))
    host = Peer(PORTS[1])
    guest = Peer(PORTS[1], "203.0.113.8")
    check("both spawned", host.spawn() is not None and guest.spawn() is not None)
    if not host.me or not guest.me:
        return
    h, g, gname = host.me["uid"], guest.me["uid"], guest.me["name"]
    pump_all([host, guest], 0.5)

    m_h, m_g = host.mark(), guest.mark()
    host.say("/tp 10 20.5 -30")
    pump_all([host, guest], 0.3)
    check("tp x y z: POSITION to the host", host.moved(None, (10.0, 20.5, -30.0), m_h))
    check("tp x y z: CHANGE_POSITION to the guest", guest.moved(h, (10.0, 20.5, -30.0), m_g))
    check("tp x y z reply", host.got_text("Teleported to 10.00 20.50 -30.00.", m_h))

    m_h, m_g = host.mark(), guest.mark()
    host.say(f"/bring {gname.lower()} 5 60 7.25")
    pump_all([host, guest], 0.3)
    check("bring player x y z: POSITION to the guest", guest.moved(None, (5.0, 60.0, 7.25), m_g))
    check("bring player x y z: CHANGE_POSITION to the host", host.moved(g, (5.0, 60.0, 7.25), m_h))
    check("bring reply", host.got_text(f"Brought {gname} to 5.00 60.00 7.25.", m_h))

    m_g = guest.mark()
    host.say(f"/tp {gname}")
    pump_all([host, guest], 0.3)
    check("tp player: the host goes to the guest", guest.moved(h, (5.0, 60.0, 7.25), m_g))

    m_h, m_g = host.mark(), guest.mark()
    host.say("/tp 1 2 3")
    host.say("/bring all")
    pump_all([host, guest], 0.3)
    check("bring all: the guest comes to the host", guest.moved(None, (1.0, 2.0, 3.0), m_g) and host.moved(g, (1.0, 2.0, 3.0), m_h))
    check("bring all reply", host.got_text("Brought 1 player to you.", m_h))

    m_h = host.mark()
    host.say("/bring nobody")
    check("unknown name", host.got_text("Player 'nobody' not found.", m_h))

    m_h, m_g = host.mark(), guest.mark()
    guest.say("/tp 100 100 100")
    guest.say(f"/bring {host.me['name']} 0 0 0")
    pump_all([host, guest], 0.3)
    check("guest tp / bring refused", guest.got_text("Host or developer required", m_g))
    check("guest tp / bring: nobody moved", not any(t in (POSITION, CHANGE_POSITION) for t, _ in host.packets[m_h:] + guest.packets[m_g:]))

    m_h, m_g = host.mark(), guest.mark()
    rc, out = rcon("bring", "all", "0", "50", "0")
    pump_all([host, guest], 0.5)
    check("rcon bring all x y z", "Brought 2 players to 0.00 50.00 0.00." in out, out.strip()[-200:])
    check("rcon bring all moves both", host.moved(None, (0.0, 50.0, 0.0), m_h) and guest.moved(None, (0.0, 50.0, 0.0), m_g)
          and host.moved(g, (0.0, 50.0, 0.0), m_h) and guest.moved(h, (0.0, 50.0, 0.0), m_g))
    rc, out = rcon("tp", "1", "2", "3")
    check("rcon tp refused", "tp moves your own player" in out, out.strip()[-200:])
    check("nobody dropped", not host.closed and not guest.closed)
    host.close()
    guest.close()


def main():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    idle_after_map_change()
    tp_bring()


try:
    main()
except Exception as e:  # noqa: BLE001
    check("exception", False, repr(e))
finally:
    for p in procs:
        p.terminate()
    for p in procs:
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            p.kill()
    if not all(results):
        for i in range(2):
            path = os.path.join(WORK, f"s{i}.log")
            if os.path.exists(path):
                print(f"--- s{i}.log (tail)")
                print(open(path).read()[-3000:])
print(f"{sum(results)}/{len(results)} passed", flush=True)
sys.exit(0 if results and all(results) else 1)

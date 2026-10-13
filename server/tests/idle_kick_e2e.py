#!/usr/bin/env python3
"""The server's idle kick (server/src/net.c net_check_idle_clients) against real play patterns.

  python3 -I server/tests/idle_kick_e2e.py BUILD_DIR      (run from the repository root)

One server_bin (no database or coordinator) with idle_kick_seconds 4 and idle_menu_kick_seconds
8; every peer is a bridged remote browser (NET_PACKET_FORWARDED), so none is DEV or host.
Over ~14 s of wall time:
  shooter   alive, never moves: holds fire on and off and turns its view       -> never kicked
  strafer   alive, strafes left / right in place (no net 1-unit move)          -> never kicked
  looker    alive, only looks around (mouse)                                   -> never kicked
  respawner stands still, /kill, waits on the death screen, respawns, again    -> never kicked
  chatter   alive, stands still, says something every 2 s                      -> never kicked
  menu      /kill right after spawning, then sits in the menu                  -> kicked 8 s after the death
  afk       alive, its client keeps sending empty input frames (same look)     -> kicked 4 s after the spawn
  silent    alive, sends nothing at all after the spawn                        -> kicked 4 s after the spawn
Prints when each one is kicked, PASS/FAIL lines; exit code 0 only when everything passed.
"""
import math
import os
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
(PORT,) = kn_proto.free_ports(1)  # ephemeral: concurrent runs must not share servers
WORK = tempfile.mkdtemp(prefix="kn-idle-kick-e2e-")
IDLE, MENU_IDLE = 4.0, 8.0
SPAWN, INIT, CHAT_SEND, ERROR, FORWARDED, INPUT_BATCH = 4, 2, 8, 30, 85, 111
INPUT_FMT = "<iifffBBBb"  # NetInputPacket
SHOOT = 1
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def query():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", PORT))
        return s.recvfrom(2048)[0]


class Peer(kn_proto.Conn):
    def __init__(self, name, index):
        super().__init__("127.0.0.1", PORT)
        self.name = name
        self.closed_at = None
        self.kicked_at = None
        self.me = None
        self.seq = 1
        self.yaw = 0.0
        self.s.setblocking(False)
        self.send(FORWARDED, f"203.0.113.{10 + index}".encode() + b"\0")
        self.send(kn_proto.HELLO, bytes([1, 3, 0]))
        self.join()

    def drain(self):
        """Non-blocking: reads whatever arrived, records SPAWN (own), the idle ERROR and the close."""
        if self.closed_at is not None:
            return
        try:
            while True:
                data = self.s.recv(65536)
                if not data:
                    self.closed_at = time.time()
                    break
                self.buf += data
        except (BlockingIOError, InterruptedError):
            pass
        except OSError:
            self.closed_at = time.time()
        while len(self.buf) >= 3:
            t, n = struct.unpack(">BH", self.buf[:3])
            if len(self.buf) < 3 + n:
                break
            payload, self.buf = self.buf[3:3 + n], self.buf[3 + n:]
            self.packets.append((t, payload))
            if t == SPAWN:
                info = kn_proto.spawn_info(payload)
                if info and info["is_you"]:
                    self.me = (time.time(), info)
            elif t == ERROR and kn_proto.error_info(payload)[1] == "000005" and self.kicked_at is None:
                self.kicked_at = time.time()

    def alive(self):
        return self.closed_at is None and self.kicked_at is None

    def send_frames(self, move_dir=-1, flags=0, dyaw=0.0, count=2):
        frames = []
        for _ in range(count):
            self.yaw = math.remainder(self.yaw + dyaw, 2 * math.pi)
            frames.append(struct.pack(INPUT_FMT, self.seq, move_dir, 1 / 60, 0.0, self.yaw, flags, 0, 0, 0))
            self.seq += 1
        try:
            self.send(INPUT_BATCH, bytes([len(frames)]) + b"".join(frames))
        except OSError:
            pass

    def say(self, text):
        try:
            self.send(CHAT_SEND, text.encode() + b"\0")
        except OSError:
            pass


def start_server():
    cfg = os.path.join(WORK, "s.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Idle kick"\nport {PORT}\nregion local\nmaxplayers 12\nmapcycle Sandstorm\nmode ffa\n'
                f"round_seconds 600\ncheats all\nidle_kick_seconds {IDLE:g}\nidle_menu_kick_seconds {MENU_IDLE:g}\n")
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    out = open(os.path.join(WORK, "s.log"), "w")
    return subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                            stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)


def main():
    proc = start_server()
    end = time.time() + 15
    while time.time() < end:
        try:
            if query():
                break
        except OSError:
            time.sleep(0.2)
    names = ["shooter", "strafer", "looker", "respawner", "chatter", "menu", "afk", "silent"]
    peers = {n: Peer(n, i) for i, n in enumerate(names)}
    for p in peers.values():
        p.ent(class_index=0, lag_comp=1.0)
    t0 = time.time()
    while time.time() - t0 < 5 and not all(p.me for p in peers.values()):
        for p in peers.values():
            p.drain()
        time.sleep(0.02)
    check("everyone spawned", all(p.me for p in peers.values()), ", ".join(n for n, p in peers.items() if not p.me))
    start = time.time()
    peers["menu"].say("/kill")
    died_at = {"menu": start}
    respawn_next = start + 1.0  # the respawner's cycle: /kill, 3 s on the death screen, ENT
    respawner_dead = False
    respawns = 0
    chat_next = start + 2.0
    tick = 0
    while time.time() - start < 14.0:
        now = time.time()
        tick += 1
        for p in peers.values():
            p.drain()
        if peers["shooter"].alive():
            peers["shooter"].send_frames(flags=SHOOT if (tick // 20) % 2 == 0 else 0, dyaw=0.002)
        if peers["strafer"].alive():
            peers["strafer"].send_frames(move_dir=2 if tick % 2 else 6, count=1)
        if peers["looker"].alive():
            peers["looker"].send_frames(dyaw=0.01 * math.sin(tick / 10))
        if peers["afk"].alive():
            peers["afk"].send_frames()
        r = peers["respawner"]
        if r.alive():
            if not respawner_dead and now >= respawn_next:
                r.say("/kill")
                respawner_dead = True
                respawn_next = now + 3.0
            elif respawner_dead and now >= respawn_next:
                r.ent(class_index=0, lag_comp=1.0)
                respawner_dead = False
                respawns += 1
                respawn_next = now + 1.0
            elif not respawner_dead:
                r.send_frames()
        c = peers["chatter"]
        if c.alive():
            c.send_frames()
            if now >= chat_next:
                c.say(f"still here {tick}")
                chat_next = now + 2.0
        time.sleep(1 / 30)
    for n, p in peers.items():
        at = p.kicked_at or p.closed_at
        print(f"      {n:9s} {'kicked %.1f s after the start' % (at - start) if at else 'still connected'}", flush=True)
    for n in ["shooter", "strafer", "looker", "respawner", "chatter"]:
        p = peers[n]
        check(f"{n}: not kicked", p.alive(), f"kicked {((p.kicked_at or p.closed_at) - start):.1f} s in" if not p.alive() else "")
    check("respawner died and respawned (>= 2 cycles)", respawns >= 2, f"{respawns}")

    def kicked_near(n, expect, slack=1.6):
        p = peers[n]
        at = p.kicked_at
        check(f"{n}: idle-kicked (000005) ~{expect:g} s in", at is not None and abs(at - start - expect) <= slack,
              f"at {at - start:.1f} s" if at else f"not kicked (closed={p.closed_at is not None})")

    kicked_near("menu", MENU_IDLE)
    kicked_near("afk", IDLE)
    kicked_near("silent", IDLE)
    for p in peers.values():
        p.close()
    proc.terminate()
    proc.wait(5)


try:
    main()
except Exception as e:  # noqa: BLE001
    check("exception", False, repr(e))
if not all(results):
    path = os.path.join(WORK, "s.log")
    if os.path.exists(path):
        print("--- s.log (tail)")
        print(open(path).read()[-3000:])
print(f"{sum(results)}/{len(results)} passed", flush=True)
sys.exit(0 if results and all(results) else 1)

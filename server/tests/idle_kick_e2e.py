#!/usr/bin/env python3
"""The server's idle kick (server/src/net.c net_check_idle_clients) against real play patterns.

  python3 -I server/tests/idle_kick_e2e.py BUILD_DIR      (run from the repository root)

server_bin (no database or coordinator) on server/tests/arena.json with idle_kick_seconds 4 and
idle_menu_kick_seconds 8; every peer is a bridged remote browser (NET_PACKET_FORWARDED), so none
is DEV or host. Phase 1, ~14 s of wall time:
  shooter    alive, never moves: taps fire on and off and turns its view         -> never kicked
  strafer    alive, strafes left / right in place (no net 1-unit move)          -> never kicked
  looker     alive, only looks around (mouse)                                   -> never kicked
  respawner  stands still, /kill, waits on the death screen, respawns, again    -> never kicked
  chatter    alive, stands still, says something every 2 s                      -> never kicked
  holder     alive, holds fire the whole time, mouse perfectly still            -> never kicked
  aimer      alive, holds aim (ADS) the whole time, mouse still                 -> never kicked
  croucher   alive, holds crouch the whole time, mouse still                    -> never kicked
  tracker    alive, no key, turns 3e-5 rad a frame (low sensitivity, scoped)    -> never kicked
  botshooter alive, stands still and holds fire on a (stopped) bot in front     -> never kicked, hits it
  menu       /kill right after spawning, then sits in the menu                  -> kicked 8 s after the death
  afk        alive, its client keeps sending empty input frames (same look)     -> kicked 4 s after the spawn
  silent     alive, sends nothing at all after the spawn                        -> kicked 4 s after the spawn
Phase 2: the server is stopped (SIGTERM, a deploy) and a fresh server_bin starts on the same port;
the players rejoin it at once (the client's auto-rejoin) and keep playing for ~10 s:
  rejoin_hold  keeps its old input seq, holds fire + aim, mouse still           -> never kicked
  rejoin_play  input seq from 0 again, taps fire and turns                      -> never kicked
  rejoin_seq   keeps its old input seq, taps fire and turns                     -> never kicked
  rejoin_afk   empty input frames                                               -> kicked 4 s after the spawn
Phase 3: console `map Sandstorm` (a map change respawns everyone alive); rejoin_hold and rejoin_play
carry on for 9 s                                                                -> never kicked
Prints when each one is kicked, PASS/FAIL lines; exit code 0 only when everything passed.
"""
import math
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
(PORT,) = kn_proto.free_ports(1)  # ephemeral: concurrent runs must not share servers
WORK = tempfile.mkdtemp(prefix="kn-idle-kick-e2e-")
IDLE, MENU_IDLE = 4.0, 8.0
SPAWN, INIT, CHAT_SEND, HIT, ERROR, FORWARDED, INPUT_BATCH = 4, 2, 8, 11, 30, 85, 111
INPUT_FMT = "<iifffBBBb"  # NetInputPacket
SPAWN_FMT = "<iBBiifi3f2f32s"  # NetSpawnPacket's head: uid, is_you, active, class, team, hp, max, pos, dir, name
SHOOT, SCOPE, CROUCH = 1, 2, 8  # NET_INPUT_SHOOT / _SCOPE / _CROUCH
SKY = 1.2  # pitch of the peers that hold fire without a target: into the sky, never at another peer
results = []
procs = []
peer_index = [0]


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def query():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", PORT))
        return s.recvfrom(2048)[0]


def spawn_head(payload):
    if len(payload) < struct.calcsize(SPAWN_FMT):
        return None
    v = struct.unpack(SPAWN_FMT, payload[:struct.calcsize(SPAWN_FMT)])
    return {"uid": v[0], "is_you": v[1], "active": v[2], "pos": v[7:10], "name": kn_proto.uncstr(v[12])}


class Peer(kn_proto.Conn):
    def __init__(self, name, seq=1):
        super().__init__("127.0.0.1", PORT)
        self.name = name
        self.closed_at = None
        self.kicked_at = None
        self.me = None
        self.others = {}  # uid -> spawn head of everyone else (the bot)
        self.hits = 0
        self.seq = seq
        self.yaw = 0.0
        self.pitch = 0.0
        self.s.setblocking(False)
        peer_index[0] += 1
        self.send(FORWARDED, f"203.0.113.{10 + peer_index[0]}".encode() + b"\0")
        self.send(kn_proto.HELLO, bytes([1, 3, 0]))
        self.join(name=name)

    def drain(self):
        """Non-blocking: reads whatever arrived, records SPAWN, HIT, the idle ERROR and the close."""
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
            if t == SPAWN:
                info = spawn_head(payload)
                if info and info["is_you"]:
                    # the join's own SPAWN is dead (active 0, waiting for the ENT): only an active one is a spawn
                    if info["active"]:
                        self.me = (time.time(), info)
                elif info:
                    self.others[info["uid"]] = info
            elif t == HIT and len(payload) >= 8:
                self.hits += 1
            elif t == ERROR and kn_proto.error_info(payload)[1] == "000005" and self.kicked_at is None:
                self.kicked_at = time.time()

    def alive(self):
        return self.closed_at is None and self.kicked_at is None

    def send_frames(self, move_dir=-1, flags=0, dyaw=0.0, count=2):
        frames = []
        for _ in range(count):
            self.yaw = math.remainder(self.yaw + dyaw, 2 * math.pi)
            frames.append(struct.pack(INPUT_FMT, self.seq, move_dir, 1 / 60, self.pitch, self.yaw, flags, 0, 0, 0))
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


def start_server(tag):
    cfg = os.path.join(WORK, "s.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Idle kick"\nport {PORT}\nregion local\nmaxplayers 20\nmode ffa\n'
                f"round_seconds 600\ncheats all\nidle_kick_seconds {IDLE:g}\nidle_menu_kick_seconds {MENU_IDLE:g}\n")
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    out = open(os.path.join(WORK, f"s{tag}.log"), "w")
    proc = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json"),
                             "server/tests/arena.json"], stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.PIPE, text=True)
    procs.append(proc)
    end = time.time() + 15
    while time.time() < end:
        try:
            if query():
                break
        except OSError:
            time.sleep(0.1)
    return proc


def console(proc, line):
    proc.stdin.write(line + "\n")
    proc.stdin.flush()


def spawn_all(peers, timeout=5.0):
    for p in peers.values():
        p.ent(class_index=0, lag_comp=1.0)
    t0 = time.time()
    while time.time() - t0 < timeout and not all(p.me for p in peers.values()):
        for p in peers.values():
            p.drain()
        time.sleep(0.02)
    return [n for n, p in peers.items() if not p.me]


def kicked_near(peers, start, n, expect, slack=1.6):
    p = peers[n]
    at = p.kicked_at
    check(f"{n}: idle-kicked (000005) ~{expect:g} s in", at is not None and abs(at - start - expect) <= slack,
          f"at {at - start:.1f} s" if at else f"not kicked (closed={p.closed_at is not None})")


def report(peers, start, stay):
    for n, p in peers.items():
        at = p.kicked_at or p.closed_at
        print(f"      {n:11s} {'kicked %.1f s after the start' % (at - start) if at else 'still connected'}", flush=True)
    for n in stay:
        p = peers[n]
        check(f"{n}: not kicked", p.alive(), f"kicked {((p.kicked_at or p.closed_at) - start):.1f} s in" if not p.alive() else "")


def play_tapper(p, tick):
    """The pattern 11d36be already counted: taps fire on and off, turns slowly."""
    p.send_frames(flags=SHOOT if (tick // 20) % 2 == 0 else 0, dyaw=0.002)


def phase1(proc):
    names = ["shooter", "strafer", "looker", "respawner", "chatter", "holder", "aimer", "croucher", "tracker", "botshooter",
             "menu", "afk", "silent"]
    peers = {n: Peer(n) for n in names}
    for n in ["holder", "aimer", "croucher", "tracker"]:
        peers[n].pitch = SKY
    missing = spawn_all(peers)
    check("phase 1: everyone spawned", not missing, ", ".join(missing))
    # A stopped bot (no attack, no roam) 8 units in front of the botshooter, both away from the
    # arena's spawns; botshooter looks along -z (yaw 0), level.
    console(proc, "bot_stop on")
    console(proc, "bot_add 1")
    me = peers["botshooter"].me[1] if peers["botshooter"].me else {"pos": (0, 2.0, 0), "name": "?"}
    y = me["pos"][1]
    console(proc, f"bring {me['name']} 120 {y:g} 120")  # guests are Guest_<n>: the SPAWN names it
    bot_name = None
    t0 = time.time()
    while time.time() - t0 < 3 and bot_name is None:
        peers["botshooter"].drain()
        bots = [o["name"] for o in peers["botshooter"].others.values() if not o["name"].startswith("Guest_")]
        bot_name = bots[0] if bots else None
        time.sleep(0.05)
    check("a bot spawned", bot_name is not None)
    time.sleep(0.3)  # the bring is applied before botshooter's first shot
    bring_next = 0.0

    start = time.time()
    peers["menu"].say("/kill")
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
        if bot_name is not None and now >= bring_next:  # the bot dies under fire: bring it back each time
            console(proc, f"bring {bot_name} 120 {y:g} 112")
            bring_next = now + 0.5
        if peers["shooter"].alive():
            play_tapper(peers["shooter"], tick)
        if peers["strafer"].alive():
            peers["strafer"].send_frames(move_dir=2 if tick % 2 else 6, count=1)
        if peers["looker"].alive():
            peers["looker"].send_frames(dyaw=0.01 * math.sin(tick / 10))
        for n, flags in (("holder", SHOOT), ("aimer", SCOPE), ("croucher", CROUCH), ("botshooter", SHOOT)):
            if peers[n].alive():
                peers[n].send_frames(flags=flags)
        if peers["tracker"].alive():
            peers["tracker"].send_frames(dyaw=3e-5)
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
    report(peers, start, ["shooter", "strafer", "looker", "respawner", "chatter", "holder", "aimer", "croucher", "tracker",
                          "botshooter"])
    check("respawner died and respawned (>= 2 cycles)", respawns >= 2, f"{respawns}")
    check("botshooter hit the bot again and again (>= 10 HITs)", peers["botshooter"].hits >= 10, f"{peers['botshooter'].hits}")
    kicked_near(peers, start, "menu", MENU_IDLE)
    kicked_near(peers, start, "afk", IDLE)
    kicked_near(peers, start, "silent", IDLE)
    return peers


def phase2(old_peers, proc):
    # The deploy: SIGTERM, a fresh process on the same port, everyone back at once.
    seqs = {n: old_peers[n].seq for n in ("shooter", "holder")}
    proc.terminate()
    proc.wait(10)
    for p in old_peers.values():
        p.close()
    proc = start_server(2)
    restarted = time.time()
    peers = {
        "rejoin_hold": Peer("rejoin_hold", seq=seqs["holder"]),
        "rejoin_play": Peer("rejoin_play", seq=1),
        "rejoin_seq": Peer("rejoin_seq", seq=seqs["shooter"]),
        "rejoin_afk": Peer("rejoin_afk"),
    }
    missing = spawn_all(peers)
    check(f"phase 2: everyone rejoined the restarted server ({time.time() - restarted:.1f} s after it answered)", not missing,
          ", ".join(missing))
    peers["rejoin_hold"].pitch = SKY
    start = time.time()
    tick = 0
    while time.time() - start < 10.0:
        tick += 1
        for p in peers.values():
            p.drain()
        if peers["rejoin_hold"].alive():
            peers["rejoin_hold"].send_frames(flags=SHOOT | SCOPE)
        for n in ("rejoin_play", "rejoin_seq"):
            if peers[n].alive():
                play_tapper(peers[n], tick)
        if peers["rejoin_afk"].alive():
            peers["rejoin_afk"].send_frames()
        time.sleep(1 / 30)
    report(peers, start, ["rejoin_hold", "rejoin_play", "rejoin_seq"])
    kicked_near(peers, start, "rejoin_afk", IDLE)

    # Phase 3: a map change (console `map`, markPlayerUnspawned + the respawn of everyone alive);
    # the players keep doing what they did, the input seq goes on.
    for p in peers.values():
        p.me = None
    console(proc, "map Sandstorm")
    t0 = time.time()
    while time.time() - t0 < 5 and not all(peers[n].me for n in ("rejoin_hold", "rejoin_play")):
        for p in peers.values():
            p.drain()
        time.sleep(0.02)
    check("phase 3: the map change respawned the players", all(peers[n].me for n in ("rejoin_hold", "rejoin_play")))
    start = time.time()
    tick = 0
    while time.time() - start < 2 * IDLE + 1:
        tick += 1
        for p in peers.values():
            p.drain()
        if peers["rejoin_hold"].alive():
            peers["rejoin_hold"].send_frames(flags=SHOOT | SCOPE)
        if peers["rejoin_play"].alive():
            play_tapper(peers["rejoin_play"], tick)
        time.sleep(1 / 30)
    for n in ("rejoin_hold", "rejoin_play"):
        p = peers[n]
        check(f"{n}: not kicked after the map change", p.alive(),
              f"kicked {((p.kicked_at or p.closed_at) - start):.1f} s in" if not p.alive() else "")
    for p in peers.values():
        p.close()
    return proc


def main():
    proc = start_server(1)
    peers = phase1(proc)
    proc = phase2(peers, proc)
    proc.terminate()
    proc.wait(10)


try:
    main()
except Exception as e:  # noqa: BLE001
    check("exception", False, repr(e))
finally:
    for p in procs:
        if p.poll() is None:
            p.kill()
            p.wait()
if not all(results):
    for tag in (1, 2):
        path = os.path.join(WORK, f"s{tag}.log")
        if os.path.exists(path):
            print(f"--- s{tag}.log (tail)")
            print(open(path).read()[-3000:])
print(f"{sum(results)}/{len(results)} passed", flush=True)
sys.exit(0 if results and all(results) else 1)

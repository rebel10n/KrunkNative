#!/usr/bin/env python3
"""How fast server.cfg bot_quota / chat `autofill` fill a lobby (shared/src/bots.c bot_quota_tick,
doc/console.md "Bots").

  python3 -I server/tests/bot_fill_time_e2e.py BUILD_DIR      (run from the repository root)

Standalone server_bin instances with real kn_proto clients; every bot's first SPAWN on the human's
connection is timed from the human's own SPAWN:
  1. FFA, maxplayers 16, bot_quota 16: one human -> 15 bots within FILL_LIMIT s, each on its own
     spawn (no two bodies on one spot), each with its PLAYER_INFO, and the leaderboard lists all 16
  2. a second human: a bot leaves at once; it leaves again: the bot is back within FILL_LIMIT s
  3. TDM, maxplayers 8, bot_quota 8: 7 bots within FILL_LIMIT s, teams 4 v 4
  4. no bot_quota, chat `/autofill 12` (the server's own machine is a DEV peer): 11 bots within
     FILL_LIMIT s
Prints every bot's join time, PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
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
FILL_LIMIT = 2.0  # s from the human's spawn to a full lobby
WORK = tempfile.mkdtemp(prefix="kn-bot-fill-time-e2e-")
SPAWN, LEAVE, LEADERBOARD, PLAYER_INFO = kn_proto.SPAWN, 20, 14, 34
SPAWN_FMT = "<iBBiifi3f2f32s"
results = []
procs = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


class Server:
    def __init__(self, tag, extra):
        (self.port,) = kn_proto.free_ports(1)
        cfg = os.path.join(WORK, f"{tag}.cfg")
        with open(cfg, "w") as f:
            f.write(f'hostname "Bot fill time"\nport {self.port}\nregion local\nmapcycle Sandstorm\n'
                    f"round_seconds 600\nbot_difficulty easy\n{extra}")
        none = os.path.join(WORK, "none.json")
        with open(none, "w") as f:
            f.write("{}")
        self.log_path = os.path.join(WORK, f"{tag}.log")
        self.proc = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", none],
                                     stdout=open(self.log_path, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        procs.append(self)
        end = time.time() + 15
        while time.time() < end and self.counts() == (None, None):
            time.sleep(0.2)

    def query(self):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.settimeout(1.0)
            s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", self.port))
            data, _ = s.recvfrom(2048)
        return json.loads(data[11:])

    def counts(self):
        try:
            info = self.query()
            return info.get("players"), info.get("bots")
        except OSError:
            return None, None

    def wait_counts(self, players, bots, timeout):
        end = time.time() + timeout
        last = (None, None)
        while time.time() < end:
            last = self.counts()
            if last == (players, bots):
                return True, last
            time.sleep(0.05)
        return False, last

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def spawn_of(payload):
    if len(payload) < struct.calcsize(SPAWN_FMT):
        return None
    v = struct.unpack(SPAWN_FMT, payload[: struct.calcsize(SPAWN_FMT)])
    return {"uid": v[0], "is_you": v[1], "team": v[4], "pos": v[7:10], "name": kn_proto.uncstr(v[12])}


class Human:
    """A guest with a timeline: (t, packet type, payload) for every packet read after its JOIN."""

    def __init__(self, server, name):
        self.c = kn_proto.Conn("127.0.0.1", server.port)
        self.c.hello()
        self.c.join(name=name)
        self.c.ent(class_index=0, lag_comp=1.0)
        self.timeline = []
        self.me = None
        self.t0 = None
        end = time.time() + 8
        while time.time() < end and self.me is None:
            self.pump(0.2)

    def pump(self, seconds):
        """Read for `seconds`, stamping every packet with its arrival time."""
        end = time.time() + seconds
        while time.time() < end:
            n = len(self.c.packets)
            self.c.read(timeout=min(0.05, max(0.01, end - time.time())))
            now = time.time()
            for t, payload in self.c.packets[n:]:
                self.timeline.append((now, t, payload))
                if t == SPAWN and self.me is None:
                    s = spawn_of(payload)
                    if s and s["is_you"]:
                        self.me, self.t0 = s, now

    def bot_spawns(self, since=0.0):
        """{uid: (t from since, spawn)}: the first SPAWN of each other player who had none before `since`
        (a join, not a respawn)."""
        out, before = {}, set()
        for t, ptype, payload in self.timeline:
            if ptype != SPAWN:
                continue
            s = spawn_of(payload)
            if not s or s["is_you"]:
                continue
            if t < since:
                before.add(s["uid"])
            elif s["uid"] not in out and s["uid"] not in before:
                out[s["uid"]] = (t - since, s)
        return out

    def say(self, text):
        self.pump(0.4)  # the chat burst limit
        self.c.send(8, text.encode() + b"\0")  # NET_PACKET_CHAT_SEND

    def close(self):
        self.c.close()


def last_leaderboard(h):
    for _, ptype, payload in reversed(h.timeline):
        if ptype == LEADERBOARD and payload:
            n = payload[0]
            return [struct.unpack_from("<i", payload, 1 + 16 * i)[0] for i in range(n)]
    return []


def info_uids(h):
    return {struct.unpack_from("<i", p, 0)[0] for _, t, p in h.timeline if t == PLAYER_INFO and len(p) >= 4}


def wait_fill(h, server, players, bots, since):
    """Pump the human until the server has (players, bots) and as many bot SPAWNs arrived."""
    end = since + FILL_LIMIT + 4.0
    while time.time() < end:
        h.pump(0.05)
        if len(h.bot_spawns(since)) >= bots and server.counts() == (players, bots):
            break
    h.pump(0.5)  # the leaderboard / info packets after the last spawn
    return h.bot_spawns(since)


def report(label, spawns):
    times = sorted(t for t, _ in spawns.values())
    print(f"  {label}: {len(times)} bots joined at " + " ".join(f"{t:.2f}" for t in times) + " s", flush=True)
    return times[-1] if times else math.inf


def overlaps(spawns, me):
    """Pairs of bodies put on one spot by the same burst (spawns under 0.1 s apart; the human stands still)."""
    pts = [(t, s["pos"]) for t, s in spawns.values()] + [(None, me["pos"])]
    bad = 0
    for i in range(len(pts)):
        for j in range(i + 1, len(pts)):
            (ti, a), (tj, b) = pts[i], pts[j]
            if ti is not None and tj is not None and abs(ti - tj) >= 0.1:
                continue
            dx, dy, dz = (a[k] - b[k] for k in range(3))
            bad += dx * dx + dz * dz < 1.0 and abs(dy) < 5.0
    return bad


def main():
    # 1. FFA 16
    s = Server("ffa", "maxplayers 16\nmode ffa\nbot_quota 16\nbot_quota_grace 10\n")
    check("ffa server up", s.counts() == (0, 0), str(s.counts()))
    time.sleep(1.5)  # an idle lobby, past any round-start hold
    a = Human(s, "Alpha")
    check("ffa: the human spawns", a.me is not None)
    spawns = wait_fill(a, s, 1, 15, a.t0 or time.time())
    last = report("ffa 16, 1 human", spawns)
    check(f"ffa: 15 bots within {FILL_LIMIT:.1f} s of the human's spawn", len(spawns) == 15 and last <= FILL_LIMIT,
          f"{len(spawns)} bots, last at {last:.2f} s, query {s.counts()}")
    check("  every bot on its own spot", overlaps(spawns, a.me) == 0, f"{overlaps(spawns, a.me)} overlapping pairs")
    missing = set(spawns) - info_uids(a)
    check("  every bot has its PLAYER_INFO (scoreboard row)", not missing, f"missing {sorted(missing)}")
    board = last_leaderboard(a)
    want = set(spawns) | {a.me["uid"]}
    check("  the leaderboard lists all 16 players", len(board) == 16 and set(board) == want, f"{len(board)} rows")

    # 2. a join takes a bot at once; its leave brings it back
    b = Human(s, "Bravo")
    ok, last_counts = s.wait_counts(2, 14, 0.5)
    check("ffa: a second human takes a bot's slot at once", b.me is not None and ok, str(last_counts))
    a.pump(0.3)  # Bravo's own SPAWN on Alpha's connection, before the leave's clock starts
    b.close()
    t_leave = time.time()
    back = {uid: v for uid, v in wait_fill(a, s, 1, 1, t_leave).items() if uid != b.me["uid"]}
    took = report("ffa 16, after a leave", back)
    check(f"ffa: the leave's bot is back within {FILL_LIMIT:.1f} s", len(back) == 1 and s.counts() == (1, 15) and took <= FILL_LIMIT,
          f"{len(back)} new bots, {s.counts()} in {took:.2f} s")
    a.close()
    s.stop()

    # 3. TDM 8: balanced teams
    s = Server("tdm", "maxplayers 8\nmode tdm\nbot_quota 8\nbot_quota_grace 10\n")
    check("tdm server up", s.counts() == (0, 0), str(s.counts()))
    time.sleep(1.5)
    a = Human(s, "Alpha")
    spawns = wait_fill(a, s, 1, 7, a.t0 or time.time())
    last = report("tdm 8, 1 human", spawns)
    check(f"tdm: 7 bots within {FILL_LIMIT:.1f} s", len(spawns) == 7 and last <= FILL_LIMIT, f"{len(spawns)} bots, last at {last:.2f} s")
    teams = [sp["team"] for _, sp in spawns.values()] + [a.me["team"] if a.me else 0]
    check("  teams 4 v 4", teams.count(1) == 4 and teams.count(2) == 4, f"team 1: {teams.count(1)}, team 2: {teams.count(2)}")
    check("  every bot on its own spot", a.me is not None and overlaps(spawns, a.me) == 0)
    a.close()
    s.stop()

    # 4. chat autofill on a server without bot_quota
    s = Server("auto", "maxplayers 12\nmode ffa\n")
    check("autofill server up", s.counts() == (0, 0), str(s.counts()))
    a = Human(s, "Alpha")
    a.pump(1.0)
    check("autofill: no bots before the command", s.counts() == (1, 0), str(s.counts()))
    a.say("/autofill 12")
    t_cmd = time.time()
    spawns = wait_fill(a, s, 1, 11, t_cmd)
    last = report("autofill 12, 1 human", spawns)
    check(f"autofill: 11 bots within {FILL_LIMIT:.1f} s of the command", len(spawns) == 11 and last <= FILL_LIMIT,
          f"{len(spawns)} bots, last at {last:.2f} s")
    a.close()
    s.stop()


try:
    main()
except Exception as e:  # noqa: BLE001
    check("exception", False, repr(e))
finally:
    for p in procs:
        p.stop()
    if not all(results):
        for p in procs:
            if os.path.exists(p.log_path):
                print(f"--- {os.path.basename(p.log_path)} (tail)")
                print(open(p.log_path).read()[-2000:])
print(f"{sum(results)}/{len(results)} passed", flush=True)
sys.exit(0 if results and all(results) else 1)

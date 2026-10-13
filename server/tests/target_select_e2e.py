#!/usr/bin/env python3
"""Player target selectors (shared/src/target_select.c) on server_bin: god * / ungod * / give * with
several clients and bots, every player affected, and the permission case.

  python3 -I server/tests/target_select_e2e.py BUILD_DIR      (run from the repository root)

No database or coordinator: one standalone server_bin (perms on) from a generated server.cfg. The
host connects over loopback (the server's own machine: a DEV peer); two guests write a
NET_PACKET_FORWARDED first (bridged remote browsers), so they are neither DEV nor host. rcon adds
3 bots and stops them (bot_stop), so nobody dies during the test.
  1. host /god *: "God mode ON: 6 players", a TEAM refill for every uid; rcon `god <uid>` per player
     answers OFF (it was on) and turns it back on
  2. host /ungod *: "God mode OFF: 6 players"; per player `god <uid>` answers ON (it was off)
  3. host /give * crossbow: "Gave Crossbow: 6 players", LOADOUT_EXTRA with the crossbow for every uid
  4. a guest's god * / ungod * / kick * / give * are refused (notify / reply), nobody changes or leaves
  5. host /kick @bots removes the 3 bots, the guests stay; rcon god * counts the 3 humans
  6. the server CLI over rcon: op @humans, deop *, kick * (every connection)
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
RCON_PORT, PORT = kn_proto.free_ports(2)  # ephemeral: concurrent runs must not share servers
WORK = tempfile.mkdtemp(prefix="kn-target-select-e2e-")
SPAWN, CHAT_SEND, CHAT, ERROR, TEAM, LEAVE = 4, 8, 9, 30, 42, 20
NOTIFY, FORWARDED, LOADOUT_EXTRA = 82, 85, 252
CROSSBOW = 13
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


class Peer(kn_proto.Conn):
    """A v7 (1.3.0) peer. `closed` once the server hung up."""

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
        mark = len(self.packets)
        self.send(CHAT_SEND, text.encode() + b"\0")
        self.pump(0.45)  # the chat burst limit is 3 lines per second
        return mark

    def got_text(self, text, since=0):
        needle = text.encode()
        return any(t in (CHAT, NOTIFY) and needle in p for t, p in self.packets[since:])

    def uids(self):
        """Every player uid this peer saw spawn, minus the ones that left."""
        seen = set()
        for t, p in self.packets:
            if t == SPAWN:
                info = kn_proto.spawn_info(p)
                if info:
                    seen.add(info["uid"])
            if t == LEAVE and len(p) >= 4:
                seen.discard(struct.unpack("<i", p[:4])[0])
        return seen

    def names(self):
        """uid -> name from every SPAWN this peer saw."""
        out = {}
        for t, p in self.packets:
            if t == SPAWN:
                info = kn_proto.spawn_info(p)
                if info:
                    out[info["uid"]] = info["name"]
        return out

    def team_refills(self, since):
        return {struct.unpack("<i", p[:4])[0] for t, p in self.packets[since:] if t == TEAM and len(p) >= 16}

    def extras(self, uid):
        found = None
        for t, p in self.packets:
            if t == LOADOUT_EXTRA and len(p) >= 5:
                u, n = struct.unpack("<iB", p[:5])
                if u == uid:
                    found = list(p[5: 5 + n])
        return found


def pump_all(peers, seconds):
    end = time.time() + seconds
    while time.time() < end:
        for p in peers:
            p.pump(0.1)


NAMES = {}


def god_states(uids):
    """Per uid: the state rcon `god <name>` flipped away from (it answers the new state), flipped back.
    By name: `god 0` / `god 1` read as on / off (resolveDevToggleTarget's toggle literals)."""
    states = {}
    for uid in sorted(uids):
        _, out = rcon("god", NAMES[uid])
        states[uid] = "on" if "God mode OFF on" in out else "off" if "God mode ON on" in out else out.strip()[-120:]
        rcon("god", NAMES[uid])  # back to where it was
    return states


def run():
    cfg = os.path.join(WORK, "s.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Selectors"\nport {PORT}\nregion local\nmaxplayers 12\nmapcycle Sandstorm\nmode ffa\n'
                f"round_seconds 900\nperms 1\nrcon_password rc-pw\nrcon_port {RCON_PORT}\n")
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    out = open(os.path.join(WORK, "s.log"), "w")
    procs.append(subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                                  stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
    check("server up", bool(wait_for(lambda: query(PORT))))

    host = Peer(PORT)
    guests = [Peer(PORT, "203.0.113.21"), Peer(PORT, "203.0.113.22")]
    peers = [host] + guests
    check("3 humans spawned", all(p.spawn() is not None for p in peers))
    rc, o = rcon("bot_stop", "on")
    rc, o = rcon("bot_add", "3")
    check("rcon bot_add 3", "Spawned 3 nextbot bots." in o, o.strip()[-200:])
    pump_all(peers, 1.5)
    uids = host.uids()
    check("the host sees 6 players", len(uids) == 6, str(sorted(uids)))
    humans = {p.me["uid"] for p in peers if p.me}
    NAMES.update(host.names())
    check("unique names", len(set(NAMES[u] for u in uids)) == len(uids), str(NAMES))

    # 1. god *
    mark = host.say("/god *")
    pump_all(peers, 0.3)
    check("god * reply", host.got_text("God mode ON: 6 players", mark))
    refilled = host.team_refills(mark)
    check("god *: a TEAM refill for every player", uids <= refilled, f"missing {sorted(uids - refilled)}")
    states = god_states(uids)
    check("god *: every player is in god mode", all(v == "on" for v in states.values()), str(states))

    # 2. ungod *
    mark = host.say("/ungod *")
    check("ungod * reply", host.got_text("God mode OFF: 6 players", mark))
    states = god_states(uids)
    check("ungod *: nobody is in god mode", all(v == "off" for v in states.values()), str(states))

    # 3. give * crossbow
    mark = host.say("/give * crossbow")
    pump_all(peers, 0.3)
    check("give * crossbow reply", host.got_text("Gave Crossbow: 6 players", mark))
    missing = [u for u in uids if CROSSBOW not in (host.extras(u) or [])]
    check("give *: LOADOUT_EXTRA with the crossbow for every player", not missing, f"missing {missing}")
    check("give *: a guest hears its own extra", CROSSBOW in (guests[0].extras(guests[0].me["uid"]) or []))

    # 4. permissions: a selector never widens a command's gate
    g = guests[0]
    mark = g.say("/god *")
    check("guest god * refused (notify)", g.got_text("DEV or premium custom host only", mark))
    mark = g.say("/ungod *")
    check("guest ungod * refused", g.got_text("DEV or premium custom host only", mark))
    mark = g.say("/give * shotgun")
    check("guest give * refused", g.got_text("Only developers and the host can give to other players.", mark))
    mark = g.say("/kick *")
    check("guest kick * refused (notify)", g.got_text("DEV only", mark))
    pump_all(peers, 0.5)
    states = god_states(uids)
    check("guest selectors: nobody in god mode", all(v == "off" for v in states.values()), str(states))
    check("guest kick *: nobody left", not any(p.closed for p in peers) and host.uids() == uids, str(sorted(host.uids())))

    # 5. kick @bots, then the console counts the humans
    mark = host.say("/kick @bots")
    pump_all(peers, 0.5)
    check("kick @bots reply", host.got_text("Kicked: 3 players", mark))
    check("kick @bots: only the humans remain", host.uids() == humans and not any(p.closed for p in peers),
          f"{sorted(host.uids())} vs {sorted(humans)}")
    rc, o = rcon("god", "*")
    check("rcon god *: the 3 humans", "God mode ON: 3 players" in o, o.strip()[-200:])
    rc, o = rcon("god", "@me")
    check("rcon god @me: no player", "needs your own player" in o, o.strip()[-200:])
    rc, o = rcon("god", "zz*")
    check("rcon: a selector matching nobody says so", "No players match 'zz*'." in o, o.strip()[-200:])
    # the server CLI's own kick / op (server/src/cli.c) over the same resolver
    rc, o = rcon("op", "@humans")
    check("rcon op @humans", "Opped: 3 players" in o, o.strip()[-200:])
    rc, o = rcon("deop", "*")
    check("rcon deop *", "De-opped: 3 players" in o, o.strip()[-200:])
    rc, o = rcon("kick", "zz*")
    check("rcon kick: no match", "No players match 'zz*'." in o, o.strip()[-200:])
    rc, o = rcon("kick", "*")
    pump_all(peers, 1.0)
    check("rcon kick *: every connection", "Kicked: 3 players" in o and all(p.closed for p in peers), o.strip()[-200:])
    for p in peers:
        p.close()


try:
    run()
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
        path = os.path.join(WORK, "s.log")
        if os.path.exists(path):
            print("--- s.log (tail)")
            print(open(path).read()[-3000:])
print(f"{sum(results)}/{len(results)} passed", flush=True)
sys.exit(0 if results and all(results) else 1)

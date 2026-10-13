#!/usr/bin/env python3
"""The Nuke killstreak end to end on a dedicated server_bin (Krunker v7.1.4 streaks.js Nuke,
players.js addStreak / checkStreak, index.js io_nukeUpdate; shared/src/effects.c, shared/src/game.c,
client/src/net/combat_events.c).

  python3 -I server/tests/nuke_e2e.py BUILD_DIR      (run from the repository root)

1. Free for All, through the real client: a headless client_bin joins as a guest (over a non-loopback
   address of this machine, so it is not the server's own machine and has no account, like a live
   player), kills 26 frozen NextBots with the console's `kill @bots` and gets the Nuke in the HUD tray
   (#killStreakHolder, key 3). Pressing 3 sends "k2"; the tray empties. Two protocol watchers (a
   1.4.2 guest and a 1.3.0 peer in god mode) see "n" 0 at once and "n" 1 ten seconds later; at "n" 1
   every active player but the caller and the god-mode watcher dies, killed by the caller, flagged as
   Nuke kills (NET_KILL_NUKE, cleared for the 1.3.0 peer); the round goes on (no END) and the caller
   leads with +50 per victim. The client shows the white #imgFlash, which fades out, and the kill
   feed's streak icon. Frames: the tray before the call and the detonation (logs kept on failure).
2. Team Deathmatch, protocol only: a guest caller's Nuke ("k2" from the packet) kills every active
   enemy and none of its team.
Needs a headless GL device for part 1 (KRUNKNATIVE_HEADLESS=1: EGL; KRUNKNATIVE_EGL_DEVICE picks it).
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-nuke-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
SPAWN, CHAT_SEND, KILL, HIT, LEADERBOARD, END, LEAVE, FORWARDED = 4, 8, 10, 11, 14, 16, 20, 85
STREAK, USE_STREAK, NUKE = 171, 172, 173
KILL_FISTS, KILL_NUKE = 0x01, 0x02
KILL_FMT = "<iiiBBBiiiif"  # NetKillPacket (packed, 35 bytes)
results = []
procs = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def start_server(name, mode):
    rcon_port, port = kn_proto.free_ports(2)
    cfg = os.path.join(WORK, f"{name}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Nuke {name}"\nport {port}\nregion local\nmaxplayers 40\nmapcycle Sandstorm\nmode {mode}\n'
                f"round_seconds 900\ncheats all\nrcon_password rc\nrcon_port {rcon_port}\n")  # guests may /kill
    none = os.path.join(WORK, "none.json")
    with open(none, "w") as f:
        f.write("{}")
    log = os.path.join(WORK, f"{name}-server.log")
    with open(log, "w") as out:
        p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", none], stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    procs.append(p)

    def rcon(*cmd):
        out = subprocess.run([f"{BUILD}/server_bin", "--rcon", f"127.0.0.1:{rcon_port}", "--rcon-password", "rc", *cmd],
                             capture_output=True, text=True, timeout=10)
        return out.stdout + out.stderr

    end = time.time() + 10
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 0.5).close()
            break
        except OSError:
            time.sleep(0.1)
    return p, port, rcon, log


def stop(p):
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()


class Peer(kn_proto.Conn):
    """A protocol client: HELLO `version` (major, minor, patch), optionally bridged for a remote address
    (net-bridge NET_PACKET_FORWARDED: not this machine, a guest)."""

    def __init__(self, port, name, version, remote=None):
        super().__init__("127.0.0.1", port)
        if remote:
            self.send(FORWARDED, remote.encode() + b"\0")
        self.send(kn_proto.HELLO, bytes(version))
        self.join(name=name)
        self.uid = None
        self.times = []  # arrival time of each packet in self.packets

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            n = len(self.packets)
            self.read(timeout=max(0.05, end - time.time()))
            self.times += [time.time()] * (len(self.packets) - n)

    def spawn(self):
        self.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 6
        while time.time() < end and self.uid is None:
            self.pump(0.2)
            for t, p in self.packets:
                info = spawn(p) if t == SPAWN else None
                if info and info["is_you"]:
                    self.uid = info["uid"]
        return self.uid

    def say(self, text):
        self.send(CHAT_SEND, text.encode() + b"\0")
        self.pump(0.45)


def spawn(payload):
    fmt = "<iBBii"  # uid, is_you, active, class, team
    if len(payload) < struct.calcsize(fmt):
        return None
    uid, is_you, active, _cls, team = struct.unpack(fmt, payload[: struct.calcsize(fmt)])
    return {"uid": uid, "is_you": is_you, "active": active, "team": team}


def kill(payload):
    if len(payload) != struct.calcsize(KILL_FMT):
        return None
    v = struct.unpack(KILL_FMT, payload)
    return {"victim": v[0], "killer": v[1], "fists": v[5]}


def world(peer, upto):
    """uid -> {active, team} from this peer's SPAWN / KILL / LEAVE packets before index `upto`."""
    players = {}
    for t, p in peer.packets[:upto]:
        if t == SPAWN:
            info = spawn(p)
            if info:
                players[info["uid"]] = {"active": bool(info["active"]), "team": info["team"]}
        elif t == KILL:
            k = kill(p)
            if k and k["victim"] in players:
                players[k["victim"]]["active"] = False
        elif t == LEAVE and len(p) >= 4:
            players.pop(struct.unpack("<i", p[:4])[0], None)
    return players


def nuke_events(peer):
    """[(index, state, time)] of the "n" packets this peer got."""
    return [(i, p[0], peer.times[i]) for i, (t, p) in enumerate(peer.packets) if t == NUKE and len(p) == 1]


def burst_kills(peer, start):
    """The KILL packets right after index `start` (the detonation's tick: before any other packet type
    but kill-side traffic of that tick)."""
    out = []
    for t, p in peer.packets[start + 1:]:
        if t == KILL:
            k = kill(p)
            if k:
                out.append(k)
        elif t in (SPAWN, NUKE):
            break
    return out


def leaderboard(peer, before=None):
    """The last leaderboard (uid -> score) before index `before`."""
    last = None
    for t, p in peer.packets[:before]:
        if t == LEADERBOARD and len(p) >= 1:
            n = p[0]
            entries = [struct.unpack("<iiii", p[1 + 16 * i: 17 + 16 * i]) for i in range(n) if len(p) >= 17 + 16 * i]
            last = {e[0]: e[1] for e in entries}
    return last or {}


def own_address():
    """A non-loopback address of this machine (the route source towards a documentation address, no
    packet sent): the server then sees the client as a remote guest. None when there is none."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(("192.0.2.1", 9))
            ip = s.getsockname()[0]
        return None if ip.startswith("127.") else ip
    except OSError:
        return None


def run_client(port, host, script):
    path = os.path.join(WORK, "client.script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "XDG_CONFIG_HOME": os.path.join(WORK, "client-cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "client-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "960x540"})
    out = open(log, "w")
    p = subprocess.Popen(["stdbuf", "-oL", f"{BUILD}/client_bin", "--connect", f"{host}:{port}"], stdout=out,
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    return p, log


def ffa():
    server, port, rcon, slog = start_server("ffa", "ffa")
    watcher = Peer(port, "Watcher", [1, 4, 2], remote="203.0.113.7")  # a guest on protocol 1.4.2
    old = Peer(port, "OldPeer", [1, 3, 0])  # a 1.3.0 peer: no NET_KILL_NUKE bit
    check("ffa: the watchers spawn", watcher.spawn() is not None and old.spawn() is not None)
    rcon("bot_stop", "on")  # frozen bots: nobody but the caller kills
    check("ffa: rcon bot_add 26", "Spawned 26 nextbot bots." in rcon("bot_add", "26"))
    god = rcon("god", str(old.uid))
    check("ffa: rcon god <the 1.3.0 peer> (god mode survives the Nuke)", "ON" in god.upper(), god.strip()[-120:])
    watcher.pump(1.0)
    old.pump(0.2)

    remote = own_address()
    host = remote or "127.0.0.1"
    shots = os.path.join(WORK, "frame")
    # times from `play`; the server's Nuke runs on wall-clock time, so no fixed delta
    script = ["0 play", "1500 con kill @bots", "2500 vis @tray killStreakHolder", "2550 nodes @trayitems .killStreakItem",
              f"2600 frames {shots}-tray 1", "2700 down 3", "2750 up 3", "3300 vis @after killStreakHolder"]
    script += [f"{t} vis @flash imgFlash" for t in range(11800, 14600, 40)]
    script += [f"{t} frames {shots}-blast{t} 1" for t in (12780, 12860, 12940, 13100)]
    script += ["13600 nodes @feed .streakChatIcon", "15000 vis @end imgFlash killStreakHolder", "15500 quit"]
    script.sort(key=lambda line: int(line.split()[0]))  # the script runs in file order
    client, clog = run_client(port, host, "\n".join(script) + "\n")
    t_press = None
    end = time.time() + 90
    while client.poll() is None and time.time() < end:
        watcher.pump(0.05)
        old.pump(0.02)
    if client.poll() is None:
        os.killpg(client.pid, signal.SIGKILL)
        check("ffa: client_bin finished its script", False, f"timeout, log {clog}")
    client.wait()
    watcher.pump(0.5)
    old.pump(0.2)
    lines = read(clog).splitlines()
    server_log = read(slog)

    if remote:
        check("ffa: the client joins as a remote guest (not this machine)", f"connected from {remote}" in server_log,
              f"expected 'connected from {remote}'")
    else:
        print("NOTE  no non-loopback address: the client joins from 127.0.0.1 (the server's own machine)")

    # caller: the killer of the bots (the console's kill @bots)
    killers = [kill(p)["killer"] for t, p in watcher.packets if t == KILL and kill(p)]
    caller = max(set(killers), key=killers.count) if killers else None
    tray = [l for l in lines if "vis t=" in l and "@tray" in l]
    items = [l for l in lines if "@trayitems" in l]
    check("ffa: 25 kills put the Nuke in the HUD tray", tray and tray[0].endswith("killStreakHolder=1")
          and items and "match=1" in items[0], "; ".join(tray + items))
    after = [l for l in lines if "@after" in l]
    check("ffa: the streak key (3) calls it in: the tray empties", after and after[0].endswith("killStreakHolder=0"), "; ".join(after))

    events = nuke_events(watcher)
    states = [s for _, s, _ in events]
    check("ffa: \"n\" 0 then \"n\" 1 to everyone", states[:2] == [0, 1] and [s for _, s, _ in nuke_events(old)][:2] == [0, 1],
          f"watcher {states}, 1.3.0 peer {[s for _, s, _ in nuke_events(old)]}")
    if len(events) >= 2:
        gap = events[1][2] - events[0][2]
        check("ffa: it detonates 10 s after the call", 9.5 <= gap <= 10.8, f"{gap:.2f} s")
        i_blast = events[1][0]
        before = world(watcher, i_blast)
        targets = {u for u, s in before.items() if s["active"] and u not in (caller, old.uid)}
        burst = burst_kills(watcher, i_blast)
        victims = {k["victim"] for k in burst}
        check("ffa: every active player but the caller dies (bots and the watcher)",
              targets and victims == targets and watcher.uid in victims and len(targets) >= 20,
              f"{len(victims)} killed of {len(targets)} active; missing {sorted(targets - victims)[:8]}, extra {sorted(victims - targets)[:8]}")
        check("ffa: the caller and the god-mode player live", caller not in victims and old.uid not in victims)
        check("ffa: killed by the caller, flagged as Nuke kills", burst and all(k["killer"] == caller and k["fists"] & KILL_NUKE for k in burst),
              str(burst[:3]))
        old_burst = burst_kills(old, nuke_events(old)[1][0]) if len(nuke_events(old)) >= 2 else []
        check("ffa: a 1.3.0 peer gets the same kills without the Nuke bit",
              len(old_burst) == len(burst) and all(not k["fists"] & KILL_NUKE for k in old_burst), f"{len(old_burst)} kills")
        left = [i for i, (t, p) in enumerate(watcher.packets) if t == LEAVE and len(p) >= 4 and struct.unpack("<i", p[:4])[0] == caller]
        lb0, lb1 = leaderboard(watcher, i_blast), leaderboard(watcher, left[0] if left else None)  # the board before the client quit
        gained = lb1.get(caller, 0) - lb0.get(caller, 0)
        check("ffa: the caller scores +50 per victim and leads", gained >= 50 * len(victims) and lb1 and max(lb1, key=lb1.get) == caller,
              f"+{gained} for {len(victims)} victims, board {sorted(lb1.items(), key=lambda e: -e[1])[:3]}")
        check("ffa: the round goes on (no END)", not any(t == END for t, _ in watcher.packets))
    flash = [l for l in lines if "@flash" in l]
    shown = [l for l in flash if l.endswith("imgFlash=1")]
    check("ffa: the white flash shows at the blast", shown, f"{len(flash)} samples")
    if shown:
        first = int(shown[0].split("t=")[1].split()[0])
        last = int(shown[-1].split("t=")[1].split()[0])
        check("ffa: and fades out within ~0.5 s", last - first <= 700 and flash[-1].endswith("imgFlash=0"), f"shown {first}..{last} ms")
    feed = [l for l in lines if "@feed" in l]
    check("ffa: the kill feed shows the Nuke icon", feed and "match=0" not in feed[0], "; ".join(feed))
    frames = sorted(f for f in os.listdir(WORK) if f.startswith("frame-") and f.endswith(".png"))
    check("ffa: frames written (tray, blast)", any("tray" in f for f in frames) and any("blast" in f for f in frames), ", ".join(frames))
    watcher.close()
    old.close()
    stop(server)


def tdm():
    server, port, rcon, _ = start_server("tdm", "tdm")
    caller = Peer(port, "Caller", [1, 4, 2], remote="203.0.113.8")
    other = Peer(port, "Other", [1, 4, 2], remote="203.0.113.9")
    check("tdm: the players spawn", caller.spawn() is not None and other.spawn() is not None)
    rcon("bot_stop", "on")
    check("tdm: rcon bot_add 26", "Spawned 26 nextbot bots." in rcon("bot_add", "26"))
    caller.pump(1.5)
    caller.say("/kill @bots")
    caller.pump(1.0)
    streaks = [p for t, p in caller.packets if t == STREAK and len(p) >= 2]
    check("tdm: a guest's 25 kills earn the Nuke (\"st\" 0)", any(p[0] == 0 and p[1] >= 1 for p in streaks), str([p.hex() for p in streaks]))
    caller.send(USE_STREAK, bytes([2]))  # "k2"
    end = time.time() + 12
    while time.time() < end and len(nuke_events(caller)) < 2:
        caller.pump(0.1)
        other.pump(0.02)
    caller.pump(0.3)
    events = nuke_events(caller)
    check("tdm: \"n\" 0 then \"n\" 1", [s for _, s, _ in events][:2] == [0, 1], str(events))
    if len(events) >= 2:
        before = world(caller, events[1][0])
        team = before.get(caller.uid, {}).get("team")
        enemies = {u for u, s in before.items() if s["active"] and s["team"] != team}
        mates = {u for u, s in before.items() if s["active"] and s["team"] == team and u != caller.uid}
        victims = {k["victim"] for k in burst_kills(caller, events[1][0])}
        check("tdm: every active enemy dies", enemies and victims == enemies, f"{len(victims)} of {len(enemies)}")
        check("tdm: no team mate dies", mates and not (victims & mates), f"{len(mates)} mates, {len(victims & mates)} killed")
    caller.close()
    other.close()
    stop(server)


def main():
    try:
        ffa()
        tdm()
    finally:
        for p in procs:
            stop(p)
    if results and all(results):
        keep = os.environ.get("KN_NUKE_FRAMES")
        if keep:  # copy the frames out before the work dir goes
            os.makedirs(keep, exist_ok=True)
            for f in os.listdir(WORK):
                if f.startswith("frame-"):
                    shutil.copy(os.path.join(WORK, f), keep)
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    print(f"{sum(results)}/{len(results)} passed")
    sys.exit(0 if results and all(results) else 1)


if __name__ == "__main__":
    main()

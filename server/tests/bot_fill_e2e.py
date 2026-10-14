#!/usr/bin/env python3
"""server.cfg bot_quota fill mode on a dedicated server (shared/src/bots.c, doc/console.md "Bots").

  python3 -I server/tests/bot_fill_e2e.py BUILD_DIR      (run from the repository root)

One standalone server_bin (max_players 4, bot_quota 4, bot_quota_grace 3), real kn_proto clients:
  1. an empty lobby stays empty (bots 0 for 3 s)
  2. one human: 3 bots come in (quota - humans)
  2b. `autofill off` (the human is the server's own machine: a DEV peer): the bots leave and stay
     away, the query drops its "autofill" tag; `autofill on` brings the 3 back; `autofill 2` fills to
     2 players; `autofill 4` back to the server's quota
  3. three more humans join the bot-filled lobby: every one spawns (no "server full"), a bot
     leaves per join, the query shows players 4 / bots 0
  4. humans leave: the bots come back
  5. the last human leaves: the bots stay for the grace, then the lobby is empty again
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
(PORT,) = kn_proto.free_ports(1)  # ephemeral: concurrent runs must not share servers
WORK = tempfile.mkdtemp(prefix="kn-bot-fill-e2e-")
SPAWN, ERROR = kn_proto.SPAWN, kn_proto.ERROR
results = []
procs = []


def check(name, ok, detail=""):
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def query():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", PORT))
        data, _ = s.recvfrom(2048)
    return json.loads(data[11:])


def counts():
    try:
        info = query()
        return info.get("players"), info.get("bots")
    except OSError:
        return None, None


def wait_counts(players, bots, timeout=8.0):
    end = time.time() + timeout
    last = (None, None)
    while time.time() < end:
        last = counts()
        if last == (players, bots):
            return True, last
        time.sleep(0.1)
    return False, last


def tags():
    try:
        return query().get("tags", [])
    except OSError:
        return []


def say(conn, text):
    time.sleep(0.4)  # the chat burst limit
    conn.send(8, text.encode() + b"\0")  # NET_PACKET_CHAT_SEND
    conn.read(timeout=0.3)


def join(name):
    """A guest that sends ENT; returns (conn, spawned)."""
    c = kn_proto.Conn("127.0.0.1", PORT)
    c.hello()
    c.join(name=name)
    c.ent(class_index=0, lag_comp=1.0)
    end = time.time() + 6
    while time.time() < end:
        p = c.read(until=SPAWN, timeout=1)
        if p is not None and kn_proto.spawn_info(p)["is_you"]:
            return c, True
    return c, False


def main():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    cfg = os.path.join(WORK, "s.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Bot fill"\nport {PORT}\nregion local\nmaxplayers 4\nmapcycle Sandstorm\nmode ffa\n'
                "round_seconds 600\nbot_quota 4\nbot_quota_grace 3\nbot_difficulty easy\n")
    log = open(os.path.join(WORK, "s.log"), "w")
    procs.append(subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                                  stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
    end = time.time() + 15
    while time.time() < end and counts() == (None, None):
        time.sleep(0.2)
    check("server up", counts() != (None, None))

    # 1. empty
    seen = set()
    end = time.time() + 3
    while time.time() < end:
        seen.add(counts())
        time.sleep(0.2)
    check("empty lobby stays empty", seen == {(0, 0)}, str(seen))

    # 2. one human
    a, ok = join("Alpha")
    check("first human spawns", ok)
    ok, last = wait_counts(1, 3)
    check("1 human: 3 bots fill the lobby", ok, str(last))
    check("  the query says autofill", "autofill" in tags(), str(tags()))

    # 2b. autofill off / <n> / on at run time (a DEV peer: the server's own machine)
    say(a, "/autofill off")
    ok, last = wait_counts(1, 0, 3.0)
    check("autofill off: the bots leave", ok, str(last))
    time.sleep(1.5)
    check("  and stay away", counts() == (1, 0) and "autofill" not in tags(), f"{counts()} {tags()}")
    say(a, "/autofill on")
    ok, last = wait_counts(1, 3, 4.0)
    check("autofill on: the server's quota fills the lobby again", ok and "autofill" in tags(), f"{last} {tags()}")
    say(a, "/autofill 2")
    ok, last = wait_counts(1, 1, 4.0)
    check("autofill 2: fill to 2 players (1 bot)", ok, str(last))
    say(a, "/autofill 4")
    ok, last = wait_counts(1, 3, 4.0)
    check("autofill 4: the 3 bots are back", ok and "autofill" in tags(), f"{last} {tags()}")

    # 3. the lobby is full (1 + 3 bots); every human still gets in
    others = []
    for i in range(3):
        c, spawned = join(f"Joiner{i}")
        errors = [p for t, p in c.packets if t == ERROR]
        check(f"human {i + 2} joins the bot-filled lobby", spawned and not errors, f"spawned={spawned} errors={len(errors)}")
        others.append(c)
        ok, last = wait_counts(2 + i, 2 - i, 3.0)
        check(f"  a bot left for it (players {2 + i}, bots {2 - i})", ok, str(last))

    # 4. leaves bring the bots back
    for c in others[:2]:
        c.close()
    ok, last = wait_counts(2, 2)
    check("2 humans left: 2 bots back", ok, str(last))
    others[2].close()
    ok, last = wait_counts(1, 3)
    check("1 human: 3 bots", ok, str(last))

    # 5. the last human leaves: grace, then empty
    a.close()
    ok, last = wait_counts(0, 3, 3.0)
    check("last human gone: the bots hold the match during the grace", ok, str(last))
    time.sleep(1.5)
    p, b = counts()
    check("  still there 1.5 s later", (p, b) == (0, 3), str((p, b)))
    ok, last = wait_counts(0, 0, 6.0)
    check("after the grace the lobby is empty", ok, str(last))
    seen = set()
    end = time.time() + 2
    while time.time() < end:
        seen.add(counts())
        time.sleep(0.2)
    check("  and stays empty", seen == {(0, 0)}, str(seen))


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
        path = os.path.join(WORK, "s.log")
        if os.path.exists(path):
            print("--- s.log (tail)")
            print(open(path).read()[-3000:])
print(f"{sum(results)}/{len(results)} passed", flush=True)
sys.exit(0 if results and all(results) else 1)

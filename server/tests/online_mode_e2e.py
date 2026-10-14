#!/usr/bin/env python3
"""online_mode servers take guests and accounts alike (doc/backend.md 8.9), like every Krunker lobby
(v10 /ws sends at=<token> only when there is one; an unnamed player is Guest_<sid>; a token that
does not refresh connects as a guest). online_mode only adds account verification (hasJoined).

  python3 -I server/tests/online_mode_e2e.py BUILD_DIR      (repo root; own database)

One kn_brain (coordinator + central auth) and two online_mode server_bin:
  S  standalone (hasJoined straight to the auth service)
  L  linked to the coordinator (hasJoined through it; stats and KR committed)
Checks:
  S: AUTH_CHALLENGE after HELLO; register + /auth/join + JOIN "hj1:<nonce>" spawns as the account;
     a forged "hj1:" (a serverHash nobody announced, claiming that account's name) and a malformed
     one are admitted as Guest_N, never the account; the "hj0" guest answer spawns a guest at once;
     an old client that never answers the challenge is a guest at join_timeout; the info query has
     no online/offline tag and online_mode false (older clients label such servers account-only).
  L: no onlineMode in the game list / hello (onlineModeNodes 0); a guest seek-game for the lobby is
     not AccountRequired; an account (hasJoined via the coordinator), a guest (hj0), a forged
     "hj1:" claiming a second account and a forged session token play one round: only the
     verified account's match_results row and KR land, the claimed account gets nothing, no
     account is created for a guest; a forged join ticket is refused (Invalid join key).
"""
import hashlib
import json
import os
import secrets
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
DB = f"kn_ds_online_{os.getpid()}"
BRAIN_PORT, PORT_S, PORT_L = kn_proto.free_ports(3)
JOIN_TIMEOUT = 3
WORK = tempfile.mkdtemp(prefix="kn-ds-online-")
CHAT_SEND = 8
procs, results = [], []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def cfgdb():
    with open("config/config.json") as f:
        return json.load(f)["database"]


def psql(sql):
    d = cfgdb()
    env = dict(os.environ, PGPASSWORD=d.get("password", ""))
    out = subprocess.run(["psql", "-h", d.get("host", "127.0.0.1"), "-U", d.get("user", "krunknative"), "-d", DB,
                          "-v", "ON_ERROR_STOP=1", "-qAt", "-c", sql], env=env, capture_output=True, text=True)
    if out.returncode:
        raise RuntimeError(out.stderr)
    return out.stdout.strip()


def http(path, method="GET", body=None):
    req = urllib.request.Request(f"http://127.0.0.1:{BRAIN_PORT}{path}", method=method,
                                 data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def log(name):
    try:
        with open(os.path.join(WORK, name)) as f:
            return f.read()
    except OSError:
        return ""


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


def query(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO", ("127.0.0.1", port))
        return json.loads(s.recvfrom(2048)[0][11:])


def server_hash(sn, pk, cn):
    return hashlib.sha256(b"kn-join-v1\0" + sn.encode() + b"\0" + pk.encode() + b"\0" + cn.encode()).hexdigest()


def register(name):
    token = secrets.token_hex(16)
    psql(f"SELECT kn_register('{name}', 'x', '127.0.0.1', '', 0, 0, '{token}', 3600, 'e2e')")
    return token, int(psql(f"SELECT id FROM accounts WHERE username = '{name}'"))


def start_server(name, port, extra):
    cfg = os.path.join(WORK, f"{name}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "Online {name}"\nport {port}\nnode_id ONL{name}\nregion local\nmaxplayers 8\n'
                f"online_mode 1\nauth_url http://127.0.0.1:{BRAIN_PORT}\nmapcycle Burg\nmode ffa\n"
                f"join_timeout {JOIN_TIMEOUT}\nbot_quota 0\n{extra}")
    procs.append(subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                                  stdout=open(os.path.join(WORK, f"{name}.log"), "w"), stderr=subprocess.STDOUT,
                                  stdin=subprocess.DEVNULL))


def connect(port):
    """HELLO + the client's first JOIN (sent before it saw the challenge) + the challenge."""
    c = kn_proto.Conn("127.0.0.1", port)
    c.hello()
    c.join(name="Guest")
    ch = c.read(until=kn_proto.AUTH_CHALLENGE, timeout=5)
    return c, ch


def hj_join(c, ch, token, name):
    """What client/src/net/online_mode.c does when signed in: /auth/join, then the "hj1:" JOIN."""
    sn, pk, cn = kn_proto.uncstr(ch[:33]), kn_proto.uncstr(ch[34:99]), secrets.token_hex(16)
    st, j = http("/auth/join", "POST", {"token": token, "serverHash": server_hash(sn, pk, cn)})
    c.join(client_key="hj1:" + cn, name=name)
    return st == 200 and j.get("ok")


def spawn(c, timeout=6):
    """ENT, then this connection's own SPAWN (None when it never came or the server refused)."""
    c.ent()
    end = time.time() + timeout
    while time.time() < end:
        p = c.read(until=kn_proto.SPAWN, timeout=max(0.1, end - time.time()))
        if p is None:
            return None
        sp = kn_proto.spawn_info(p)
        if sp and sp["is_you"]:
            return sp
    return None


def errors(c):
    return [kn_proto.error_info(p) for t, p in c.packets if t == kn_proto.ERROR]


def is_guest(sp):
    return bool(sp) and sp["name"].startswith("Guest_")


def say(c, text):
    c.send(CHAT_SEND, text.encode() + b"\0")
    c.read(timeout=0.45)  # the chat burst limit is 3 lines per second


def main():
    with open(os.path.join(WORK, "none.json"), "w") as f:
        json.dump({"database": cfgdb()}, f)
    subprocess.run(["sudo", "-n", "-u", "postgres", "createdb", "-O", cfgdb().get("user", "krunknative"), DB], check=True)
    try:
        subprocess.run([f"{BUILD}/services/common/kn_migrate", "--set", f"database.name={DB}"], check=True,
                       stdout=open(os.path.join(WORK, "migrate.log"), "w"), stderr=subprocess.STDOUT)
        procs.append(subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--set", f"database.name={DB}", "--set", f"brain.port={BRAIN_PORT}",
                                       "--set", "brain.region=local"], stdout=open(os.path.join(WORK, "brain.log"), "w"),
                                      stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
        up = bool(wait_for(lambda: http("/mm/status")[0] == 200)) and procs[-1].poll() is None
        check("coordinator + auth up", up, "" if up else log("brain.log")[-300:])
        start_server("S", PORT_S, "")
        start_server("L", PORT_L, f"coordinator_url ws://127.0.0.1:{BRAIN_PORT}/mm/internal/ws\n"
                                  f"coordinator_http_url http://127.0.0.1:{BRAIN_PORT}\nperms 0\ncheats all\n")
        linked_up = bool(wait_for(lambda: "welcomed" in log("L.log")))
        check("L linked to the coordinator", linked_up, "" if linked_up else log("L.log")[-300:])
        standalone()
        linked()
    finally:
        for p in procs:
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
        for p in procs:
            try:
                p.wait(15)
            except subprocess.TimeoutExpired:
                p.kill()
        if not os.environ.get("KN_TEST_KEEP"):
            subprocess.run(["sudo", "-n", "-u", "postgres", "dropdb", "--if-exists", DB])
        print(f"logs: {WORK}")
    ok = results and all(results)
    print(f"{'ALL PASSED' if ok else 'FAILED'} ({sum(results)}/{len(results)})")
    sys.exit(0 if ok else 1)


def standalone():
    name = "onl_" + secrets.token_hex(3)
    token, _ = register(name)
    conns = []

    c, ch = connect(PORT_S)
    conns.append(c)
    check("S: AUTH_CHALLENGE after HELLO (online_mode 1)", ch is not None and ch[32] == 0 and ch[33] == 1, repr(ch[:40]) if ch else "none")
    if not ch:
        return
    check("S: central /auth/join", hj_join(c, ch, token, name))
    sp = spawn(c)
    check("S: verified hasJoined join spawns as the account", bool(sp) and sp["name"] == name, str(sp))

    d, ch = connect(PORT_S)  # a serverHash nobody announced, claiming that account's name
    conns.append(d)
    d.join(client_key="hj1:" + secrets.token_hex(16), name=name)
    sp = spawn(d, timeout=8)
    check("S: forged hj1 join is a guest, never the claimed account", is_guest(sp) and not errors(d), f"{sp} {errors(d)}")
    check("S: the failed verification is logged", "joins as a guest (failed to verify username)" in log("S.log"))

    m, ch = connect(PORT_S)
    conns.append(m)
    m.join(client_key="hj1:not-hex", name=name)
    sp = spawn(m)
    check("S: malformed hj1 join is a guest", is_guest(sp) and not errors(m), f"{sp} {errors(m)}")

    g, ch = connect(PORT_S)  # a signed-out client answers the challenge with the guest key
    conns.append(g)
    t0 = time.time()
    g.join(client_key="hj0")
    sp = spawn(g)
    took = time.time() - t0
    check("S: hj0 guest answer spawns a guest at once (no join_timeout wait)", is_guest(sp) and took < JOIN_TIMEOUT - 1,
          f"{sp} {took:.2f}s")

    o, ch = connect(PORT_S)  # a client from before hj0: never answers the challenge
    conns.append(o)
    t0 = time.time()
    sp = spawn(o, timeout=JOIN_TIMEOUT + 5)
    took = time.time() - t0
    check("S: an old client's silent guest joins at join_timeout", is_guest(sp) and not errors(o) and took >= JOIN_TIMEOUT - 0.5,
          f"{sp} {took:.2f}s {errors(o)}")

    info = query(PORT_S)
    check("S: info query has no online/offline tag and online_mode false",
          info.get("online_mode") is False and not {"online", "offline"} & set(info.get("tags", [])), json.dumps(info))
    for x in conns:
        x.close()


def linked():
    st, status = http("/mm/status")
    check("L: the coordinator sees no account-only (onlineMode) node", st == 200 and status.get("onlineModeNodes") == 0, json.dumps(status))
    lobby = wait_for(lambda: [g for g in http("/mm/game-list?hostname=krunknative")[1]["games"] if g[0].startswith("ONLL:")])
    lobby_id = lobby[0][0] if lobby else "ONLL:x"
    check("L: lobby listed without an onlineMode marker", bool(lobby) and "onlineMode" not in lobby[0][4], json.dumps(lobby)[:300])
    st, seek = http(f"/mm/seek-game?hostname=krunknative&region=local&game={lobby_id}&autoChangeGame=false")
    check("L: a guest seek-game gets the lobby (no AccountRequired)", st == 200 and seek.get("gameId") == lobby_id, f"{st} {json.dumps(seek)[:200]}")

    alice, victim = "onl_alice_" + secrets.token_hex(2), "onl_victim_" + secrets.token_hex(2)
    ta, alice_id = register(alice)
    _, victim_id = register(victim)
    funds = lambda i: int(psql(f"SELECT coalesce((SELECT player_funds FROM players WHERE player_id = {i}), 0)"))  # noqa: E731
    alice_funds, victim_funds = funds(alice_id), funds(victim_id)
    accounts_before = int(psql("SELECT count(*) FROM accounts"))
    conns = []

    a, ch = connect(PORT_L)
    conns.append(a)
    check("L: AUTH_CHALLENGE", ch is not None and ch[33] == 1)
    if not ch:
        return
    hj_join(a, ch, ta, alice)
    spa = spawn(a)
    check("L: the account joins through hasJoined via the coordinator", bool(spa) and spa["name"] == alice, str(spa))

    g, ch = connect(PORT_L)
    conns.append(g)
    g.join(client_key="hj0")
    spg = spawn(g)
    check("L: a guest joins the same public server", is_guest(spg) and not errors(g), f"{spg} {errors(g)}")

    f, ch = connect(PORT_L)  # impersonation attempt: the victim's name, a hash nobody announced
    conns.append(f)
    f.join(client_key="hj1:" + secrets.token_hex(16), name=victim)
    spf = spawn(f, timeout=12)
    check("L: forged hj1 claiming another account is a guest", is_guest(spf) and not errors(f), f"{spf} {errors(f)}")

    s = kn_proto.Conn("127.0.0.1", PORT_L)  # a brain-mode JOIN with a forged session token
    conns.append(s)
    s.hello()
    s.join(session=secrets.token_hex(16))
    sps = spawn(s, timeout=12)
    check("L: forged session token is a guest, not an account", is_guest(sps) and not errors(s), f"{sps} {errors(s)}")

    t = kn_proto.Conn("127.0.0.1", PORT_L)  # a forged seek-game ticket: a seat claim, refused
    conns.append(t)
    t.hello()
    t.join(client_key="eyJmb3JnZWQiOjF9.bm9wZQ")
    err = t.read(until=kn_proto.ERROR, timeout=8)
    check("L: forged join ticket refused", err is not None and "join key" in kn_proto.error_info(err)[0].lower(),
          str(err and kn_proto.error_info(err)))

    # one round: everybody scores 500 (5 KR each), then /end (cheats all: any player may)
    for c in (a, g, f, s):
        say(c, "/score 500")
    say(a, "/end")
    rows = wait_for(lambda: psql(f"SELECT kr, score FROM match_results WHERE account_id = {alice_id} AND match_id NOT LIKE '%#c%'"), 20)
    check("L: the account's round is saved (match_results, score, KR)", bool(rows) and int(rows.split("|")[0]) >= 5
          and int(rows.split("|")[1]) >= 500, str(rows))
    check("L: the account's KR is credited", bool(wait_for(lambda: funds(alice_id) >= alice_funds + 5, 10)),
          f"{alice_funds} -> {funds(alice_id)}")
    match = psql(f"SELECT match_id FROM match_results WHERE account_id = {alice_id} AND match_id NOT LIKE '%#c%' LIMIT 1")
    check("L: nothing saved for the guests (one row in the round)",
          psql(f"SELECT count(*) FROM match_results WHERE match_id = '{match}'") == "1", match)
    check("L: the claimed account got no stats and no KR",
          psql(f"SELECT count(*) FROM match_results WHERE account_id = {victim_id}") == "0" and funds(victim_id) == victim_funds)
    check("L: no account created for a guest", int(psql("SELECT count(*) FROM accounts")) == accounts_before)
    for x in conns:
        x.close()


if __name__ == "__main__":
    main()

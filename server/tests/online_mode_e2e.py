#!/usr/bin/env python3
"""online_mode standalone server test (doc/backend.md 8.9): server_bin verifies players with
the central auth service's hasJoined (kn_brain --brain.roles=auth) without a coordinator.

  python3 -I server/tests/online_mode_e2e.py BUILD_DIR      (repo root; own database)

Checks: AUTH_CHALLENGE after HELLO; register + /auth/join + JOIN "hj1:<nonce>" spawns as
the account; a JOIN for a serverHash nobody announced is refused; a guest (no JOIN) is
refused; the info query says online_mode.
"""
import hashlib
import json
import os
import secrets
import signal
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
DB = f"kn_ds_online_{os.getpid()}"
AUTH_PORT, GAME_PORT = 5393, 22331
WORK = tempfile.mkdtemp(prefix="kn-ds-online-")
procs, results = [], []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def post(path, body):
    req = urllib.request.Request(f"http://127.0.0.1:{AUTH_PORT}{path}", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def server_hash(sn, pk, cn):
    return hashlib.sha256(b"kn-join-v1\0" + sn.encode() + b"\0" + pk.encode() + b"\0" + cn.encode()).hexdigest()


def connect():
    c = kn_proto.Conn("127.0.0.1", GAME_PORT)
    c.hello()
    ch = c.read(until=kn_proto.AUTH_CHALLENGE, timeout=5)
    return c, ch


def main():
    dbuser = json.load(open("config/config.json"))["database"].get("user", "krunknative")
    subprocess.run(["sudo", "-n", "-u", "postgres", "createdb", "-O", dbuser, DB], check=True)
    try:
        subprocess.run([f"{BUILD}/services/common/kn_migrate", "--set", f"database.name={DB}"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        procs.append(subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--set", f"database.name={DB}", "--set", f"brain.port={AUTH_PORT}",
                                       "--brain.roles=auth"], stdout=open(f"{WORK}/auth.log", "w"), stderr=subprocess.STDOUT))
        time.sleep(1.0)
        with open(f"{WORK}/server.cfg", "w") as f:
            f.write(f'hostname "Online"\nport {GAME_PORT}\nonline_mode 1\nauth_url http://127.0.0.1:{AUTH_PORT}\nmapcycle Burg\njoin_timeout 2\n')
        procs.append(subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", f"{WORK}/server.cfg", "--config", f"{WORK}/none.json"],
                                      stdout=open(f"{WORK}/server.log", "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
        open(f"{WORK}/none.json", "w").write("{}")
        time.sleep(1.0)
        run()
    finally:
        for p in procs:
            p.send_signal(signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        subprocess.run(["sudo", "-n", "-u", "postgres", "dropdb", "--if-exists", DB])
        print(f"logs: {WORK}")
    ok = results and all(results)
    print(f"{'ALL PASSED' if ok else 'FAILED'} ({sum(results)}/{len(results)})")
    sys.exit(0 if ok else 1)


def run():
    name = "online_" + secrets.token_hex(3)
    st, reg = post("/auth/register", {"username": name, "password": "pw-" + secrets.token_hex(6)})
    check("central register", st == 200 and reg.get("token"), json.dumps(reg)[:200])
    token = reg.get("token", "")

    c, ch = connect()
    check("AUTH_CHALLENGE after HELLO", ch is not None and ch[32] == 0 and ch[33] == 1, repr(ch[:40]) if ch else "none")
    sn = kn_proto.uncstr(ch[:33]) if ch else ""
    pk = kn_proto.uncstr(ch[34:99]) if ch else ""
    cn = secrets.token_hex(16)
    c.join(name="Guest")  # what a client sends before it saw the challenge: must be ignored
    st, j = post("/auth/join", {"token": token, "serverHash": server_hash(sn, pk, cn)})
    check("central /auth/join", st == 200 and j.get("ok"), json.dumps(j))
    c.join(client_key="hj1:" + cn, name=name)
    c.ent()
    sp = None
    while True:
        p = c.read(until=kn_proto.SPAWN, timeout=6)
        if p is None:
            break
        sp = kn_proto.spawn_info(p)
        if sp and sp["is_you"]:
            break
    check("verified join spawns as the account", bool(sp) and sp["name"] == name, str(sp))

    d, ch = connect()
    d.join(client_key="hj1:" + secrets.token_hex(16), name=name)  # nobody announced this hash
    err = d.read(until=kn_proto.ERROR, timeout=8)
    check("unannounced serverHash refused", err is not None and "verify" in kn_proto.error_info(err)[0].lower(),
          str(err and kn_proto.error_info(err)))

    e, ch = connect()  # no JOIN at all (an offline client)
    err = e.read(until=kn_proto.ERROR, timeout=8)
    check("guest refused on an online-mode server", err is not None, str(err and kn_proto.error_info(err)))

    import socket
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1)
        s.sendto(b"\xff\xff\xff\xffKNINFO", ("127.0.0.1", GAME_PORT))
        info = json.loads(s.recvfrom(2048)[0][11:])
    check("info query reports online_mode", info.get("online_mode") is True and "online" in info.get("tags", []), json.dumps(info))
    for x in (c, d, e):
        x.close()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Command authorisation (doc/v7/command-auth.md): forged privileged packets from plain guests are
refused and logged, the legitimate paths still work.

  python3 -I server/tests/command_auth_e2e.py BUILD_DIR      (run from the repository root)

No database or coordinator. Ports from kn_proto.free_ports(); every server_bin this test starts is
stopped at the end (also on failure). Three standalone servers from generated server.cfg files:

  A. the production default: no `perms`, no `cheats` line (perms off, cheats "auth"), rcon on
     loopback. The host connects over loopback (the server's own machine: a DEV peer); the guests
     write a NET_PACKET_FORWARDED first (bridged remote browsers), so they are neither DEV nor host.
     1. a guest's forged chat commands (NET_PACKET_CHAT_SEND and the v7 NET_PACKET_CHAT_V7):
        /kick <host>, /kick * (refused before the selector resolves: no count, no "no match"),
        /ban, /banip, /ip (no address leaks), /map, /end, /god *, /kill *, /bring all, /bot_add,
        /give <weapon> <host>, /autofill: each refused with the notify and a "perms denied ... account 0" line;
        /spawn refused by the Object Manager's rules
     2. forged dev packets: prmA (noclip key), tp, cycle, clearLog: refused and logged
     3. a late NET_PACKET_FORWARDED "127.0.0.1" (claiming the server's machine) is ignored
     4. positive: the host /ip, /god and /kick of a guest work; rcon works with its password
     5. rcon: a wrong and a blank password are refused and logged; 5 bad passwords lock rcon (the
        right password is then refused too, "locked" logged)
  B. a ticket server (brain_url on a loopback port nobody listens on, a random brain_secret,
     `ops 7`): forged join tickets.
     6. a ticket signed with the wrong secret claiming the op account: "Invalid join key"
     7. a correctly signed ticket for another node: "Join key is for another server"
     8. chat before the JOIN settles: refused ("join not verified")
     9. a valid ticket for account 8 (not an op): /kick refused, logged with "account 8"
    10. positive: a valid ticket for account 7 (the op): /kick works
  C. a misconfigured remote coordinator (brain_url on TEST-NET, no brain_secret: the public dev
     default) and rcon on 0.0.0.0 with a short password, cheats "all":
    11. the server refuses to link ("not linking") and to open rcon ("not listening")
    12. positive: perms off + cheats "all" is v3.1.1's open server: a guest's /god works
  D. kn_brain (scratch SQLite database, brain.internal_secret = the public development default):
    13. /mm/admin/drain with a wrong secret: 401, logged
    14. the right (dev default) secret through a proxy (X-Forwarded-For): 403, logged
    15. positive: the same secret on a direct local connection passes the gate (404 NodeNotLocal)
    16. 30 attempts a minute per address: the next one is 429, logged
Prints PASS/FAIL lines; exit code 0 only when everything passed.
"""
import base64
import hashlib
import hmac
import json
import os
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
PORT_A, RCON_A, PORT_B, BRAIN_B, PORT_C, RCON_C = kn_proto.free_ports(6)
WORK = tempfile.mkdtemp(prefix="kn-cmd-auth-e2e-")
SPAWN, CYCLE, CHAT_SEND, CHAT, ERROR, LEAVE = 4, 7, 8, 9, 30, 20
TELEPORT, NOTIFY, CLEAR_LOG, PREMIUM_ACTION, FORWARDED, CHAT_V7 = 80, 82, 83, 84, 85, 245
SECRET_B = secrets.token_hex(24)
results = []
procs = []


def check(name, ok, detail=""):
    ok = bool(ok)
    results.append(ok)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


def query(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", port))
        data, _ = s.recvfrom(2048)
    return data


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


def log(name):
    try:
        with open(os.path.join(WORK, name), errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def start_server(name, port, extra):
    cfg = os.path.join(WORK, f"{name}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "CmdAuth {name}"\nport {port}\nregion local\nnode_id {name.upper()}1\nmaxplayers 12\n'
                f"mapcycle Sandstorm\nmode ffa\nround_seconds 900\nbans_file {os.path.join(WORK, name + '-bans.json')}\n{extra}")
    none = os.path.join(WORK, "none.json")
    with open(none, "w") as f:
        f.write("{}")
    out = open(os.path.join(WORK, f"{name}.log"), "w")
    procs.append(subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--cfg", cfg, "--config", none],
                                  stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL))
    return bool(wait_for(lambda: query(port)))


def rcon(port, *cmd, password):
    out = subprocess.run([f"{BUILD}/server_bin", "--rcon", f"127.0.0.1:{port}", "--rcon-password", password, *cmd],
                         capture_output=True, text=True, timeout=15)
    return out.returncode, out.stdout + out.stderr


def rcon_raw(port, password_line):
    """The first line rcon answers (a blank password the client refuses to send itself)."""
    with socket.create_connection(("127.0.0.1", port), 5) as s:
        s.settimeout(5)
        s.sendall(password_line.encode() + b"\n")
        data = b""
        while b"\n" not in data:
            chunk = s.recv(256)
            if not chunk:
                break
            data += chunk
    return data.decode(errors="replace").strip()


def b64url(raw):
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def ticket(secret, claims):
    """An HS256 join ticket as kn_signed_token_create makes them (services/common/src/crypto.c)."""
    head = b64url(json.dumps({"alg": "HS256", "typ": "JWT"}, separators=(",", ":")).encode())
    body = b64url(json.dumps(claims, separators=(",", ":")).encode())
    sig = hmac.new(secret.encode(), f"{head}.{body}".encode(), hashlib.sha256).digest()
    return f"{head}.{body}.{b64url(sig)}"


class Peer(kn_proto.Conn):
    """A v7 (1.3.0) peer. `remote_address`: a bridged browser (NET_PACKET_FORWARDED first)."""

    def __init__(self, port, remote_address=None, client_key="", join=True):
        super().__init__("127.0.0.1", port)
        self.closed = False
        self.me = None
        if remote_address:
            self.send(FORWARDED, remote_address.encode())
        self.send(kn_proto.HELLO, bytes([1, 3, 0]))
        if join:
            self.join(client_key=client_key)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end and not self.closed:
            if self.read(timeout=max(0.05, end - time.time())) is None and time.time() < end - 0.05:
                self.closed = True  # read() returned early: the socket closed

    def spawn(self):
        self.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 5
        while time.time() < end and self.me is None and not self.closed:
            p = self.read(until=SPAWN, timeout=1)
            if p is not None and kn_proto.spawn_info(p)["is_you"]:
                self.me = kn_proto.spawn_info(p)
        return self.me

    def say(self, text, v7=False):
        mark = len(self.packets)
        if v7:
            self.send(CHAT_V7, bytes([0]) + text.encode() + b"\0")
        else:
            self.send(CHAT_SEND, text.encode() + b"\0")
        self.pump(0.45)  # the chat burst limit is 3 lines per second
        return mark

    def texts(self, since=0):
        return [p for t, p in self.packets[since:] if t in (CHAT, NOTIFY)]

    def got_text(self, text, since=0):
        return any(text.encode() in p for p in self.texts(since))

    def error(self):
        for t, p in self.packets:
            if t == ERROR:
                return kn_proto.error_info(p)
        return None


def pump_all(peers, seconds):
    end = time.time() + seconds
    while time.time() < end:
        for p in peers:
            p.pump(0.1)


def refused(peer, mark, server_log, command, before):
    """The guest got the notify and nothing else, and the server logged the denial."""
    # broadcasts, and a configured motd line (none by default) on a first spawn
    texts = [p for p in peer.texts(mark) if b" joined the game" not in p and b" left the game" not in p and b"Welcome to " not in p]
    refusals = (b"DEV only", b"Host or developer required", b"premium custom host", b"Only the host or a developer")
    notified = any(any(r in p for r in refusals) for p in texts)
    leaked = [p for p in texts if not any(r in p for r in refusals)]
    logged = f"perms denied: {command} from" in log(server_log)[before:]
    return notified and not leaked and logged, f"notify={notified} other={leaked[:2]} logged={logged}"


def server_a():
    check("A: server up (prod default: perms off, cheats auth)", start_server("a", PORT_A, f"rcon_password rc-pw\nrcon_port {RCON_A}\n"))
    host = Peer(PORT_A)  # loopback: the server's own machine
    guest = Peer(PORT_A, "203.0.113.31")
    victim = Peer(PORT_A, "203.0.113.32")
    peers = [host, guest, victim]
    check("A: 3 players spawned", all(p.spawn() is not None for p in peers))
    host_uid, victim_uid = host.me["uid"], victim.me["uid"]

    # 1. forged chat commands
    forged = [
        ("kick", f"/kick {host_uid}", False), ("kick", "/kick *", False), ("kick", "/kick @humans", True),
        ("ban", f"/ban {host_uid}", False), ("banip", f"/banip {host_uid}", False), ("ip", f"/ip {host_uid}", False),
        ("map", "/map Burg", False), ("end", "/end", True), ("god", "/god *", False), ("kill", "/kill *", False),
        ("bring", "/bring all", False), ("bot_add", "/bot_add 3", False), ("unban", "/unban 1", False),
        ("autofill", "/autofill off", False), ("autofill", "/autofill 8", True),
    ]
    for command, text, v7 in forged:
        before = len(log("a.log"))
        mark = guest.say(text, v7=v7)
        time.sleep(0.15)
        ok, detail = refused(guest, mark, "a.log", command, before)
        check(f"A1: guest {'CHAT_V7' if v7 else 'CHAT_SEND'} {text!r} refused + logged", ok, detail)
        if command == "autofill":
            check("A1: the autofill refusal says who may turn it on or off", guest.got_text("Only the host or a developer can turn autofill on or off", mark))
    check("A1: the denial log names the guest's account (0 = guest)", "account 0)" in log("a.log"))
    check("A1: /ip leaked no address to the guest", not any(b"127.0.0.1" in p or b"203.0.113" in p for p in guest.texts()))
    check("A1: kick * resolved nothing for the guest (no count / no-match reply)",
          not guest.got_text("Kicked") and not guest.got_text("No players match"))
    give_mark = guest.say(f"/give sniper {host_uid}")
    check("A1: guest /give <weapon> <host> refused", guest.got_text("Only developers and the host", give_mark))
    spawn_mark = guest.say("/spawn crate")
    check("A1: guest /spawn refused (the Object Manager's rules)", guest.got_text("only developers and the host can spawn", spawn_mark) and
          not guest.got_text("Spawned", spawn_mark))
    pump_all(peers, 0.5)
    check("A1: nobody was kicked or banned", not host.closed and not victim.closed and not guest.closed)
    check("A1: no ban was written", not os.path.exists(os.path.join(WORK, "a-bans.json")) or "203.0.113" not in open(os.path.join(WORK, "a-bans.json")).read())

    # 2. forged dev packets (the client sends them for the mod's keys)
    for label, ptype, payload in [("prmA", PREMIUM_ACTION, bytes([0])), ("tp", TELEPORT, struct.pack("<3f", 0.0, 50.0, 0.0)),
                                  ("cycle", CYCLE, b""), ("clearLog", CLEAR_LOG, b"")]:
        before = len(log("a.log"))
        mark = len(guest.packets)
        guest.send(ptype, payload)
        guest.pump(0.4)
        ok, detail = refused(guest, mark, "a.log", label, before)
        check(f"A2: guest's forged {label} packet refused + logged", ok, detail)
    check("A2: the map did not cycle", "Burg" not in log("a.log") or "cycle" not in log("a.log").split("perms denied: cycle")[-1])

    # 3. a late FORWARDED claiming the server's own machine
    guest.send(FORWARDED, b"127.0.0.1")
    guest.pump(0.3)
    check("A3: a late FORWARDED is ignored", wait_for(lambda: "ignored FORWARDED from" in log("a.log"), 3))
    before = len(log("a.log"))
    mark = guest.say(f"/kick {host_uid}")
    ok, detail = refused(guest, mark, "a.log", "kick", before)
    check("A3: ... and the guest still cannot /kick", ok and not host.closed, detail)

    # 4. positive: the server's own machine (DEV) and rcon
    mark = host.say(f"/ip {victim_uid}")
    check("A4: host /ip shows the guest's forwarded address", host.got_text("203.0.113.32", mark))
    mark = host.say("/god")
    check("A4: host /god works", host.got_text("God mode ON", mark))
    mark = host.say(f"/kick {victim_uid}")
    pump_all(peers, 0.8)
    err = victim.error()
    check("A4: host /kick of a guest works", host.got_text("Kicked", mark) and victim.closed, f"{err}")
    rc, out = rcon(RCON_A, "status", password="rc-pw")
    check("A4: rcon with the right password works", rc == 0 and "ERR" not in out, out.strip()[-160:])

    # 5. rcon: wrong / blank passwords, then the lockout
    before = len(log("a.log"))
    rc, out = rcon(RCON_A, "status", password="rc-pX")
    check("A5: rcon wrong password refused", rc != 0 and "bad password" in out, out.strip()[-120:])
    time.sleep(1.1)
    check("A5: blank rcon password refused", rcon_raw(RCON_A, "").startswith("ERR"))
    check("A5: rcon refusals logged",  # the server answers ERR before it writes the log line: poll
          wait_for(lambda: log("a.log")[before:].count("[rcon] rejected a connection (bad password)") >= 2, 5))
    for i in range(3):
        time.sleep(1.1)  # a refused connection stays pending for its 1 s delay (at most 4 at once)
        rcon(RCON_A, "status", password=f"guess{i}")
    time.sleep(1.1)
    rc, out = rcon(RCON_A, "status", password="rc-pw")
    check("A5: 5 bad passwords lock rcon (the right one is refused)", rc != 0 and "locked" in out, out.strip()[-120:])
    check("A5: the lockout is logged", wait_for(lambda: "[rcon] 5 bad passwords: locked" in log("a.log"), 5))
    for p in peers:
        p.close()


def server_b():
    check("B: server up (tickets, ops 7)", start_server("b", PORT_B, f"brain_url ws://127.0.0.1:{BRAIN_B}/mm/internal/ws\n"
                                                              f"brain_secret {SECRET_B}\nops 7\nallow_guests 1\n"))
    lobby = wait_for(lambda: (log("b.log").split("[brain] linking to ", 1)[1].split(" as B1:", 1)[1][:5]
                              if "[brain] linking to " in log("b.log") else None), 5)
    check("B: lobby id from the log", bool(lobby), log("b.log")[-300:])
    now = int(time.time())

    def claims(acc, name, nid="B1"):
        return {"nid": nid, "lid": lobby, "jti": secrets.token_hex(8), "acc": acc, "name": name, "exp": now + 120}

    # 6. wrong secret, claiming the op account
    forged = Peer(PORT_B, "203.0.113.41", client_key=ticket("kn-dev-secret", claims(7, "op_acct")))
    forged.pump(1.0)
    err = forged.error()
    check("B6: a ticket signed with the wrong secret is refused", forged.closed and err and "Invalid join key" in err[0], str(err))
    # 7. another node
    other = Peer(PORT_B, "203.0.113.42", client_key=ticket(SECRET_B, claims(7, "op_acct", nid="ZZ9")))
    other.pump(1.0)
    err = other.error()
    check("B7: a ticket for another node is refused", other.closed and err and "another server" in err[0], str(err))
    check("B6/7: both rejections logged", log("b.log").count("Rejecting 203.0.113.4") >= 2)

    victim = Peer(PORT_B, "203.0.113.43")  # a guest (no ticket, allow_guests)
    check("B: a guest spawns", victim.spawn() is not None)
    # 8. chat before the JOIN settles
    early = Peer(PORT_B, "203.0.113.44", join=False)
    early.say(f"/kick {victim.me['uid']}")
    check("B8: chat before the JOIN is refused and logged", wait_for(lambda: "join not verified" in log("b.log"), 3) and not victim.closed)
    early.close()

    # 9. a valid ticket for a non-op account
    user = Peer(PORT_B, "203.0.113.45", client_key=ticket(SECRET_B, claims(8, "plain_acct")))
    check("B9: a valid ticket (account 8) spawns", user.spawn() is not None)
    before = len(log("b.log"))
    mark = user.say(f"/kick {victim.me['uid']}")
    ok, detail = refused(user, mark, "b.log", "kick", before)
    check("B9: account 8 /kick refused", ok and not victim.closed, detail)
    check("B9: the denial names account 8", "account 8)" in log("b.log")[before:])

    # 10. the op account (ops 7) with a valid ticket, from a remote address
    op = Peer(PORT_B, "203.0.113.46", client_key=ticket(SECRET_B, claims(7, "op_acct")))
    check("B10: the op account spawns", op.spawn() is not None)
    check("B10: logged as an op", wait_for(lambda: "is an op (config gameserver.ops)" in log("b.log"), 3))
    mark = op.say(f"/kick {victim.me['uid']}")
    victim.pump(0.8)
    check("B10: the op's /kick works", op.got_text("Kicked", mark) and victim.closed)
    for p in (forged, other, victim, user, op):
        p.close()


def server_c():
    check("C: server up (remote coordinator with the dev secret, rcon on 0.0.0.0 with a short password, cheats all)",
          start_server("c", PORT_C, "brain_url ws://192.0.2.1:9/mm/internal/ws\ncheats all\n"
                                    f"rcon_password short\nrcon_port {RCON_C}\nrcon_host 0.0.0.0\n"))
    wait_for(lambda: "not listening" in log("c.log"), 5)
    check("C11: refuses to link with the public dev secret", "[brain] not linking to ws://192.0.2.1" in log("c.log"))
    check("C11: refuses to open rcon off loopback with a short password", "[rcon] not listening" in log("c.log"))
    guest = Peer(PORT_C, "203.0.113.51")
    check("C: a guest spawns", guest.spawn() is not None)
    mark = guest.say("/god")
    check("C12: perms off + cheats all: a guest's /god works (v3.1.1's open server)", guest.got_text("God mode ON", mark))
    guest.close()


def admin_drain(port, secret, headers=None):
    req = urllib.request.Request(f"http://127.0.0.1:{port}/mm/admin/drain?node=nope", method="POST", data=b"",
                                 headers=dict({"Authorization": f"Bearer {secret}"}, **(headers or {})))
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code


def brain_d():
    root = os.getcwd()
    brain = os.path.join(BUILD, "services", "brain", "kn_brain")
    if not os.path.exists(brain):
        print("SKIP  D: no kn_brain in the build")
        return
    scratch = [sys.executable, "-I", os.path.join(root, "services", "brain", "tests", "db_scratch.py"), root]
    schema = f"cmdauth{os.getpid()}"
    r = subprocess.run(scratch + ["create", "sqlite", schema], capture_output=True)
    if r.returncode:
        print(f"SKIP  D: sqlite scratch database unavailable ({r.stderr.decode(errors='replace').strip()[:120]})")
        return
    try:
        cfg = {"database": dict(json.loads(r.stdout), migrations_dir=os.path.join(root, "services", "db", "migrations")),
               "brain": {"host": "127.0.0.1", "port": 0, "internal_secret": "kn-dev-secret", "federation": False}}
        cfg_path = os.path.join(WORK, "brain.json")
        with open(cfg_path, "w") as f:
            json.dump(cfg, f)
        out = open(os.path.join(WORK, "brain.log"), "w")
        procs.append(subprocess.Popen([brain, "--config", cfg_path, "--migrate"], stdout=out, stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, cwd=root))
        port = wait_for(lambda: int(log("brain.log").split("listening on http://127.0.0.1:", 1)[1].split(" ", 1)[0])
                        if "listening on http://127.0.0.1:" in log("brain.log") else None, 60)
        check("D: kn_brain up", bool(port), log("brain.log")[-300:])
        if not port:
            return
        code = admin_drain(port, "wrong-secret")
        check("D13: admin drain with a wrong secret: 401", code == 401, str(code))
        check("D13: logged", "admin drain: rejected bad/missing auth" in log("brain.log"))
        code = admin_drain(port, "kn-dev-secret", {"X-Forwarded-For": "203.0.113.61"})
        check("D14: the dev default secret through a proxy: 403", code == 403, str(code))
        check("D14: logged", "development default" in log("brain.log"))
        code = admin_drain(port, "kn-dev-secret")
        check("D15: the dev default secret on a direct local connection passes the gate (404 NodeNotLocal)", code == 404, str(code))
        codes = [admin_drain(port, "wrong-secret") for _ in range(30)]
        check("D16: past 30 attempts a minute: 429", codes[-1] == 429 and 429 not in codes[:26], str(codes[-6:]))
        check("D16: logged", "too many attempts from" in log("brain.log"))
    finally:
        subprocess.run(scratch + ["drop", "sqlite", schema], capture_output=True)


def main():
    try:
        server_a()
        server_b()
        server_c()
        brain_d()
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
    failed = results.count(False)
    print(f"\n{len(results) - failed}/{len(results)} passed (logs in {WORK})")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

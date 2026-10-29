#!/usr/bin/env python3
"""Krunker v10.0.4 "Chat-restricted players can now vote to forfeit" on a brain-linked server_bin.

  python3 -I server/tests/chat_restrict_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite) with one DEV account (accounts.developers) and
two players, and one brain-linked server_bin node. The DEV chat-bans one player (POST /moderation/chat, a
feature_bans 'chat' row, v10_mods.c); the account the node gets carries chat_banned (db_account_json). Two bridged
peers sign in live (NET_EXT_KN_ACCOUNT_AUTH by session token):
  R  the chat-banned player: a chat line is dropped (server log "dropped, chat restricted"), another command (/c) is
     refused too, and /ff and /forfeit still run (the forfeit vote command, server/src/net.c net_handle_chat)
  F  the other player: its chat line goes out as usual
Exit 77 when the database is unavailable. Prints PASS/FAIL lines; exit code 0 only when all passed.
"""
import json
import os
import re
import shutil
import signal
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
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
STAMP = os.getpid() % 100000
NAME = "kn_test_chatrestrict_%d" % os.getpid()
SECRET = "kn-chatrestrict-%d" % os.getpid()
PORT = kn_proto.free_ports(1)[0]
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-chatrestrict-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
DEV, BANNED, FREE = "crdev%d" % STAMP, "crban%d" % STAMP, "crfree%d" % STAMP
PW = "pw-chatrestrict-%d" % os.getpid()

CHAT_SEND, FORWARDED, EXT = 8, 85, 254
EXT_AUTH, EXT_RIGHTS = 0x8000, 0x8001
AUTH_SESSION = 1
ST_STATE, ST_OK = 0, 1
RIGHTS_FMT = "<BBBBii32s64s"
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 200)
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if show else ''}", flush=True)


def read_log(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs=10.0):
    end = time.time() + secs
    while time.time() < end:
        try:
            v = fn()
            if v:
                return v
        except Exception:  # noqa: BLE001 - polling
            pass
        time.sleep(0.1)
    return None


def http(port, method, path, body=None, token=None):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if data is not None else {}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


def query(port):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(1.0)
        s.sendto(b"\xff\xff\xff\xffKNINFO\x00e2e", ("127.0.0.1", port))
        data, _ = s.recvfrom(2048)
    return data


class Peer(kn_proto.Conn):
    """A bridged browser on protocol 1.4 (NET_PACKET_EXT), from a TEST-NET address (never the server's own machine)."""

    def __init__(self, port, remote):
        super().__init__("127.0.0.1", port)
        self.seq = 0
        self.send(FORWARDED, remote.encode() + b"\0")
        self.send(kn_proto.HELLO, bytes([1, 4, 2, 1]))
        self.join()
        self.wait_rights(lambda r: r["status"] == ST_STATE, 8)

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            if self.read(timeout=max(0.05, end - time.time())) is None and time.time() < end - 0.05:
                return

    def rights_since(self, mark):
        out = []
        for t, p in self.packets[mark:]:
            if t == EXT and len(p) >= 2 and struct.unpack("<H", p[:2])[0] == EXT_RIGHTS:
                v = struct.unpack(RIGHTS_FMT, p[2:2 + struct.calcsize(RIGHTS_FMT)])
                out.append({"status": v[0], "seq": v[1], "account": v[5], "name": kn_proto.uncstr(v[6])})
        return out

    def wait_rights(self, pred, secs=12, mark=0):
        end = time.time() + secs
        while time.time() < end:
            for r in self.rights_since(mark):
                if pred(r):
                    return r
            self.pump(0.1)
        return None

    def sign_in(self, token):
        self.seq = (self.seq + 1) % 256 or 1
        seq, mark = self.seq, len(self.packets)
        body = struct.pack("<BB32s64s", AUTH_SESSION, seq, kn_proto.cstr("", 32), kn_proto.cstr(token, 64))
        self.send(EXT, struct.pack("<H", EXT_AUTH) + body)
        return self.wait_rights(lambda r: r["seq"] == seq and r["status"] != ST_STATE, 12, mark)

    def say(self, text):
        time.sleep(1.2)  # the chat burst limit and config.chatInterval
        self.send(CHAT_SEND, text.encode() + b"\0")
        self.pump(0.6)


def register(port, name):
    st, reg = http(port, "POST", "/auth/register", {"username": name, "password": PW})
    token = reg.get("token", "") if isinstance(reg, dict) else ""
    check(f"account {name} registered", st == 200 and token, f"{st} {str(reg)[:160]}")
    return token


def main():
    if not os.access(f"{BUILD}/server_bin", os.X_OK) or not os.access(f"{BUILD}/services/brain/kn_brain", os.X_OK):
        print("chat_restrict_e2e: no server_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("chat_restrict_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    db = json.loads(r.stdout)
    if str(db.get("port")) == "5434":  # never the production tunnel
        print("chat_restrict_e2e: refusing database port 5434")
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        return 1
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    node_log = os.path.join(WORK, "node.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=chatrestrict-%d" % os.getpid(),
                                  "--finder.poll_interval_s=1", "--accounts.developers=" + DEV],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        gs = {"region": "local", "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0,
              "bans_file": os.path.join(WORK, "bans.json"), "node_id": "CHATR", "name": "chat restrict e2e",
              "listen_port": PORT, "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % port,
              "brain_secret": SECRET, "finder_url": "http://127.0.0.1:%d" % port}
        cfg = os.path.join(WORK, "node.json")
        with open(cfg, "w") as f:
            json.dump({"gameserver": gs}, f)
        node = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", cfg, "--cfg", "none"], cwd=ROOT,
                                stdout=open(node_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                start_new_session=True)
        procs.append(node)
        check("the node links to the coordinator", wait_for(lambda: "welcomed" in read_log(node_log) or "linked" in read_log(node_log), 20),
              read_log(node_log)[-300:])
        tokens = {n: register(port, n) for n in (DEV, BANNED, FREE)}
        if not all(tokens.values()):
            return 1
        # the chat ban is staff only (players.player_moderator / player_developer, v10_mods.c officer); DEV registers after
        # the brain's startup grant, so it is made a moderator here, on whichever database the scratch run uses
        grant = "UPDATE players SET player_moderator = 1 WHERE player_id = (SELECT id FROM accounts WHERE username = '%s')" % DEV
        if db.get("path"):
            import sqlite3
            con = sqlite3.connect(db["path"], timeout=10)
            try:
                con.execute(grant)
                con.commit()
            finally:
                con.close()
        elif db.get("driver") == "postgres":
            env = dict(os.environ, PGPASSWORD=str(db.get("password", "")),
                       PGOPTIONS="-c search_path=" + str(db.get("search_path", "public")))
            subprocess.run(["psql", "-h", str(db.get("host", "127.0.0.1")), "-p", str(db.get("port", 5432)),
                            "-U", str(db.get("user", "krunknative")), "-d", str(db.get("name", "krunknative")),
                            "-v", "ON_ERROR_STOP=1", "-qAt", "-c", grant], env=env, check=True, capture_output=True)
        st, body = http(port, "POST", "/moderation/chat", {"player_name": BANNED, "ban": True}, token=tokens[DEV])
        data = body.get("data") if isinstance(body, dict) else None
        check("the DEV chat-bans a player (POST /moderation/chat)", st == 200 and isinstance(data, dict) and data.get("chat_banned") is True,
              f"{st} {str(body)[:200]}")

        banned = Peer(PORT, "192.0.2.41")
        ok = banned.sign_in(tokens[BANNED])
        check("the chat-banned player signs in on the node", ok is not None and ok["status"] == ST_OK, str(ok))
        free = Peer(PORT, "192.0.2.42")
        ok = free.sign_in(tokens[FREE])
        check("the other player signs in on the node", ok is not None and ok["status"] == ST_OK, str(ok))

        banned.say("hello from the banned one")
        free.say("hello from the free one")
        banned.say("/c")
        banned.say("/ff")
        banned.say("/forfeit")
        free.pump(1.0)
        text = read_log(node_log)
        check("R: its chat line is dropped (chat restricted)", f"[chat] {BANNED}: dropped, chat restricted" in text
              and "hello from the banned one" not in text, "\n".join(ln for ln in text.splitlines() if BANNED in ln)[-400:])
        check("R: another command (/c) is refused too", f"[cmd] {BANNED}: /c" not in text)
        check("R: /ff still runs (the forfeit vote)", f"[cmd] {BANNED}: /ff" in text)
        check("R: /forfeit still runs", f"[cmd] {BANNED}: /forfeit" in text)
        check("F: the other player's chat goes out", f"[chat] {FREE}: hello from the free one" in text)
        banned.close()
        free.close()
        check("the node still answers", bool(wait_for(lambda: query(PORT), 5)))
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                p.wait()
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
    if results and all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if results and all(results) else 1


if __name__ == "__main__":
    sys.exit(main())

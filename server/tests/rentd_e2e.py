#!/usr/bin/env python3
"""kn_rentd's process loop (server/src/rentd.c, doc/v10/rented-servers.md) against a fake agent API and a fake server_bin.

  rentd_e2e.py <kn_rentd>

The fake brain serves GET /mm/internal/rentals?host=h and records POST /mm/internal/rentals/status (and checks the
Bearer secret); the fake server_bin is a shell script that logs its pid and cfg, then sleeps (or exits at once when
its map is "crash"). Covers: a rented slot is started with its server.cfg; the status says running; a new generation
restarts it (a new pid, the new map); a crash is restarted after the backoff; the end of the rental stops it; a
brain outage keeps the child running; SIGTERM stops everything.
"""
import http.server
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

RENTD = sys.argv[1]
SECRET = "rentd-e2e-secret"
FAILS = []
STATE = {"slots": [], "status": [], "down": False, "bad_auth": 0}


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)


class Brain(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authed(self):
        if self.headers.get("Authorization") != "Bearer " + SECRET:
            STATE["bad_auth"] += 1
            self.reply(401, {"error": "unauthorized"})
            return False
        return True

    def do_GET(self):
        if STATE["down"]:
            return self.reply(503, {"error": "down"})
        if not self.authed():
            return
        if self.path != "/mm/internal/rentals?host=testhost":
            return self.reply(404, {})
        self.reply(200, {"now": int(time.time()), "slots": STATE["slots"]})

    def do_POST(self):
        if not self.authed():
            return
        n = int(self.headers.get("Content-Length", "0"))
        STATE["status"].append(json.loads(self.rfile.read(n)))
        self.reply(200, {"updated": 1})


def slot(gen, run, map_name):
    return {"id": 1, "port": 31024, "name": "Ashburn_1", "region": 0, "public_host": "", "generation": gen, "run": run,
            "map": map_name, "creator": "alice", "key": "", "game_id": 7}


def wait(cond, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if cond():
            return True
        time.sleep(0.2)
    return False


def main():
    work = tempfile.mkdtemp(prefix="kn-rentd-e2e.", dir=os.path.join(os.path.expanduser("~"), ".cache"))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Brain)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]
    runs = os.path.join(work, "runs.log")
    fake = os.path.join(work, "server_bin")
    with open(fake, "w") as f:
        f.write("#!/bin/sh\n"
                "cfg=\"$4\"\n"
                "echo \"$$ $(grep -o 'published:[^\\\"]*' \"$cfg\")\" >> %s\n"
                "grep -q 'published:alice/crash' \"$cfg\" && exit 3\n"
                "trap 'exit 0' TERM\n"
                "while :; do sleep 0.1; done\n" % runs)
    os.chmod(fake, 0o755)
    state_dir = os.path.join(work, "state")
    cfg = os.path.join(work, "rentd.cfg")
    with open(cfg, "w") as f:
        f.write('host_id "testhost"\nbrain_url "http://127.0.0.1:%d"\nserver_bin "%s"\nstate_dir "%s"\n'
                'coordinator_secret "%s"\npoll_s 1\nstop_grace_s 3\nchild_mem_mb 0\npublic_host "pub.example"\n'
                % (port, fake, state_dir, SECRET))
    log = open(os.path.join(work, "rentd.log"), "w")
    proc = subprocess.Popen([RENTD, "--cfg", cfg], stdout=log, stderr=subprocess.STDOUT)

    def started():
        try:
            with open(runs) as f:
                return [l.split() for l in f if l.strip()]
        except OSError:
            return []

    def alive(pid):
        try:
            os.kill(int(pid), 0)
            return True
        except OSError:
            return False

    def last_state():
        for st in reversed(STATE["status"]):
            for s in st.get("slots", []):
                if s["id"] == 1:
                    return s
        return {}

    try:
        STATE["slots"] = [slot(1, False, "")]
        check(wait(lambda: len(STATE["status"]) >= 1), "the agent polls and posts its status")
        check(not started() and last_state().get("state") == "stopped", "a free slot runs nothing (stopped)")
        check(STATE["bad_auth"] == 0 and STATE["status"][0].get("host") == "testhost", "the Bearer secret and the host id")

        STATE["slots"] = [slot(2, True, "Arena")]
        check(wait(lambda: len(started()) == 1), "a rented slot starts a server_bin")
        first = started()[0]
        check(first[1:] == ["published:alice/Arena"], "with the slot's game: %s" % first)
        with open(os.path.join(state_dir, "rent-1.cfg")) as f:
            text = f.read()
        check('hostname        "Ashburn_1"' in text and "dedicated       1" in text and 'public_host     "pub.example"' in text
              and "port            31024" in text, "its server.cfg: name, dedicated, the host's public address, the port")
        check(wait(lambda: last_state().get("state") == "running" and last_state().get("generation") == 2),
              "the status says running generation 2 after 5 s")

        STATE["down"] = True
        time.sleep(3)
        check(alive(first[0]) and len(started()) == 1, "a brain outage keeps the child as it is")
        STATE["down"] = False

        STATE["slots"] = [slot(3, True, "Other")]
        check(wait(lambda: len(started()) == 2), "a new generation restarts it")
        second = started()[1]
        check(second[1:] == ["published:alice/Other"] and not alive(first[0]), "a new pid with the new game, the old one gone")

        STATE["slots"] = [slot(4, True, "crash")]
        check(wait(lambda: len(started()) >= 4, 30), "a crashing server is started again (backoff)")
        check(wait(lambda: last_state().get("state") in ("failed", "starting")), "its state is not running: %s" % last_state())

        STATE["slots"] = [slot(5, True, "Arena")]
        check(wait(lambda: last_state().get("state") == "running" and last_state().get("generation") == 5, 40),
              "a good game after the crashes runs again")
        pid = started()[-1][0]
        STATE["slots"] = [slot(6, False, "")]
        check(wait(lambda: not alive(pid)), "the rental's end stops it")
        check(wait(lambda: last_state().get("state") == "stopped"), "the status says stopped")

        STATE["slots"] = [slot(7, True, "Arena")]
        check(wait(lambda: last_state().get("state") == "running" and last_state().get("generation") == 7), "rented again: running")
        pid = started()[-1][0]
        proc.send_signal(signal.SIGTERM)
        proc.wait(15)
        check(proc.returncode == 0 and not alive(pid), "SIGTERM stops the children and exits 0")
    finally:
        if proc.poll() is None:
            proc.kill()
        srv.shutdown()
        log.close()
    print("rentd e2e: " + ("OK" if not FAILS else "%d failure(s), logs in %s" % (len(FAILS), work)))
    if not FAILS:
        shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if FAILS else 0)


main()

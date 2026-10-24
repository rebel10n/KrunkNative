#!/usr/bin/env python3
"""End-to-end test of the Krunker Strike director (services/strike/director.c,
doc/v10/strike-director.md) against real server_bin processes:

  - 2 Krunker Strike servers (S1 requires a director ticket, S2 checks tickets but admits guests)
    and 1 base server, all heartbeating to the director (gameserver.finder_url);
  - GET /live answers; only the two Strike servers are listed; the base server is verified but never
    handed out (no allocation ever names its port);
  - the x-game-version gate: an old version, a missing one and an old body gameVersion get 426;
  - FRVR's own client flow (Strike 1.37.2 bundle): allocate/private {capacity, region: "us-central",
    customId: "usc1a::<id>"} opens a new lobby with a code and answers {address}; a client joins it
    with the ticket and spawns; a second client joins it by code (customMatchId) and lands on the
    same server; the same room ({roomId} = the customId's id) finds that lobby, full at its capacity
    (409); an unknown code is 404;
  - public allocations ({openSlots: 2} with x-game-version 1.37.2 as FRVR sends, then {}) fill the
    other Strike server (fullest first) and then answer 503, never the private lobby or the base
    server;
  - the game server refuses a forged ticket (wrong secret), a tampered one, a ticket for another
    server, a public ticket into the private lobby, and (S1) a join without a ticket;
  - client_bin (Krunker Strike, headless, KRUNKNATIVE_STRIKE_DIRECTOR) seeks through the director and
    plays; with the director unreachable it falls back to the finder's Strike pool (a fake finder).

  python3 -I services/strike/tests/director_e2e.py BUILD_DIR     (from the repository root)

Ports from server/tests/kn_proto.free_ports(); the director's HTTP port is ephemeral. Every process
started here is stopped at the end; nothing touches a display. Exit 77 when the binaries are missing.
"""
import base64
import hashlib
import hmac
import http.server as httpserver
import json
import os
import re
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "server", "tests"))
import kn_proto  # noqa: E402

BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
DIRECTOR = os.path.join(BUILD, "services", "strike", "kn_strike_director")
SERVER = os.path.join(BUILD, "server_bin")
CLIENT = os.path.join(BUILD, "client_bin")
INIT, SPAWN, ERROR = 2, 4, 30
HELLO_STRIKE = bytes([1, 4, 2, (1 << 2) | (1 << 4)])  # rulesets byte: NET_HELLO_RULESETS + flavor strike
SECRET = secrets.token_hex(24)
MAXP = 3
results = []
procs = []
WORK = None


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if detail and not ok else ''}", flush=True)


def note(text):
    print(f"NOTE  {text}", flush=True)


# ------------------------------------------------------------------ tickets (director_ticket.h)

def b64url(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def b64url_dec(s):
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def sign_ticket(secret, claims):
    """A director ticket as services/strike/director_ticket.c signs it: HS256 JWS under
    HMAC-SHA256(secret, "kn-strike-director-v1") hex (kn_signed_token_create)."""
    key = hmac.new(secret.encode(), b"kn-strike-director-v1", hashlib.sha256).hexdigest().encode()
    now = int(time.time())
    body = dict(claims, iat=now, exp=now + 60)
    signing = b64url(b'{"alg":"HS256","typ":"JWT"}') + "." + b64url(json.dumps(body, separators=(",", ":")).encode())
    return signing + "." + b64url(hmac.new(key, signing.encode(), hashlib.sha256).digest())


def ticket_claims(ticket):
    return json.loads(b64url_dec(ticket.split(".")[1]))


# ------------------------------------------------------------------ processes

def start_server(name, port, extra):
    cfg = os.path.join(WORK, f"{name}.cfg")
    with open(cfg, "w") as f:
        f.write(f'hostname "{name}"\nport {port}\nregion local\nmaxplayers {MAXP}\nmapcycle Sandstorm\nmode ffa\n'
                f'join_timeout 3\n{extra}')
    log = os.path.join(WORK, f"{name}.log")
    p = subprocess.Popen([SERVER, "--headless", "--cfg", cfg, "--config", os.path.join(WORK, "none.json")],
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    procs.append(p)
    return log


def start_director(args):
    log = os.path.join(WORK, "director.log")
    env = dict(os.environ, KN_LOG_LEVEL="info")
    p = subprocess.Popen([DIRECTOR, "--config", os.path.join(WORK, "none.json"), "--strike_director.host=127.0.0.1",
                          "--strike_director.port=0", f"--strike_director.ticket_secret={SECRET}", *args],
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env)
    procs.append(p)
    end = time.time() + 10
    while time.time() < end and p.poll() is None:
        m = re.search(r"listening on http://[^:]+:(\d+)", read(log))
        if m:
            return "http://127.0.0.1:%s" % m.group(1), log
        time.sleep(0.1)
    return None, log


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_port(port, timeout=20.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), 0.5).close()
            return True
        except OSError:
            time.sleep(0.2)
    return False


def wait_for(fn, timeout=20, interval=0.25):
    end = time.time() + timeout
    while time.time() < end:
        if fn():
            return True
        time.sleep(interval)
    return False


# ------------------------------------------------------------------ HTTP

def http(method, url, body=None, headers=None):
    data = json.dumps(body).encode() if body is not None else None
    h = {"Content-Type": "application/json"} if data is not None else {}
    h.update(headers or {})
    req = urllib.request.Request(url, data=data, headers=h, method=method)
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status, json.loads(r.read().decode() or "null")
    except urllib.error.HTTPError as e:
        text = e.read().decode()
        try:
            return e.code, json.loads(text)
        except ValueError:
            return e.code, text


# ------------------------------------------------------------------ game connections

def join(port, ticket, hold=False):
    """HELLO (strike) + JOIN with ticket (client_key), then toward a spawn. Returns
    (spawned, error_message, conn-or-None)."""
    c = kn_proto.Conn("127.0.0.1", port)
    c.send(kn_proto.HELLO, HELLO_STRIKE)
    c.join(client_key=ticket or "", name="dir")
    c.read(until=INIT, timeout=5)
    refused = c.read(until=ERROR, timeout=1.0) is not None
    spawned = False
    if not refused:
        c.ent(class_index=0, lag_comp=1.0)
        end = time.time() + 8
        while time.time() < end and not spawned:
            p = c.read(until=SPAWN, timeout=1)
            spawned = p is not None and bool((kn_proto.spawn_info(p) or {}).get("is_you"))
            if any(t == ERROR for t, _ in c.packets):
                break
    err = next((kn_proto.error_info(p)[0] for t, p in c.packets if t == ERROR), None)
    if hold and spawned:
        return spawned, err, c
    c.close()
    return spawned, err, None


def assets_dir():
    for cand in [os.environ.get("KRUNKNATIVE_ASSETS_DIR"), os.path.join(REPO, "assets")]:
        if cand and os.path.exists(os.path.join(cand, "css", "fonts", "font2.ttf")):
            return os.path.abspath(cand)
    return None


def run_client(name, director_url, finder_url, timeout, assets):
    """client_bin as Krunker Strike, headless, an input script with a time limit; the director (and
    the finder) from the environment, no account service (mode offline: no prod contact). At debug
    level 2 (KRUNKNATIVE_DEBUG, doc/console.md): the checks read the "[strike] director allocated"
    (level 1) and "[strike] finder pool" (level 2) lines."""
    cfg = os.path.join(WORK, f"{name}-cfg")
    os.makedirs(cfg, exist_ok=True)
    script = os.path.join(WORK, f"{name}.script")
    with open(script, "w") as f:
        f.write("0 play\n4000 state\n5000 quit\n")
    env = {k: v for k, v in os.environ.items()
           if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL", "KRUNKNATIVE_UI_DIR", "KN_FLAVOR")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, f"{name}-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "640x360",
                "KRUNKNATIVE_ASSETS_DIR": assets, "KN_FLAVOR": "strike", "KRUNKNATIVE_MODE": "offline",
                "KRUNKNATIVE_STRIKE_DIRECTOR": director_url, "KRUNKNATIVE_FINDER_URL": finder_url, "KRUNKNATIVE_DEBUG": "2"})
    log = os.path.join(WORK, f"{name}.log")
    with open(log, "w") as out:
        p = subprocess.Popen(["stdbuf", "-oL", CLIENT], stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env)
        procs.append(p)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
    return read(log)


class FakeFinder:
    """GET /servers?flavor=strike&space=1 listing one Strike server (the finder pool fallback)."""

    def __init__(self, entry):
        outer = self
        self.requests = []

        class H(httpserver.BaseHTTPRequestHandler):
            def do_GET(self):
                outer.requests.append(self.path)
                body = json.dumps({"ok": True, "servers": [entry]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass

        self.httpd = httpserver.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.url = "http://127.0.0.1:%d/servers" % self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def close(self):
        self.httpd.shutdown()
        self.httpd.server_close()


# ------------------------------------------------------------------ the test

def main():
    for b in (DIRECTOR, SERVER):
        if not os.path.exists(b):
            print(f"SKIP  {b} missing")
            return 77
    with open(os.path.join(WORK, "none.json"), "w") as f:
        f.write("{}")
    proto = re.search(r"protocol (\d+\.\d+)", subprocess.run([SERVER, "--version"], capture_output=True, text=True).stdout)
    version = proto.group(1) if proto else "1.4"

    url, dlog = start_director(["--strike_director.poll_interval_s=1", "--strike_director.join_window_s=2",
                                "--strike_director.private_idle_s=600", "--strike_director.region=local"])
    check("director starts (ephemeral port)", url is not None, read(dlog)[-400:])
    if not url:
        return 1
    st, live = http("GET", url + "/live")
    check("GET /live answers ok", st == 200 and live.get("ok") is True, live)

    s1, s2, base = kn_proto.free_ports(3)
    common = f"finder_url {url}\n"
    logs = {
        s1: start_server("Strike-1", s1, common + f"flavor strike\ndirector_secret {SECRET}\ndirector_require_ticket true\n"),
        s2: start_server("Strike-2", s2, common + f"flavor strike\ndirector_secret {SECRET}\n"),
        base: start_server("Base-1", base, common),
    }
    for port, name in ((s1, "Strike-1"), (s2, "Strike-2"), (base, "Base-1")):
        check(f"{name} up", wait_port(port))
    check("Strike-1 checks director tickets (required)", "Strike director tickets checked" in read(logs[s1])
          and "required for every join" in read(logs[s1]))

    def listed():
        st, o = http("GET", url + "/v1/director/servers")
        return sorted(s["port"] for s in o.get("servers", [])) if st == 200 else []

    check("both Strike servers registered, verified and listed", wait_for(lambda: listed() == sorted([s1, s2]), 25),
          listed())
    check("the base server registered and was verified, but is not listed",
          wait_for(lambda: re.search(r"verified 127\.0\.0\.1:%d .*not a Strike server" % base, read(dlog)), 10)
          and base not in listed(), read(dlog)[-600:])
    st, live = http("GET", url + "/live")
    check("/live counts 2 servers and 6 open seats", st == 200 and live.get("servers") == 2 and live.get("open") == 2 * MAXP,
          live)
    allocated_ports = []

    # ---- x-game-version gate
    st, o = http("POST", url + "/v1/director/allocate", {}, {"x-game-version": "0.1"})
    check("old x-game-version -> 426 GameVersionTooOld", st == 426 and o.get("error") == "GameVersionTooOld"
          and o.get("minVersion") == version, (st, o))
    st, o = http("POST", url + "/v1/director/allocate", {})
    check("no game version -> 426", st == 426, (st, o))
    st, o = http("POST", url + "/v1/director/allocate/private", {"gameVersion": "1.0"})
    check("old body gameVersion on allocate/private -> 426", st == 426, (st, o))
    hv = {"x-game-version": version}

    # ---- private lobby: opened with a code, joined by ticket, then by code
    # exactly what FRVR's client sends (allocateCustomPrivateServer: capacity 12 there; 2 here)
    frvr = {"x-game-version": "1.37.2", "x-min-game-version": "1.37.0"}
    st, p1 = http("POST", url + "/v1/director/allocate/private",
                  {"capacity": 2, "region": "us-central", "customId": "usc1a::room-a"}, frvr)
    ok = st == 200 and re.fullmatch(r"[A-Z2-9]{6}", p1.get("code", "")) and p1.get("created") is True
    check("allocate/private {capacity, region, customId} (FRVR's body) opens a new lobby with a code", ok, (st, p1))
    if not ok:
        return 1
    allocated_ports.append(p1["port"])
    P = p1["port"]
    Q = s2 if P == s1 else s1
    code = p1["code"]
    check("the private allocation is a Strike server with a director ticket",
          P in (s1, s2) and p1["host"] == "127.0.0.1" and p1.get("gameId", "").startswith("local:")
          and p1.get("customMatchId") == code and p1.get("roomId") == "room-a" and p1["gameServer"]["port"] == P
          and p1.get("address") == "127.0.0.1:%d" % P, p1)
    claims = ticket_claims(p1["ticket"])
    check("its ticket names the server port, game id and the code",
          claims.get("typ") == "sdt" and claims.get("port") == P and claims.get("prv") == code
          and claims.get("gid") == p1["gameId"], claims)
    spawned, err, c1 = join(P, p1["ticket"], hold=True)
    check("client 1 joins the private lobby with its ticket and spawns", spawned, err)
    st, p2 = http("POST", url + "/v1/director/allocate/private", {"customMatchId": code.lower()}, hv)
    check("a second client gets the same lobby by code (customMatchId)",
          st == 200 and p2.get("port") == P and p2.get("code") == code and p2.get("created") is False, (st, p2))
    allocated_ports.append(p2.get("port"))
    spawned, err, c2 = join(P, p2.get("ticket", ""), hold=True)
    check("client 2 joins the private lobby by code and spawns", spawned, err)

    def players(port):
        st, o = http("GET", url + "/v1/director/servers")
        return next((x.get("players") for x in o.get("servers", []) if x["port"] == port), None)

    check("the director's poll sees both players in the private lobby", wait_for(lambda: players(P) == 2, 8), players(P))
    st, p3 = http("POST", url + "/v1/director/allocate/private", {"roomId": "room-a"}, hv)
    check("the same room ({roomId} = the customId's id) finds that lobby: full at capacity 2 -> 409",
          st == 409 and p3.get("error") == "GameFull", (st, p3))
    st, o = http("POST", url + "/v1/director/allocate/private", {"customMatchId": "ZZZZZZ"}, hv)
    check("an unknown code -> 404 LobbyNotFound", st == 404 and o.get("error") == "LobbyNotFound", (st, o))

    # ---- the game server refuses forged / foreign / wrong-lobby tickets
    forged = sign_ticket("not-the-secret", {"typ": "sdt", "gid": p1["gameId"], "port": P, "jti": "f" * 32, "prv": code})
    spawned, err, _ = join(P, forged)
    check("a forged ticket (wrong secret) is refused by the server", not spawned and err == "Invalid join key", err)
    head, payload, sig = p2["ticket"].split(".")
    body = json.loads(b64url_dec(payload))
    body["prv"] = "AAAAAA"
    tampered = head + "." + b64url(json.dumps(body, separators=(",", ":")).encode()) + "." + sig
    spawned, err, _ = join(P, tampered)
    check("a tampered ticket is refused by the server", not spawned and err == "Invalid join key", err)
    public_into_private = sign_ticket(SECRET, {"typ": "sdt", "gid": p1["gameId"], "port": P, "jti": "a" * 32})
    spawned, err, _ = join(P, public_into_private)
    check("a valid public ticket into the private lobby is refused", not spawned and err == "This lobby is private", err)
    other = sign_ticket(SECRET, {"typ": "sdt", "gid": "local:x", "port": base, "jti": "b" * 32})
    spawned, err, _ = join(P, other)
    check("a ticket for another server is refused", not spawned and err == "Join key is for another server", err)
    spawned, err, _ = join(s1, "")
    check("Strike-1 (director_require_ticket) refuses a join without a ticket",
          not spawned and err == "Join through matchmaking", err)

    st, srv = http("GET", url + "/v1/director/servers")
    pe = next((s for s in srv.get("servers", []) if s["port"] == P), {})
    check("the private lobby is marked private (server tag + director), its code never listed",
          wait_for(lambda: "private" in next((s for s in http("GET", url + "/v1/director/servers")[1].get("servers", [])
                                              if s["port"] == P), {}).get("tags", []), 6)
          and pe.get("private") is True and code not in json.dumps(srv), pe)

    # ---- public allocations fill the other Strike server, then 503
    public = []
    for body, headers in (({"openSlots": 2}, frvr), ({"region": "local"}, hv), ({}, hv), ({}, frvr)):
        st, o = http("POST", url + "/v1/director/allocate", body, headers)
        public.append((st, o))
        if st == 200:
            allocated_ports.append(o["port"])
    ok = all(st == 200 and o["port"] == Q and o.get("address") == "127.0.0.1:%d" % Q for st, o in public[:2])
    check(f"public allocations ({{openSlots: 2}} as FRVR sends, then one more) fill the free Strike server ({MAXP} seats)",
          ok, [(st, o.get("port") if isinstance(o, dict) else o) for st, o in public])
    check("then 503 NoAvailableServers (the private lobby is never handed out publicly)",
          all(st == 503 and o.get("error") == "NoAvailableServers" for st, o in public[2:]), public[2:])
    q_ticket = public[0][1].get("ticket", "") if public[0][0] == 200 else ""
    check("public tickets carry no code", q_ticket and "prv" not in ticket_claims(q_ticket))
    spawned, err, _ = join(P, q_ticket)
    check("a public ticket for the other server is refused by the private lobby's server",
          not spawned and err == "Join key is for another server", err)
    spawned, err, _ = join(Q, q_ticket)
    check("the public ticket plays on its own server", spawned, err)
    st, o = http("POST", url + "/v1/director/allocate", {"roomId": "room-b", "region": "local"}, hv)
    check("a room's public allocation while full -> 503", st == 503, (st, o))
    check("the base server was never handed out", base not in allocated_ports and all(p in (s1, s2) for p in allocated_ports),
          allocated_ports)

    for c in (c1, c2):
        if c:
            c.close()

    # ---- client_bin: the Strike seek path through the director, and the finder fallback
    assets = assets_dir()
    if not os.path.exists(CLIENT) or not assets:
        note("client_bin cases SKIPPED (no client_bin or assets/css/fonts)")
        return 0
    time.sleep(3.5)  # the public tickets above stop holding seats after the join window (2 s here)
    st, live = http("GET", url + "/live")
    note(f"/live before the client: {live}")
    text = run_client("client-director", url, "off", 60, assets)
    m = re.search(r"\[strike\] director allocated (\S+) at 127\.0\.0\.1:(\d+)", text)
    check("Strike client_bin seeks through the director (allocate)", m is not None and int(m.group(2)) == Q,
          "\n".join(l for l in text.splitlines() if "[strike]" in l or "[mm]" in l)[-800:])
    check("Strike client_bin joins the allocated server with the ticket and plays",
          "connected to server" in text and "weapon=" in text and "Invalid join key" not in text,
          text[-800:])

    # the director unreachable: the finder's Strike pool (S2 admits ticketless guests)
    dead = kn_proto.free_ports(1)[0]
    finder = FakeFinder({"host": "127.0.0.1", "port": s2, "name": "Strike-2", "players": 0, "max": MAXP,
                         "flavor": "strike", "password": False, "region": "local", "tags": []})
    try:
        text = run_client("client-fallback", "http://127.0.0.1:%d" % dead, finder.url, 60, assets)
    finally:
        finder.close()
    check("director unreachable: the client asks the finder's Strike pool",
          any("flavor=strike" in r for r in finder.requests) and "[strike] finder pool: 127.0.0.1:%d" % s2 in text,
          (finder.requests, "\n".join(l for l in text.splitlines() if "[strike]" in l)[-600:]))
    check("... and plays on that Strike server", "connected to server" in text and "weapon=" in text, text[-600:])
    return 0


def run():
    global WORK
    WORK = tempfile.mkdtemp(prefix="kn-strike-director-e2e-", dir=os.path.expanduser("~/.cache"))
    rc = 1
    try:
        rc = main()
    finally:
        for p in procs:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    p.kill()
                    p.wait()
    if rc == 77:
        shutil.rmtree(WORK, ignore_errors=True)
        return 77
    if results and all(results) and rc == 0:
        shutil.rmtree(WORK, ignore_errors=True)
        print(f"{sum(results)}/{len(results)} passed")
        return 0
    print(f"{sum(results)}/{len(results)} passed  (logs: {WORK})")
    return 1


if __name__ == "__main__":
    sys.exit(run())

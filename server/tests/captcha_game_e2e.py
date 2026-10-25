#!/usr/bin/env python3
"""The in-game captcha and the seek's CaptchaRequired on Box Office servers, end to end (doc/v10/captcha.md): v10.0.3's
"cpt" / "cptR" (deob-10.0.3.js:111424-111437) and its seek retry (:164680-164699), with kn_brain's captcha.enforce on.

  python3 -I server/tests/captcha_game_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on sqlite (in the run's directory under ~/.cache) with captcha.enforce on, one brain-linked server_bin node,
every port from kn_proto.free_ports().

  protocol (no client): the node's welcome turns the captcha on; /mm/seek-game without captchaToken answers 400
    CaptchaRequired, with a solved for=seek challenge (and a fresh validation token) a ticket; a player that joins with it
    gets NET_EXT_CAPTCHA [challenge], and its solved NET_EXT_CAPTCHA_ANSWER is accepted (an empty answer back) and it
    plays on; a wrong number, another connection's answer (a replay) and a peer without EXT packets are closed with
    NET_PACKET_ERROR "CAPTCHA FAILED" (004008)
  client: a headless client_bin seeks at launch: CaptchaRequired, the seek's captcha solved, the seek again, the join,
    the node's "cpt" answered and accepted, and the client is in the game
  off: a second brain without captcha.enforce: the seek needs no captcha and the node asks nothing

Exit 0 when every check passes, 1 otherwise, 77 without kn_brain / server_bin / client_bin.
"""
import base64
import hashlib
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.getcwd()
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
import kn_proto  # noqa: E402

BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-captcha-game-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
SECRET = "kn-captcha-game-e2e-secret-%d" % os.getpid()
EXT = 254
CPT, CPTR = 0x0C00, 0x0C01
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + str(detail)) if detail and not ok else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(fn, secs):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.1)
    return None


def http(port, path):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=10) as r:
            raw, status = r.read(), r.status
    except urllib.error.HTTPError as e:
        raw, status = e.read(), e.code
    try:
        return status, json.loads(raw)
    except ValueError:
        return status, raw.decode(errors="replace")


def solve(ch):
    """the ALTCHA widget's search (altcha.1.0.6.min.js), its value (B())"""
    for n in range(int(ch["maxnumber"]) + 1):
        if hashlib.sha256((ch["salt"] + str(n)).encode()).hexdigest() == ch["challenge"]:
            v = {"algorithm": ch["algorithm"], "challenge": ch["challenge"], "number": n, "salt": ch["salt"],
                 "signature": ch["signature"], "took": 3}
            return base64.b64encode(json.dumps(v, separators=(",", ":")).encode()).decode(), n
    return None, None


def forge(ch, number):
    v = {"algorithm": ch["algorithm"], "challenge": ch["challenge"], "number": number, "salt": ch["salt"],
         "signature": ch["signature"], "took": 3}
    return base64.b64encode(json.dumps(v, separators=(",", ":")).encode()).decode()


def seek(port, captcha=None):
    """the client's seek: a fresh validation token, then /mm/seek-game (with captchaToken when given)"""
    _, tok = http(port, "/mm/generate-token")
    q = "/mm/seek-game?hostname=krunknative&region=local&autoChangeGame=true&validationToken=" + urllib.parse.quote(str(tok).strip())
    if captcha:
        q += "&captchaToken=" + urllib.parse.quote(captcha, safe="")
    return http(port, q)


def ticket(port):
    _, ch = http(port, "/mm/captcha?for=seek")
    payload, _ = solve(ch)
    return seek(port, payload)


def ext(subtype, body=b""):
    return struct.pack("<H", subtype) + body


def ext_of(conn, subtype, seen=0):
    for t, data in conn.packets[seen:]:
        if t == EXT and len(data) >= 2 and struct.unpack("<H", data[:2])[0] == subtype:
            return data[2:]
    return None


def join(rep, minor=4):
    c = kn_proto.Conn("127.0.0.1", int(rep["port"]))
    c.send(kn_proto.HELLO, bytes([1, minor, 0]))
    c.join(client_key=rep.get("clientId", ""), game_id=rep.get("gameId", ""))
    c.ent()
    return c


def read_until(c, pred, secs):
    end = time.time() + secs
    while time.time() < end:
        c.read(timeout=0.3)
        v = pred()
        if v is not None:
            return v
    return None


def error_of(c):
    for t, data in c.packets:
        if t == kn_proto.ERROR:
            return kn_proto.error_info(data)
    return None


def start_brain(log, extra):
    db = os.path.join(WORK, os.path.basename(log) + ".db")
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET)
    p = subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=0", "--brain.region=local", "--database.driver=sqlite",
                          "--database.path=" + db, "--migrate", "--brain.id=cptgame-%d-%d" % (os.getpid(), len(procs)),
                          "--captcha.max_number=20000", "--debug=2"] + extra,
                         cwd=ROOT, env=env, stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    procs.append(p)
    m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read(log)), 30)
    return p, (int(m.group(1)) if m else None)


def start_node(brain_port, node_id, port=None):
    # Pick the port immediately before each launch. A port reserved while the brain and the first
    # client start is often taken by another test by the time the second node starts; retry a bind
    # race with a fresh port instead of reporting NoAvailableServers from a dead node.
    for _ in range(4):
        if port is None:
            port = kn_proto.free_ports(1)[0]
        log = os.path.join(WORK, node_id + ".log")
        cfg = os.path.join(WORK, node_id + ".json")
        with open(cfg, "w") as f:
            json.dump({"gameserver": {"node_id": node_id, "name": "captcha e2e " + node_id, "region": "local", "listen_port": port,
                                      "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
                                      "brain_secret": SECRET, "modes": [0], "maps": ["Burg"], "max_players": 8,
                                      "idle_kick_seconds": 0, "idle_menu_kick_seconds": 0}}, f)
        p = subprocess.Popen([os.path.join(BUILD, "server_bin"), "--headless", "--config", cfg, "--cfg", "none"], cwd=ROOT,
                             stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(p)
        wait_for(lambda: "welcomed" in read(log) or "Cannot listen" in read(log), 20)
        text = read(log)
        if "Cannot listen" not in text:
            wait_for(lambda: http(brain_port, "/mm/game-list")[1].get("games") if isinstance(http(brain_port, "/mm/game-list")[1], dict) else None, 20)
            return p, log
        stop(p)
        port = None
    return p, log


def stop(p):
    if p.poll() is None:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)


def protocol(brain_port, node_log):
    check("the node's welcome turns the in-game captcha on", wait_for(lambda: "the brain enforces the in-game captcha" in read(node_log), 10),
          read(node_log)[-400:])
    st, body = seek(brain_port)
    check("seek-game without captchaToken -> 400 CaptchaRequired (v10.0.3's matchmaker answer)",
          st == 400 and isinstance(body, dict) and body.get("error") == "CaptchaRequired", "HTTP %d %s" % (st, body))
    _, ch = http(brain_port, "/mm/captcha?for=login")
    st, body = seek(brain_port, solve(ch)[0])
    check("seek-game with a login challenge's payload -> CaptchaRequired again", st == 400 and body.get("error") == "CaptchaRequired",
          "HTTP %d %s" % (st, body))
    st, rep = ticket(brain_port)
    check("seek-game with a solved seek challenge and a fresh validation token -> a ticket",
          st == 200 and isinstance(rep, dict) and rep.get("clientId"), "HTTP %d %s" % (st, rep))
    if st != 200:
        return

    # a player that answers: accepted, plays on
    c = join(rep)
    body = read_until(c, lambda: ext_of(c, CPT), 15)
    chal = json.loads(body.rstrip(b"\0").decode()) if body else {}
    check("the joined player gets NET_EXT_CAPTCHA with a for=game ALTCHA challenge",
          chal.get("algorithm") == "SHA-256" and "&for=game" in chal.get("salt", "") and len(chal.get("challenge", "")) == 64, chal)
    payload, _ = solve(chal) if chal else (None, None)
    seen = len(c.packets)
    c.send(EXT, ext(CPTR, payload.encode() + b"\0"))
    ack = read_until(c, lambda: ext_of(c, CPTR, seen), 10)
    check("its solved answer is accepted: an empty NET_EXT_CAPTCHA_ANSWER back (v10.0.3's server cptR)", ack == b"", repr(ack))
    before = len(c.packets)
    read_until(c, lambda: None, 2)
    check("and it plays on (no error, the game's packets keep coming)", error_of(c) is None and len(c.packets) > before,
          "%d packets after the answer" % (len(c.packets) - before))
    check("the node logs the acceptance", re.search(r"\[captcha\] client \d+: accepted", read(node_log)))

    # a wrong number: CAPTCHA FAILED
    st, rep2 = ticket(brain_port)
    c2 = join(rep2)
    body2 = read_until(c2, lambda: ext_of(c2, CPT), 15)
    chal2 = json.loads(body2.rstrip(b"\0").decode()) if body2 else {}
    _, n2 = solve(chal2) if chal2 else (None, 0)
    c2.send(EXT, ext(CPTR, forge(chal2, (n2 or 0) + 1).encode() + b"\0"))
    err = read_until(c2, lambda: error_of(c2), 10)
    check("a wrong number -> NET_PACKET_ERROR \"CAPTCHA FAILED\" (004008), the connection closed",
          err == ("CAPTCHA FAILED", "004008"), err)

    # another connection's answer (the first player's, replayed): CAPTCHA FAILED
    st, rep3 = ticket(brain_port)
    c3 = join(rep3)
    read_until(c3, lambda: ext_of(c3, CPT), 15)
    c3.send(EXT, ext(CPTR, payload.encode() + b"\0"))
    err3 = read_until(c3, lambda: error_of(c3), 10)
    check("another connection's answer (a replay) -> CAPTCHA FAILED", err3 == ("CAPTCHA FAILED", "004008"), err3)

    # a peer that cannot be asked (no EXT packets): CAPTCHA FAILED
    st, rep4 = ticket(brain_port)
    c4 = join(rep4, minor=0)
    err4 = read_until(c4, lambda: error_of(c4), 10)
    check("a peer without EXT packets (protocol minor 0) cannot answer -> CAPTCHA FAILED", err4 == ("CAPTCHA FAILED", "004008"), err4)
    for conn in (c, c2, c3, c4):
        conn.close()


def client_run(brain_port, dead_port):
    script = os.path.join(WORK, "client.script")
    with open(script, "w") as f:
        f.write("0 menu\n9000 players\n9500 quit\n")
    log = os.path.join(WORK, "client.log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": script, "KRUNKNATIVE_WINDOW_SIZE": "640x360", "KRUNKNATIVE_DEBUG": "1",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d" % dead_port, "KRUNKNATIVE_KRUNKER_URL": "https://krunker.io"})
    p = subprocess.Popen([os.path.join(BUILD, "client_bin")], cwd=ROOT, stdout=open(log, "w"), stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    try:
        p.wait(120)
    except subprocess.TimeoutExpired:
        stop(p)
        check("client_bin finished its script", False, "timeout, log " + log)
    return read(log)


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    for b in (BRAIN, os.path.join(BUILD, "server_bin"), os.path.join(BUILD, "client_bin")):
        if not os.access(b, os.X_OK):
            print("captcha_game_e2e: no %s: skipped" % b)
            return 77
    dead_port = kn_proto.free_ports(1)[0]
    try:
        brain_log = os.path.join(WORK, "brain.log")
        brain, brain_port = start_brain(brain_log, ["--captcha.enforce=true"])
        check("kn_brain starts with captcha.enforce on", brain_port is not None and "captcha enforced" in read(brain_log), read(brain_log)[-300:])
        if brain_port is None:
            return 1
        node, node_log = start_node(brain_port, "CPTA")
        protocol(brain_port, node_log)

        log = client_run(brain_port, dead_port)
        check("client: the launch seek is answered CaptchaRequired and the client solves the seek's captcha",
              "seek-game: CaptchaRequired: solving the seek's captcha (attempt 1)" in log and "[captcha] seek: solved" in log,
              [l for l in log.splitlines() if "captcha" in l.lower() or "[mm]" in l][:8])
        check("client: the brain accepted the seek's captcha", re.search(r"seek from \S+: captcha accepted", read(brain_log)))
        check("client: it joined the node and answered its \"cpt\", accepted",
              "[captcha] game: the server asks a security challenge" in log and "[captcha] game: the server accepted the answer" in log,
              [l for l in log.splitlines() if "[captcha]" in l][:8])
        check("client: no CAPTCHA FAILED anywhere", "CAPTCHA FAILED" not in log)
        stop(node)
        stop(brain)

        # captcha.enforce off (the default): nothing is asked
        brain_log2 = os.path.join(WORK, "brain-off.log")
        brain2, brain_port2 = start_brain(brain_log2, [])
        node2, node2_log = start_node(brain_port2, "CPTB")
        st, rep = seek(brain_port2)
        check("off: seek-game needs no captcha", st == 200 and isinstance(rep, dict) and rep.get("clientId"), "HTTP %d %s" % (st, rep))
        if st == 200:
            c = join(rep)
            read_until(c, lambda: None, 4)
            check("off: the node asks no in-game captcha", ext_of(c, CPT) is None and error_of(c) is None)
            c.close()
        check("off: the node's log says nothing of an enforced captcha", "the brain enforces the in-game captcha" not in read(node2_log))
        stop(node2)
        stop(brain2)
    except Exception as e:  # noqa: BLE001
        check("the scenario runs", False, repr(e))
    finally:
        for p in procs:
            stop(p)
    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

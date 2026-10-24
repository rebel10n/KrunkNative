#!/usr/bin/env python3
"""Box Office's captcha from the native client, end to end (doc/v10/captcha.md): a local kn_brain (sqlite, temp dir under
~/.cache) with captcha.enforce on, and headless client_bin runs through the menu UI:

  register through v10's login popup -> the client fetches GET /captcha/challenge?for=register, solves it
  (client/src/net/kn_captcha.c) and the brain accepts the registration; logout; sign in by username -> the same for
  "login"; Start Game in the Host Game window -> "hosting" (the brain has no game server here: its own answer, not a
  captcha refusal). And what a client from before the captcha gets from that brain: refused, with v10.0.3's text.

  python3 -I client/tests/captcha_e2e.py BUILD_DIR      (repository root)
Headless client_bin (KRUNKNATIVE_HEADLESS=1, null audio, no DISPLAY, private XDG dirs), a local server_bin for the
menu's map, ports from kn_proto.free_ports(). 77 = skip (no kn_brain / server_bin / client_bin).
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.getcwd()
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
if not all(os.path.exists(p) for p in (BRAIN, os.path.join(BUILD, "client_bin"), os.path.join(BUILD, "server_bin"))):
    print("needs kn_brain (KRUNKNATIVE_SERVICES), client_bin and server_bin: skip")
    sys.exit(77)
from kn_proto import free_ports  # noqa: E402

CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-captcha-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
STAMP = os.getpid() % 100000
NAME, ADDR, PASS = "cap%d" % STAMP, "cap%d@example.org" % STAMP, "pass_word1"
MISSING = "Please complete the security challenge to continue."  # deob-10.0.3.js:111601
results = []
procs = []


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail and not ok else ''}", flush=True)


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def run_client(name, script, brain_url, game_port, timeout=150):
    """One headless client_bin run of `script`; returns its log text."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(WORK, "cfg"),
                "XDG_STATE_HOME": os.path.join(WORK, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_DEBUG": "1"})
    with open(log, "w") as out:
        p = subprocess.Popen([os.path.join(BUILD, "client_bin"), "--mode", "online", "--brain", brain_url, "--finder", "off",
                              "--connect", "127.0.0.1:%d" % game_port], cwd=ROOT, stdout=out, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
    return read(log)


def post(url, body):
    req = urllib.request.Request(url, data=json.dumps(body).encode(), method="POST", headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


OPEN = "0 menu\n500 clickid knV10LoginBtn\n"  # a guest: the header bar's "Login or Register" opens v10's login popup


def scenario(brain_url, game_port, brain_log):
    # 1. register through the popup's Register card (email, Tab, username, Tab, password, Register)
    log = run_client("register", OPEN + f"""800 clickid knV10AuthSwitch
1200 clickid accEmail
1300 type {ADDR}
1400 down TAB
1410 up TAB
1500 type {NAME}
1600 down TAB
1610 up TAB
1700 type {PASS}
1800 clickid knV10LoginSubmit
6000 quit
""", brain_url, game_port)
    check("the client solved the register challenge", "[captcha] register: solved" in log,
          "; ".join(re.findall(r"\[captcha\].*", log)) or "no [captcha] line")
    check("register through the native form, with captcha.enforce on", f"[account] logged in as {NAME}" in log,
          "; ".join(re.findall(r"\[account\].*", log)[-3:]))
    check("the brain accepted it", re.search(r"register from \S+: captcha accepted", read(brain_log)))

    # 2. logout (a fresh run starts as a guest), then sign in by username with Enter
    log = run_client("login", OPEN + f"""800 clickid knV10LoginMode
1000 clickid accName
1100 type {NAME}
1200 clickid accPass
1300 type {PASS}
1400 down ENTER
1410 up ENTER
6000 quit
""", brain_url, game_port)
    check("the client solved the login challenge", "[captcha] login: solved" in log,
          "; ".join(re.findall(r"\[captcha\].*", log)) or "no [captcha] line")
    check("sign in with captcha.enforce on", f"[account] logged in as {NAME}" in log, "; ".join(re.findall(r"\[account\].*", log)[-3:]))
    check("the brain accepted it", re.search(r"login from \S+: captcha accepted", read(brain_log)))

    # 3. Start Game on Box Office servers: the hosting captcha, then /mm/host-game with &captcha=
    log = run_client("host", """0 menu
500 call showWindow 2
1500 clickid hostGameBtn
1800 call knHostFlow "map" "kanji"
1900 clickid startServBtn
5000 text @msg [id=hostGameMsg]
5500 quit
""", brain_url, game_port)
    check("the client solved the hosting challenge", "[captcha] hosting: solved" in log,
          "; ".join(re.findall(r"\[captcha\].*", log)) or "no [captcha] line")
    check("the brain accepted the hosting captcha (its answer is about servers, not the captcha)",
          re.search(r"hosting from \S+: captcha accepted", read(brain_log)) and not re.search(r"hosting from \S+ refused", read(brain_log)))

    # 4. a client from before the captcha (no b) against this brain: refused, with v10.0.3's text
    st, body = post(brain_url + "/auth/login/username", {"username": NAME, "password": PASS})
    check("an older client's sign-in is refused: 400 '%s'" % MISSING, st == 400 and isinstance(body, dict) and body.get("error") == MISSING,
          "HTTP %d %s" % (st, json.dumps(body)[:160]))
    check("no captcha this client sent was refused", not re.search(r"(login|register|hosting) from \S+ refused: captcha (?!missing)",
                                                                       read(brain_log)))


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    brain_port, game_port = free_ports(2)
    db = os.path.join(WORK, "coordinator.db")
    brain_log = os.path.join(WORK, "brain.log")
    brain_url = "http://127.0.0.1:%d" % brain_port
    try:
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % brain_port, "--database.driver=sqlite",
                                       "--database.path=" + db, "--migrate", "--brain.public_url=" + brain_url, "--debug=2",
                                       "--captcha.enforce=true"],
                                      cwd=ROOT, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                      start_new_session=True))
        procs.append(subprocess.Popen([os.path.join(BUILD, "server_bin"), "--headless", "--cfg", "none", "--port", str(game_port)],
                                      cwd=ROOT, stdout=open(os.path.join(WORK, "server.log"), "w"), stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, start_new_session=True))
        for _ in range(100):
            try:
                with urllib.request.urlopen(brain_url + "/mm/status", timeout=1) as r:
                    if b'"brain"' in r.read():
                        break
            except OSError:
                time.sleep(0.1)
        check("the brain enforces the captcha", "captcha enforced" in read(brain_log), read(brain_log)[-300:])
        scenario(brain_url, game_port, brain_log)
    except Exception as e:  # noqa: BLE001
        check("exception", False, repr(e))
    finally:
        for p in procs:
            if p.poll() is None:
                os.killpg(p.pid, signal.SIGTERM)
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL)
    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

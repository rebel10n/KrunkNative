#!/usr/bin/env python3
"""Developer console `logout` end to end (client/src/ui/ui_console.c cmd_logout -> account_ui.c
account_ui_logout, the menu's Logout) on the v10 page against a real kn_brain.

  python3 -I server/tests/console_logout_e2e.py BUILD_DIR      (run from the repository root)

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite: no server needed; postgres is
config/config.json's local 127.0.0.1:5432) and one brain-linked v10 server_bin node (the menu needs a
loaded game; its port from kn_proto.free_ports()). `logout` is client-local: the node never sees
it, so its command auth is not involved. One headless client_bin, signed in from a saved
krunker_username / krunker_token, driven by an input script:

  1. signed in: v10's signed-in header bar; the token is live in the brain (/auth/me)
  2. the console opens (the toggle key); `logo` + Tab autocompletes to `logout`, Enter runs it:
     "logged out", the guest header bar, the saved token gone from storage and revoked in the
     brain (/auth/me refuses it), the menu's "[account] logged out" path
  3. `logout` again: "not logged in" and nothing else (no second "[account] logged out")
  4. `help logout` prints its line
  5. the game server's log never mentions it

Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). Exit 77 when the database is unavailable.
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

sys.path.insert(0, os.path.join(os.getcwd(), "server/tests"))
import kn_proto  # noqa: E402

BUILD = sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug"
ROOT = os.getcwd()
DRIVER = os.environ.get("KN_TEST_DB_DRIVER", "sqlite")
NAME = "kn_test_conlogout_%d" % os.getpid()
SECRET = "kn-conlogout-%d" % os.getpid()
(GAME_PORT,) = kn_proto.free_ports(1)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-conlogout-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
results = []
procs = []

# The console's input has the focus once it opens (devConsole update(): input.focus()), so the
# typed lines go to it. 4000: the restore of the saved session has signed in.
SCRIPT = """0 menu
4000 vis @before signedInHeaderBar signedOutHeaderBar knV10Name
4100 down GRAVE
4150 up GRAVE
4600 vis @console devConsoleOverlay
4700 type logo
5000 text @hint .modal .hints
5100 down TAB
5150 up TAB
5300 text @input .inputShell input
5400 down ENTER
5450 up ENTER
7000 vis @after signedInHeaderBar signedOutHeaderBar knV10Name
7100 con logout
7600 con help logout
8000 vis @again signedInHeaderBar signedOutHeaderBar
8100 quit
"""


def check(name, ok, detail=""):
    results.append(bool(ok))
    show = detail and (not ok or len(detail) <= 160)
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


def http(port, path, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=data,
                                 headers={"Content-Type": "application/json"} if data else {},
                                 method="POST" if data else "GET")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


def node(brain_port):
    log = os.path.join(WORK, "node.log")
    gs = os.path.join(WORK, "node.json")
    cfg = {"node_id": "CONLO", "name": "conlo e2e node", "region": "local", "listen_port": GAME_PORT,
           "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
           "brain_secret": SECRET, "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0,
           "idle_kick_seconds": 0, "ruleset": "v10", "finder_url": "http://127.0.0.1:%d" % brain_port}
    with open(gs, "w") as f:
        json.dump({"gameserver": cfg}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", gs, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    procs.append(p)
    return log


def lobbies(port):
    code, body = http(port, "/mm/game-list?hostname=krunknative")
    return [g[0] for g in body.get("games", [])] if code == 200 and isinstance(body, dict) else []


def vis(text, label):
    """The `vis @label a=1 b=0` line of an input script log as {id: value}"""
    m = re.search(r"\[input_script\] vis t=\d+ @%s (.*)" % re.escape(label), text)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def text_probe(text, label):
    """The `text @label` line as (value, text)"""
    m = re.search(r"\[input_script\] text t=\d+ @%s n=\d+ checked=\d+ value=(.*?) text=(.*)" % re.escape(label), text)
    return (m.group(1), m.group(2)) if m else (None, None)


def client(name, brain_port, script, storage, secs=90):
    """One headless client_bin on the v10 page, run to its script's quit; returns (log, config dir)."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    for key, value in dict({"knRuleset": "v10"}, **storage).items():
        with open(os.path.join(cfg, "krunknative", key + ".json"), "w") as f:
            f.write(value)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_UPDATES_URL": "off", "KRUNKNATIVE_INPUT_SCRIPT": path,
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_UI_DIR": "ui/v10",
                "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port, "KRUNKNATIVE_NET_TRANSPORT": "tcp",
                "KRUNKNATIVE_FINDER_URL": "http://127.0.0.1:%d/servers" % brain_port})
    p = subprocess.Popen(["stdbuf", "-oL", "-eL", f"{BUILD}/client_bin"], cwd=ROOT, stdout=open(log, "w"),
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
    procs.append(p)
    try:
        p.wait(secs)
    except subprocess.TimeoutExpired:
        os.killpg(p.pid, signal.SIGTERM)
        try:
            p.wait(5)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            p.wait()
        check(name + ": client finished its script", False, "timed out; log " + log)
    return read_log(log), cfg


def logout_case(brain_port, node_log):
    user = "conlo%d" % (os.getpid() % 100000)
    st, reg = http(brain_port, "/auth/register", {"username": user, "password": "pw-conlogout-%d" % os.getpid()})
    token = reg.get("token", "") if isinstance(reg, dict) else ""
    check("account registered", st == 200 and token, str(reg)[:200])
    if not token:
        return
    st, _ = http(brain_port, "/auth/me?token=" + token)
    check("the token is live in the brain", st == 200, str(st))

    text, cfg = client("logout", brain_port, SCRIPT, {"krunker_username": user, "krunker_token": token})
    lines = text.splitlines()
    b = vis(text, "before")
    check("1: signed in: v10's signed-in header", b.get("signedInHeaderBar") == "1" and b.get("signedOutHeaderBar") == "0"
          and b.get("knV10Name") == "1", str(b))
    c = vis(text, "console")
    check("2: the console opens", c.get("devConsoleOverlay") == "1", str(c))
    _, hint = text_probe(text, "hint")
    check("2: `logo` hints logout", hint is not None and "logout" in hint, str(hint))
    value, _ = text_probe(text, "input")
    check("2: Tab autocompletes to `logout`", value is not None and value.strip() == "logout", repr(value))

    def index_of(pattern, start=0):
        for i in range(start, len(lines)):
            if re.search(pattern, lines[i]):
                return i
        return -1

    first = index_of(r"\[console\] \] logout$")
    check("2: the typed line runs `logout`", first >= 0)
    done = index_of(r"\[console\] logged out$", first + 1)
    check("2: \"logged out\"", first >= 0 and done > first)
    check("2: the menu's Logout path ran", index_of(r"^\[account\] logged out$", first + 1) > first >= 0)
    a = vis(text, "after")
    check("2: the guest header bar", a.get("signedOutHeaderBar") == "1" and a.get("signedInHeaderBar") == "0", str(a))
    path = os.path.join(cfg, "krunknative", "krunker_token.json")
    saved = read_log(path).strip() if os.path.exists(path) else ""
    check("2: the saved token is gone", saved in ("", '""'), repr(saved[:16]))
    st, _ = http(brain_port, "/auth/me?token=" + token)
    check("2: the brain revoked the token", st in (401, 403), str(st))

    second = index_of(r"\[console\] \] logout$", first + 1)
    check("3: `logout` again runs", second > first)
    nl = index_of(r"\[console\] not logged in$", second + 1) if second > 0 else -1
    check("3: \"not logged in\"", nl > second > 0)
    tail = lines[second + 1:] if second > 0 else []
    check("3: nothing else (no second logout)", not any(re.search(r"^\[account\] logged out$|\[console\] logged out$", l)
                                                        for l in tail))
    g = vis(text, "again")
    check("3: still the guest header", g.get("signedOutHeaderBar") == "1" and g.get("signedInHeaderBar") == "0", str(g))
    check("4: help logout", index_of(r"\[console\] logout\b.*log out of your account") > 0)
    check("5: the game server never saw it", "[mm] joining" in text
          and not re.search(r"\blogout\b", read_log(node_log), re.I))
    help_count = re.findall(r"\[console\] (\d+) commands:", text)
    if help_count:
        print("      (help: %s commands)" % help_count[-1])


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK):
        print("console_logout_e2e: no client_bin in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("console_logout_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    db = json.loads(r.stdout)
    if str(db.get("port")) == "5434":  # never the production tunnel
        print("console_logout_e2e: refusing database port 5434")
        subprocess.run(scratch + ["drop", DRIVER, NAME], capture_output=True)
        return 1
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in db.items()}
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0",
                                  "--brain.region=local", "--brain.id=conlogout-%d" % os.getpid(),
                                  "--finder.poll_interval_s=1"],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        node_log = node(port)
        listed = wait_for(lambda: len(lobbies(port)) == 1, 20)
        check("the v10 lobby is listed", listed, str(lobbies(port)))
        if not listed:
            return 1
        logout_case(port, node_log)
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

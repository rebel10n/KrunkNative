#!/usr/bin/env python3
"""The account backend follows the connected server in every window (client/src/ui/account_split.c, account_ui.c's brain
gate, doc/v10/editor-social.md), end to end against local stubs only: kn_brain on a scratch database, a krunker.io stub
(server/tests/krunker_account_e2e.py: its API, FRVR auth, matchmaker and game server) and
services/common/tests/fake_smtp.py. Nothing leaves the machine.

  python3 -I client/tests/backend_route_e2e.py BUILD_DIR      (repository root)

One client home holds both accounts: a Box Office login with a stored session (the alt vault) and a krunker.io login
signed in from an official game. The editor and the Krunker Hub are opened the way a player opens them, F6 and
openURL("/social.html"): child windows, which start on the router's server kind (app_window.c, KRUNKNATIVE_APP_BACKEND),
so a child of an official krunker.io game starts on krunker.io.

  1. Setup: the Box Office account is registered (login popup), listed by Add in the Account Manager, and its Use card's
     Login stores the session. The krunker.io account signs in through an official game's card (the Security Challenge
     passes by itself on the stub; the stub's 2FA code is typed).
  2. An official krunker.io game with the stored krunker.io account: the editor it opens is signed in as the krunker.io
     account and its banner never says Box Office (editor_ai.c's state dump). The Krunker Hub it opens makes NO request
     to kn_brain and signs nobody in to Box Office.
  3. The menu (Box Office, the same home): the editor is signed in as the Box Office account from its stored session; the
     Krunker Hub signs it in and talks to kn_brain (the control).
  4. An official krunker.io game as a guest (a fresh home): the editor is not signed in and its banner names krunker.io.
  5. The menu as a guest (a fresh home): the editor's banner names Box Office.

The brain's requests go through a recording TCP forwarder (Recorder): kn_brain itself is untouched. The hub's requests
are the ones after its window opened (its log line "-> hub window"), so the main client's own start-up is not counted.
Headless (KRUNKNATIVE_HEADLESS=1, private XDG dirs); the official game's challenge webview runs under xvfb-run.
Exit 0 when every check passed, 77 when it cannot run.
"""
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

ROOT = os.getcwd()
BUILD = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "cmake-build-debug")
BRAIN = os.path.join(BUILD, "services", "brain", "kn_brain")
sys.path.insert(0, os.path.join(ROOT, "services", "common", "tests"))
sys.path.insert(0, os.path.join(ROOT, "server", "tests"))
if not os.path.exists(BRAIN) or not os.path.exists(os.path.join(BUILD, "client_bin")) or not shutil.which("openssl") \
        or not shutil.which("xvfb-run"):
    print("needs kn_brain, client_bin, openssl and xvfb-run: skip")
    sys.exit(77)
from fake_smtp import FakeSMTP, make_cert  # noqa: E402
from kn_proto import free_ports  # noqa: E402
import krunker_account_e2e as kae  # noqa: E402  (the krunker.io stub; its own WORK dir is removed below)
shutil.rmtree(kae.WORK, ignore_errors=True)

CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-backend-route-", dir=CACHE if os.path.isdir(CACHE) else None)
HOME = os.path.join(WORK, "home")        # the account home: both accounts
FRESH = os.path.join(WORK, "fresh")      # a client home with no account at all
STAMP = os.getpid() % 100000
NAME, ADDR = "br%d" % STAMP, "br%d@example.org" % STAMP  # the Box Office account
PASS = "pass_word1"                      # a fixture password, typed by the scripts only
SMTP_USER, SMTP_PASS = "mailer@caan.test", "fake-br-pass-%d" % os.getpid()
WINDOW = 104                             # ALT_MANAGER_WINDOW = KN_WINDOW_BASE + 4
HUB_OPENED = "-> hub window"             # app_mode.c's "[app] <url> -> <mode> window" line
EDITOR_OPENED = "-> editor window"
BOX_OFFICE = "Box Office"
results = []
procs = []
KIO_ENV = {}
recorder = None


def check(name, ok, detail=""):
    results.append(bool(ok))
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  -- ' + detail) if detail else ''}", flush=True)


class Recorder:
    """A TCP forwarder in front of kn_brain. Every request line a client sends (HTTP or WebSocket upgrade) is kept, in
    arrival order; the bytes go through unchanged."""
    REQ = re.compile(rb"(?m)^(GET|POST|PUT|PATCH|DELETE|HEAD|OPTIONS) (\S+) HTTP/1\.[01]")

    def __init__(self, listen_port, target_port):
        self.target = target_port
        self.lines = []
        self.lock = threading.Lock()
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", listen_port))
        self.srv.listen(64)
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self._pipe, args=(conn,), daemon=True).start()

    def _pipe(self, client):
        try:
            up = socket.create_connection(("127.0.0.1", self.target))
        except OSError:
            client.close()
            return

        def down():  # brain -> client: plain forwarding
            try:
                while True:
                    data = up.recv(65536)
                    if not data:
                        break
                    client.sendall(data)
            except OSError:
                pass
            finally:
                try:
                    client.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

        threading.Thread(target=down, daemon=True).start()
        try:
            while True:
                data = client.recv(65536)
                if not data:
                    break
                with self.lock:
                    for m in self.REQ.finditer(data):
                        self.lines.append("%s %s" % (m.group(1).decode(), m.group(2).decode(errors="replace")))
                up.sendall(data)
        except OSError:
            pass
        finally:
            try:
                up.shutdown(socket.SHUT_WR)
            except OSError:
                pass
            client.close()

    def mark(self):
        with self.lock:
            return len(self.lines)

    def since(self, mark):
        with self.lock:
            return list(self.lines[mark:])

    def close(self):
        try:
            self.srv.close()
        except OSError:
            pass


def env_base(home, brain_url):
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY", "KRUNKNATIVE_BRAIN_URL",
                                                             "KRUNKNATIVE_APP_BACKEND", "KRUNKNATIVE_SCREENSHOT")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": os.path.join(home, "cfg"),
                "XDG_STATE_HOME": os.path.join(home, "state"), "KRUNKNATIVE_NO_BROWSER": "1", "KRUNKNATIVE_UPDATES_URL": "off",
                "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_FINDER_URL": "off", "KRUNKNATIVE_BRAIN_URL": brain_url,
                "KRUNKNATIVE_ASSETS_DIR": kae.ASSETS})
    env.update(KIO_ENV)
    return env


def run(name, argv, home, brain_url, input_script=None, env_extra=None, watch=None, timeout=240):
    """One headless client_bin run under xvfb-run; returns (log text, watch hit). watch = a log line's text: the first
    time the log holds it, the recorder's mark and the log offset are kept (the hub's requests start there). The run's
    process group is ended with it, its child windows too."""
    env = env_base(home, brain_url)
    env.update(env_extra or {})
    if input_script is not None:
        path = os.path.join(WORK, name + ".script")
        with open(path, "w") as f:
            f.write(input_script)
        env["KRUNKNATIVE_INPUT_SCRIPT"] = path
    log = os.path.join(WORK, name + ".log")
    hit = {}
    stop = threading.Event()
    with open(log, "w") as out:
        p = subprocess.Popen(["xvfb-run", "-a", os.path.join(BUILD, "client_bin")] + argv, cwd=ROOT, stdout=out,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env, start_new_session=True)
        procs.append(p)
        if watch:
            def poll():
                while not stop.is_set():
                    try:
                        with open(log, errors="replace") as f:
                            text = f.read()
                    except OSError:
                        text = ""
                    i = text.find(watch)
                    if i >= 0:
                        hit["mark"], hit["offset"] = recorder.mark(), i
                        return
                    time.sleep(0.05)
            threading.Thread(target=poll, daemon=True).start()
        try:
            p.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            check(f"{name}: client_bin finished its script", False, f"timeout, log {log}")
        finally:
            stop.set()
            try:
                os.killpg(p.pid, signal.SIGKILL)  # the run's group, its child windows included
            except OSError:
                pass
            p.wait()
    with open(log, errors="replace") as f:
        return f.read(), hit


def game_argv(brain_url, game_port):
    return ["--mode", "online", "--brain", brain_url, "--finder", "off", "--connect", "127.0.0.1:%d" % game_port]


def text_of(log, label):
    for line in log.splitlines():
        if "[input_script] text " in line and f" @{label} " in line:
            return line
    return ""


def logins_as(log, name):
    return log.count(f"[account] logged in as {name} ")


def register_lines(addr, name):
    return """800 clickid knV10AuthSwitch
1200 clickid accEmail
1300 type %s
1400 down TAB
1410 up TAB
1500 type %s
1600 down TAB
1610 up TAB
1700 type %s
1800 clickid knV10LoginSubmit
""" % (addr, name, PASS)


def setup_box_office(brain_url, game_port):
    """The Box Office account, in three client runs: registered through the login popup (a plain sign-in: nothing is
    stored), listed by Add in the Account Manager of a fresh menu (the alt vault), then Use and its Login from the card,
    which stores the session (a restart signs nobody in, so the card's Login is the sign-in)."""
    log, _ = run("reg_box", game_argv(brain_url, game_port), HOME, brain_url, input_script="\n".join([
        "0 menu",
        "500 clickid knV10LoginBtn",
        register_lines(ADDR, NAME).rstrip(),
        "5000 quit",
    ]) + "\n")
    check("the Box Office account registers (its login popup signs in)", logins_as(log, NAME) >= 1, f"logins {logins_as(log, NAME)}")
    log, _ = run("add_box", game_argv(brain_url, game_port), HOME, brain_url, input_script="\n".join([
        "0 menu",
        "500 call showWindow %d" % WINDOW,
        "1000 clickid knAltUser",
        "1100 type %s" % NAME,
        "1200 clickid knAltPass",
        "1300 type %s" % PASS,
        '1400 clicksel [onclick*="knAltAdd"]',
        '2400 text @added [id="knAltMsg"]',
        "2500 quit",
    ]) + "\n")
    check("Add lists the Box Office login", f"Added {NAME}." in text_of(log, "added"), text_of(log, "added")[-120:])
    log, _ = run("use_box", game_argv(brain_url, game_port), HOME, brain_url, input_script="\n".join([
        "0 menu",
        "500 call showWindow %d" % WINDOW,
        '1000 clicksel [onclick*="knAltUse"]',
        "1500 clickid knV10LoginSubmit",
        "4000 quit",
    ]) + "\n")
    check("the Use card's Login signs the Box Office account in and stores its session", logins_as(log, NAME) == 1,
          f"logins {logins_as(log, NAME)}")
    check("the alt vault holds the Box Office login", os.path.exists(os.path.join(HOME, "cfg", "krunknative", "alt", "vault.bin")))


def setup_krunker(brain_url, game_port, stub):
    """The krunker.io account: an official krunker.io game, its login card (use username instead), the Security Challenge
    passed by the stub, the stub's 2FA code typed. The session is stored for the windows that follow."""
    posts = len(stub.find("POST", "/api/auth/login/username"))
    script = "\n".join([
        "0 menu",
        f'1500 call krunkerJoin "{kae.GAME_PICK}"',
        "9000 call loginOrRegister",
        "9500 clickid knV10LoginMode",
        "9800 clickid accName",
        "9900 type %s" % kae.LOGIN,
        "10100 clickid accPass",
        "10200 type %s" % kae.PASSWORD,
        "10500 clickid knV10LoginSubmit",
    ] + [line for i, d in enumerate(kae.CODE) for line in (f"{14600 + i * 150} clickid knV10Code{i}",
                                                           f"{14660 + i * 150} type {d}")] + [
        "19500 nodes @modalgone [id=\"knV10Login\"]",
        "20000 quit",
    ]) + "\n"
    log, _ = run("setup_kio", game_argv(brain_url, game_port), HOME, brain_url, input_script=script)
    check("krunker.io Login posts once", len(stub.find("POST", "/api/auth/login/username")) - posts == 1)
    check("the krunker.io account is signed in", f"signed in to krunker.io as {kae.LOGIN}" in log)


def editor_dump(name, home, brain_url, game_port, official):
    """The map editor as a player opens it: F6 from the menu, or from an official krunker.io game. The editor is a child
    window (its backend is the router's: app_window.c); it runs the editor script inherited from this run, whose ai:dump
    writes its AI panel state (editor_ai.c). Returns that state, {} when it was not written."""
    path = os.path.join(WORK, name + ".json")
    join = f'1500 call krunkerJoin "{kae.GAME_PICK}"\n' if official else ""
    script = "0 menu\n" + join + "6000 down F6\n6100 up F6\n17000 quit\n"
    env = {"KRUNKNATIVE_EDITOR_SCRIPT": ";".join(["ai:open", "wait:3", "ai:dump:" + path, "wait:0.5"])}
    run(name, game_argv(brain_url, game_port), home, brain_url, input_script=script, env_extra=env, watch=EDITOR_OPENED)
    if not os.path.exists(path):
        return {}
    with open(path) as f:
        return json.load(f)


def hub_window(name, home, brain_url, game_port, official):
    """The Krunker Hub as a player opens it: openURL("/social.html") from the menu, or from an official game. Returns
    (the brain requests the hub made, the log after its window opened, the whole log); requests None = it never opened."""
    join = f'1500 call krunkerJoin "{kae.GAME_PICK}"\n' if official else ""
    script = "0 menu\n" + join + '6000 call openURL "/social.html"\n17000 quit\n'
    log, hit = run(name, game_argv(brain_url, game_port), home, brain_url, input_script=script, watch=HUB_OPENED)
    if not hit:
        return None, "", log
    return recorder.since(hit["mark"]), log[hit["offset"]:], log


def main():
    global recorder
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    brain_port, game_port, http_port, ws_port, proxy_port = free_ports(5)
    stub = kae.Stub(http_port, ws_port)
    cert, key = make_cert(WORK)
    smtp = FakeSMTP("starttls", cert, key, users={SMTP_USER: SMTP_PASS}).start()
    db = os.path.join(WORK, "coordinator.db")
    brain_log = open(os.path.join(WORK, "brain.log"), "w")
    brain_url = "http://127.0.0.1:%d" % proxy_port
    try:
        procs.append(subprocess.Popen([BRAIN, "--brain.host=127.0.0.1", "--brain.port=%d" % brain_port, "--database.driver=sqlite",
                                       "--database.path=" + db, "--migrate", "--smtp.host=127.0.0.1", "--smtp.port=%d" % smtp.port,
                                       "--smtp.tls=starttls", "--smtp.user=" + SMTP_USER, "--smtp.ca_file=" + cert, "--smtp.retries=-1",
                                       "--accounts.email.resend_cooldown_s=1", "--brain.public_url=" + brain_url],
                                      cwd=ROOT, env=dict(os.environ, KN_SMTP_PASSWORD=SMTP_PASS), stdout=brain_log,
                                      stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True))
        procs.append(subprocess.Popen([os.path.join(BUILD, "server_bin"), "--headless", "--cfg", "none", "--port", str(game_port)],
                                      cwd=ROOT, stdout=open(os.path.join(WORK, "server.log"), "w"), stderr=subprocess.STDOUT,
                                      stdin=subprocess.DEVNULL, start_new_session=True))
        recorder = Recorder(proxy_port, brain_port)
        stub.start()
        KIO_ENV.update({"KRUNKNATIVE_KRUNKER_API": "http://127.0.0.1:%d/api" % http_port,
                        "KRUNKNATIVE_KRUNKER_AUTH": "http://127.0.0.1:%d/frvr/v1/auth" % http_port,
                        "KRUNKNATIVE_KRUNKER_MATCHMAKER": "http://127.0.0.1:%d/mm" % http_port,
                        # the footer counts (social / editor /player-count) go to a closed port: a test never reaches krunker.io
                        "KRUNKNATIVE_KRUNKER_SOCIAL": "http://127.0.0.1:9", "KRUNKNATIVE_KRUNKER_EDITOR": "http://127.0.0.1:9",
                        "KRUNKNATIVE_ASSET_FETCH": "off",
                        "KRUNKNATIVE_KRUNKER_CAPTCHA_URL": "http://127.0.0.1:%d%s" % (http_port, kae.CAPTCHA_PATH)})
        for _ in range(100):
            try:
                with urllib.request.urlopen("http://127.0.0.1:%d/mm/status" % brain_port, timeout=1) as r:
                    if b'"brain"' in r.read():
                        break
            except OSError:
                time.sleep(0.1)

        setup_box_office(brain_url, game_port)
        setup_krunker(brain_url, game_port, stub)

        # 2. an official krunker.io game with the stored krunker.io account
        d = editor_dump("ed_official", HOME, brain_url, game_port, official=True)
        check("editor from an official game: signed in with the krunker.io session",
              d.get("signed_in") is True and d.get("account", "").lower() == kae.LOGIN.lower(), json.dumps(d)[:240])
        check("editor from an official game: its backend is krunker.io", d.get("backend") == 1, str(d.get("backend")))
        check("editor from an official game: its banner never says Box Office", BOX_OFFICE not in d.get("banner", ""),
              d.get("banner", ""))
        reqs, after, log = hub_window("hub_official", HOME, brain_url, game_port, official=True)
        check("the hub from an official game opened", reqs is not None, log[-160:] if reqs is None else "")
        if reqs is not None:
            check("the hub from an official game makes no request to kn_brain", reqs == [], repr(reqs[:5]))
            check("the hub from an official game signs nobody in to Box Office", logins_as(after, NAME) == 0,
                  "Box Office sign-ins %d" % logins_as(after, NAME))

        # 3. the menu, the same home: the stored Box Office session
        d = editor_dump("ed_box", HOME, brain_url, game_port, official=False)
        check("editor on Box Office: signed in with the Box Office session", d.get("signed_in") is True and d.get("account") == NAME,
              json.dumps(d)[:240])
        check("editor on Box Office: its backend is Box Office", d.get("backend") == 0, str(d.get("backend")))
        reqs, after, log = hub_window("hub_box", HOME, brain_url, game_port, official=False)
        check("the hub on Box Office opened", reqs is not None, log[-160:] if reqs is None else "")
        if reqs is not None:
            check("the hub on Box Office is signed in with the stored session (the control: its avatar request carries the token)",
                  any(r.startswith("GET /api/account/avatar") for r in reqs), repr(reqs[:4]))
            check("the hub on Box Office talks to kn_brain (the control)", len(reqs) > 0, repr(reqs[:3]))

        # 4. a guest in an official game; 5. a guest on Box Office
        d = editor_dump("ed_guest_official", FRESH, brain_url, game_port, official=True)
        check("editor guest in an official game: not signed in", d.get("signed_in") is False, json.dumps(d)[:240])
        check("editor guest in an official game: the banner names krunker.io", "krunker.io" in d.get("banner", ""), d.get("banner", ""))
        check("editor guest in an official game: the banner never says Box Office", BOX_OFFICE not in d.get("banner", ""),
              d.get("banner", ""))
        d = editor_dump("ed_guest_box", FRESH, brain_url, game_port, official=False)
        check("editor guest on Box Office: the banner names Box Office", BOX_OFFICE in d.get("banner", ""), d.get("banner", ""))

        check("the password is on no client file", not any(PASS.encode() in open(os.path.join(dp, fn), "rb").read()
                                                          for dp, _, fns in os.walk(HOME) for fn in fns))
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
        if recorder:
            recorder.close()
        smtp.stop()
        stub.stop()
        brain_log.close()
    ok = all(results)
    print(f"{'PASS' if ok else 'FAIL'}: {sum(results)}/{len(results)}")
    if ok and not os.environ.get("KN_KEEP"):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print(f"logs kept in {WORK}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

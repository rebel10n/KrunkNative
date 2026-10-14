#!/usr/bin/env python3
"""Sign in with an email, end to end: Krunker v10.0.2's login / register modal (client/src/ui/v10_login.c,
account_ui.c) on the v10 page (ui/) against a real kn_brain (accounts.c SQL_LOGIN_EMAIL, migration
0122_email_login) and one brain-linked server_bin for the menu's map.

  python3 -I server/tests/v10_email_login_e2e.py BUILD_DIR      (run from the repository root)

One headless client_bin run (input script clicking the page by element id), a fresh guest:
  - Login or Register: the login card is v10's Email field (#accEmail "Enter Email", no
    "(optional, Register only)"), the password row waits; the register card is Email first
    ("Enter Email"), then Username and Password
  - Register without an email: "Please enter your email to register."; with a mixed-case email:
    signed in, the brain stores it in lower case
  - Logout (showWindow(5) as a guest: v10's login modal with "Enter Email", the window closed) -> Login with the email: the
    password row shows on Login, then signed in; again with the email in other letter cases; with the
    username ("(use email instead)" toggle); with the email typed into the Username field (the
    "Switch to email login" hint shows, the login still works)
  - a wrong password for the email and an unknown email: the same error text
  - Register with the same email in other letter cases: "Email already in use"
Plus the brain over HTTP (/auth/login): the email (any case, spaces around), {"email": ...} and the
username sign in; a wrong password and an unknown email give the same status and error.

kn_brain on a scratch database (KN_TEST_DB_DRIVER, default sqlite: no server needed), ports from
kn_proto.free_ports(). Needs a headless GL device (KRUNKNATIVE_HEADLESS=1: EGL). 77 = skipped.
"""
import json
import os
import re
import shutil
import signal
import sqlite3
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
NAME = "kn_test_emaillogin_%d" % os.getpid()
SECRET = "kn-emaillogin-%d" % os.getpid()
(GAME_PORT,) = kn_proto.free_ports(1)
CACHE = os.path.expanduser("~/.cache")
WORK = tempfile.mkdtemp(prefix="kn-emaillogin-e2e-", dir=CACHE if os.path.isdir(CACHE) else None)
STAMP = os.getpid() % 100000
USER = "elog%d" % STAMP
PW = "pw-elog-%d" % os.getpid()
MAIL = "elog%d@example.org" % STAMP          # what the brain stores
MAIL_REG = "ELog%d@Example.ORG" % STAMP      # typed at Register
MAIL_MIXED = "eLOG%d@EXAMPLE.org" % STAMP    # typed at a later Login
results = []
procs = []


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
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"null")


def node(brain_port):
    log = os.path.join(WORK, "node.log")
    gs = os.path.join(WORK, "node.json")
    cfg = {"node_id": "ELOG", "name": "email login e2e", "region": "local", "listen_port": GAME_PORT,
           "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:%d/mm/internal/ws" % brain_port,
           "brain_secret": SECRET, "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0,
           "idle_kick_seconds": 0, "ruleset": "v10", "finder_url": "http://127.0.0.1:%d" % brain_port}
    with open(gs, "w") as f:
        json.dump({"gameserver": cfg}, f)
    p = subprocess.Popen([f"{BUILD}/server_bin", "--headless", "--config", gs, "--cfg", "none"], cwd=ROOT,
                         stdout=open(log, "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                         start_new_session=True)
    procs.append(p)


def client(name, brain_port, script, secs=240):
    """One headless client_bin on the v10 page (a fresh guest), run to its script's quit; returns its log."""
    path = os.path.join(WORK, name + ".script")
    with open(path, "w") as f:
        f.write(script)
    cfg = os.path.join(WORK, name + "-cfg")
    os.makedirs(os.path.join(cfg, "krunknative"), exist_ok=True)
    log = os.path.join(WORK, name + ".log")
    env = {k: v for k, v in os.environ.items() if k not in ("DISPLAY", "WAYLAND_DISPLAY")}
    env.update({"KRUNKNATIVE_HEADLESS": "1", "KRUNKNATIVE_AUDIO_BACKEND": "null", "XDG_CONFIG_HOME": cfg,
                "XDG_STATE_HOME": os.path.join(WORK, name + "-state"), "KRUNKNATIVE_NO_BROWSER": "1",
                "KRUNKNATIVE_INPUT_SCRIPT": path, "KRUNKNATIVE_WINDOW_SIZE": "1280x720", "KRUNKNATIVE_BRAIN_URL": "http://127.0.0.1:%d" % brain_port,
                "KRUNKNATIVE_NET_TRANSPORT": "tcp", "KRUNKNATIVE_UPDATES_URL": "off",
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
    return read_log(log)


def vis(text, label):
    """The `vis @label a=1 b=0` line of an input script log as {id: value}"""
    m = re.search(r"\[input_script\] vis t=\d+ @%s (.*)" % re.escape(label), text)
    return dict(kv.split("=", 1) for kv in m.group(1).split()) if m else {}


def nodes(text, label):
    """The match count of the `nodes @label <selector>` line, -1 without one"""
    m = re.search(r"\[input_script\] nodes t=\d+ @%s elements=\d+ match=(\d+)" % re.escape(label), text)
    return int(m.group(1)) if m else -1


def text_of(text, label):
    """The text= of the `text @label <selector>` line, None without one"""
    m = re.search(r"\[input_script\] text t=\d+ %s n=(\d+) .*?text=(.*)" % re.escape("@" + label), text)
    return m.group(2).strip() if m and m.group(1) != "0" else None


def rect_y(text, label, elem):
    m = re.search(r"\[input_script\] rect t=\d+ @%s .*?%s=([-\d.]+),([-\d.]+)" % (re.escape(label), re.escape(elem)), text)
    return float(m.group(2)) if m else None


SIGNED = "signedInHeaderBar signedOutHeaderBar knV10Login"


def logout(t, label):
    """logoutAcc (showWindow(5) as a guest: the login modal, the window closed), then ESC twice:
    the first blurs the modal's focused Email field, the second closes the modal"""
    return (f"{t} call logoutAcc\n"
            f"{t + 600} nodes @{label}w5 [id=knV10Login] [id=accEmail][placeholder=\"Enter Email\"]\n"
            f"{t + 600} nodes @{label}w5old [id=menuWindow] [id=referralHeader]\n"
            f"{t + 600} nodes @{label}w5opt [placeholder*=\"optional\"]\n"
            f"{t + 700} down ESC\n{t + 720} up ESC\n"
            f"{t + 800} down ESC\n{t + 820} up ESC\n"
            f"{t + 1000} vis @{label} {SIGNED}\n")


def email_login(t, label, typed, pw):
    """Login or Register -> the Email field -> Login (the password row shows) -> the password -> Login"""
    return (f"{t} clickid knV10LoginBtn\n"
            f"{t + 400} vis @{label}card accEmail accName knV10PassRow knV10LoginMode\n"
            f"{t + 450} clickid accEmail\n{t + 500} type {typed}\n"
            f"{t + 600} clickid knV10LoginSubmit\n"
            f"{t + 900} vis @{label}pass knV10PassRow accPass\n"
            f"{t + 950} clickid accPass\n{t + 1000} type {pw}\n"
            f"{t + 1100} clickid knV10LoginSubmit\n"
            f"{t + 6000} text @{label}resp [id=accResp]\n"
            f"{t + 6000} vis @{label} {SIGNED}\n")


def script():
    s = ["0 menu", "500 vis @guest " + SIGNED]
    # the login card of a fresh guest: v10's Email field
    s += ["600 clickid knV10LoginBtn",
          "1000 vis @login accEmail accName accPass knV10PassRow knV10LoginMode",
          '1000 nodes @loginph [id=accEmail][placeholder="Enter Email"]',
          '1000 nodes @loginopt [placeholder*="optional"]',
          "1000 text @loginlabel label[for=accEmail]",
          "1000 text @loginmode [id=knV10LoginMode]",
          # the register card: Email first, then Username and Password
          "1100 clickid knV10AuthSwitch",
          "1500 vis @register accEmail accName accPass",
          '1500 nodes @regph [id=accEmail][placeholder="Enter Email"]',
          '1500 nodes @regopt [placeholder*="optional"]',
          "1500 text @reglabel label[for=accEmail]",
          "1500 rect @regorder accEmail accName accPass",
          # no email: v10's message, nothing sent
          "1600 clickid accName", "1650 type " + USER,
          "1700 clickid accPass", "1750 type " + PW,
          "1800 clickid knV10LoginSubmit",
          "2300 text @noemail [id=accResp]",
          "2300 vis @noemailstill " + SIGNED,
          "2400 clickid accEmail", "2450 type " + MAIL_REG,
          "2500 clickid knV10LoginSubmit",
          "8000 vis @registered " + SIGNED]
    s += logout(8100, "out1").strip().split("\n")
    s += email_login(9500, "mail", MAIL, PW).strip().split("\n")              # -> 15500
    s += logout(15600, "out2").strip().split("\n")
    s += email_login(17000, "mixed", MAIL_MIXED, PW).strip().split("\n")      # -> 23000
    s += logout(23100, "out3").strip().split("\n")
    # the username: "(use email instead)" -> the Username field
    s += ["24500 clickid knV10LoginBtn",
          "24900 clickid knV10LoginMode",
          "25200 vis @usercard accName accEmail knV10PassRow knV10EmailHint",
          "25200 text @userlabel label[for=accName]",
          "25250 clickid accName", "25300 type " + USER,
          "25400 clickid accPass", "25450 type " + PW,
          "25500 clickid knV10LoginSubmit",
          "31000 vis @user " + SIGNED]
    s += logout(31100, "out4").strip().split("\n")
    # an email typed as a username: the hint, and it signs in all the same
    s += ["32500 clickid knV10LoginBtn",
          "32900 clickid knV10LoginMode",
          "33200 clickid accName", "33250 type " + MAIL,
          "33500 vis @hint knV10EmailHint",
          "33550 clickid accPass", "33600 type " + PW,
          "33700 clickid knV10LoginSubmit",
          "39000 vis @hintlogin " + SIGNED]
    s += logout(39100, "out5").strip().split("\n")
    # a wrong password for the email, then an unknown email: the same answer
    s += email_login(40500, "wrongpw", MAIL, PW + "x").strip().split("\n")    # -> 46500
    s += ["46600 call knV10LoginClose"]
    s += email_login(47000, "unknown", "nobody%d@example.org" % STAMP, PW).strip().split("\n")  # -> 53000
    s += ["53100 call knV10LoginClose"]
    # Register again with the address in other letter cases: one account per email
    s += ["53500 clickid knV10LoginBtn",
          "53900 clickid knV10AuthSwitch",
          "54200 clickid accEmail", "54250 type " + MAIL_MIXED,
          "54300 clickid accName", "54350 type " + USER + "b",
          "54400 clickid accPass", "54450 type " + PW,
          "54500 clickid knV10LoginSubmit",
          "60000 text @dupresp [id=accResp]",
          "60000 vis @dup " + SIGNED,
          "60100 quit"]
    return "\n".join(s) + "\n"


def signed_in(v):
    return v.get("signedInHeaderBar") == "1" and v.get("signedOutHeaderBar") == "0" and v.get("knV10Login") == "-"


def ui_case(brain_port, db_path):
    text = client("ui", brain_port, script())
    g = vis(text, "guest")
    check("guest header bar", g.get("signedOutHeaderBar") == "1", str(g))
    m = vis(text, "login")
    check("login card: the Email field (no Username field), the password row waits",
          m.get("accEmail") == "1" and m.get("accName") == "-" and m.get("knV10PassRow") == "0" and m.get("knV10LoginMode") == "1", str(m))
    check('login card: the email placeholder reads "Enter Email"', nodes(text, "loginph") == 1, str(nodes(text, "loginph")))
    check('login card: no "(optional, ...)" placeholder', nodes(text, "loginopt") == 0, str(nodes(text, "loginopt")))
    check('login card: label "Email", toggle "(use username instead)"',
          text_of(text, "loginlabel") == "Email" and text_of(text, "loginmode") == "(use username instead)",
          "%s / %s" % (text_of(text, "loginlabel"), text_of(text, "loginmode")))
    r = vis(text, "register")
    check("register card: Email, Username and Password", r.get("accEmail") == "1" and r.get("accName") == "1" and r.get("accPass") == "1", str(r))
    check('register card: the email placeholder reads "Enter Email", label "Email"',
          nodes(text, "regph") == 1 and text_of(text, "reglabel") == "Email", "%d %s" % (nodes(text, "regph"), text_of(text, "reglabel")))
    check('register card: no "(optional, Register only)"', nodes(text, "regopt") == 0, str(nodes(text, "regopt")))
    ys = [rect_y(text, "regorder", e) for e in ("accEmail", "accName", "accPass")]
    check("register card: Email first, then Username, then Password", None not in ys and ys[0] < ys[1] < ys[2], str(ys))
    check("Register without an email: v10's message", text_of(text, "noemail") == "Please enter your email to register.",
          str(text_of(text, "noemail")))
    check("Register without an email: still a guest", vis(text, "noemailstill").get("signedOutHeaderBar") == "1",
          str(vis(text, "noemailstill")))
    check("Register with an email: signed in", signed_in(vis(text, "registered")), str(vis(text, "registered")))
    try:
        con = sqlite3.connect(db_path) if db_path else None
        row = con.execute("SELECT email FROM accounts WHERE lower(username) = lower(?)", (USER,)).fetchone() if con else None
        if con:
            con.close()
        if db_path:
            check("the brain stored the email trimmed, in lower case", row and row[0] == MAIL, str(row))
    except sqlite3.Error as e:
        check("the brain stored the email", False, str(e))
    for out in ("out1", "out2", "out3", "out4", "out5"):
        o = vis(text, out)
        check(f"{out}: logged out", o.get("signedOutHeaderBar") == "1" and o.get("signedInHeaderBar") == "0", str(o))
    check('logout -> showWindow(5) as a guest: v10\'s login modal, "Enter Email", no "(optional, ...)", no v7 guest form',
          nodes(text, "out1w5") == 1 and nodes(text, "out1w5opt") == 0 and nodes(text, "out1w5old") == 0,
          "%d %d %d" % (nodes(text, "out1w5"), nodes(text, "out1w5opt"), nodes(text, "out1w5old")))
    c = vis(text, "mailcard")
    check("after logout: the login card is the Email field again", c.get("accEmail") == "1" and c.get("accName") == "-", str(c))
    p = vis(text, "mailpass")
    check("Login with only the email: the password row shows", p.get("knV10PassRow") == "1" and p.get("accPass") == "1", str(p))
    check("Login with the email: signed in", signed_in(vis(text, "mail")), "%s %s" % (vis(text, "mail"), text_of(text, "mailresp")))
    check("Login with the email in other letter cases: signed in", signed_in(vis(text, "mixed")),
          "%s %s" % (vis(text, "mixed"), text_of(text, "mixedresp")))
    u = vis(text, "usercard")
    check('"(use email instead)" toggle: the Username field with its password row',
          u.get("accName") == "1" and u.get("accEmail") == "-" and u.get("knV10PassRow") == "1" and u.get("knV10EmailHint") == "0", str(u))
    check('Username field: label "Username"', text_of(text, "userlabel") == "Username", str(text_of(text, "userlabel")))
    check("Login with the username: signed in", signed_in(vis(text, "user")), str(vis(text, "user")))
    check('an email typed as a username: "Switch to email login" shows', vis(text, "hint").get("knV10EmailHint") == "1", str(vis(text, "hint")))
    check("an email typed as a username: signed in", signed_in(vis(text, "hintlogin")), str(vis(text, "hintlogin")))
    wrong, unknown = text_of(text, "wrongpwresp"), text_of(text, "unknownresp")
    check("a wrong password: not signed in, an error", vis(text, "wrongpw").get("signedOutHeaderBar") == "1" and wrong
          and wrong != "Please wait...", str(wrong))
    check("an unknown email: not signed in", vis(text, "unknown").get("signedOutHeaderBar") == "1", str(vis(text, "unknown")))
    check("a wrong password and an unknown email: the same error", wrong is not None and wrong == unknown, "%r vs %r" % (wrong, unknown))
    check("Register with a taken email (other case): refused", text_of(text, "dupresp") == "Email already in use"
          and vis(text, "dup").get("signedOutHeaderBar") == "1", "%s %s" % (text_of(text, "dupresp"), vis(text, "dup")))


def http_case(port):
    for label, body in (("the email", {"username": MAIL, "password": PW}),
                        ("the email, other case + spaces", {"username": "  " + MAIL_MIXED + " ", "password": PW}),
                        ('{"email": ...}', {"email": MAIL_REG, "password": PW}),
                        ("the username", {"username": USER, "password": PW})):
        st, rep = http(port, "/auth/login", body)
        ok = st == 200 and isinstance(rep, dict) and rep.get("token") and (rep.get("account") or {}).get("username") == USER
        check("HTTP /auth/login with %s" % label, ok, "%s %s" % (st, str(rep)[:200]))
    time.sleep(6)  # past the (name, IP) login throttle (accounts.login_throttle_s, 5)
    st1, wrong = http(port, "/auth/login", {"username": MAIL, "password": PW + "x"})
    st2, unknown = http(port, "/auth/login", {"username": "nobody-http%d@example.org" % STAMP, "password": PW})
    check("HTTP: a wrong password and an unknown email give the same status and error",
          st1 == st2 and st1 != 200 and wrong == unknown, "%s %s / %s %s" % (st1, wrong, st2, unknown))
    time.sleep(6)
    st3, rep = http(port, "/auth/login", {"username": MAIL, "password": PW})
    check("HTTP: the account still signs in after the failures", st3 == 200, "%s %s" % (st3, str(rep)[:200]))


def main():
    if not os.access(f"{BUILD}/client_bin", os.X_OK) or not os.access(f"{BUILD}/services/brain/kn_brain", os.X_OK):
        print("v10_email_login_e2e: no client_bin / kn_brain in %s: skipped" % BUILD)
        return 77
    scratch = [sys.executable, "-I", os.path.join(ROOT, "services", "brain", "tests", "db_scratch.py"), ROOT]
    r = subprocess.run(scratch + ["create", DRIVER, NAME], capture_output=True)
    if r.returncode:
        print("v10_email_login_e2e: %s unavailable: skipped" % DRIVER)
        return r.returncode
    dbcfg = json.loads(r.stdout)
    dbenv = {"KN_DATABASE_" + k.upper(): str(v) for k, v in dbcfg.items()}
    db_path = dbcfg.get("path") if DRIVER == "sqlite" else None
    env = dict(os.environ, KN_BRAIN_INTERNAL_SECRET=SECRET, **dbenv)
    brain_log = os.path.join(WORK, "brain.log")
    try:
        subprocess.run([f"{BUILD}/services/brain/kn_brain", "--migrate-only"], cwd=ROOT, env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        brain = subprocess.Popen([f"{BUILD}/services/brain/kn_brain", "--brain.host=127.0.0.1", "--brain.port=0", "--accounts.signup_rewards.enabled=false",
                                  "--brain.region=local", "--brain.id=emaillogin-%d" % os.getpid(),
                                  "--finder.poll_interval_s=1", "--accounts.email.required=false", "--smtp.host="],
                                 cwd=ROOT, env=env, stdout=open(brain_log, "w"), stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL, start_new_session=True)
        procs.append(brain)
        m = wait_for(lambda: re.search(r"listening on http://127\.0\.0\.1:(\d+) \(ws", read_log(brain_log)), 15)
        check("kn_brain starts", bool(m), read_log(brain_log)[-300:])
        if not m:
            return 1
        port = int(m.group(1))
        node(port)
        listed = wait_for(lambda: len(http(port, "/servers")[1].get("servers", [])) == 1, 20)
        check("the node is in the server finder", listed, str(http(port, "/servers")[1])[:300])
        ui_case(port, db_path)
        http_case(port)
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
    if all(results):
        shutil.rmtree(WORK, ignore_errors=True)
    else:
        print("logs kept in " + WORK)
    print(f"{sum(results)}/{len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
